#include "VoiceProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include "modules/audio_processing/audio_buffer.h"  // third_party/webrtc/shim: minimal stand-in
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "modules/audio_processing/three_band_filter_bank.h"
#include "speex/speex_resampler.h"

static_assert(webrtc::ThreeBandFilterBank::kFullBandSize == VoiceProcessor::kBlock, "one block = one filter-bank frame");
static_assert(webrtc::ThreeBandFilterBank::kSplitBandSize == 160 && webrtc::kNsFrameSize == 160, "16 kHz bands of 10 ms");
static_assert(VoiceProcessor::kAlgorithmLatency <= VoiceProcessor::kBlock, "Delay assumes latency <= block");

namespace
{
    constexpr int kBands = webrtc::ThreeBandFilterBank::kNumBands;
    constexpr int kBandSize = webrtc::ThreeBandFilterBank::kSplitBandSize;
}

// ------------------------------------------------------------------ NS chain (one level, one channel)

struct VoiceProcessor::NsChain
{
    explicit NsChain(int level)
    {
        webrtc::NsConfig config;
        config.target_level = static_cast<webrtc::NsConfig::SuppressionLevel>(level);
        ns = std::make_unique<webrtc::NoiseSuppressor>(config, static_cast<size_t>(kSampleRate), static_cast<size_t>(1));
        for (int b = 0; b < kBands; ++b)
        {
            bandPtrs[b] = bands[b].data();
            bandViews[b] = std::span<float>(bands[b]);
        }
    }

    void Reset() noexcept
    {
        bank = webrtc::ThreeBandFilterBank();  // fixed-size arrays: no allocation
        ns->Reset();                           // local patch, no allocation
    }

    // in/out: 480 samples in [-1, 1] float scale.
    void Run(const float* in, float* out) noexcept
    {
        for (int i = 0; i < kBlock; ++i)
            scaled[i] = in[i] * 32768.0f;  // NS works in the int16 range
        bank.Analysis(scaled, bandViews);
        webrtc::AudioBuffer buffer(bandPtrs.data(), static_cast<size_t>(kBands), static_cast<size_t>(kBandSize));
        ns->Analyze(buffer);
        ns->Process(&buffer);
        bank.Synthesis(bandViews, synthesis);
        for (int i = 0; i < kBlock; ++i)
            out[i] = synthesis[i] * (1.0f / 32768.0f);
    }

    webrtc::ThreeBandFilterBank bank;
    std::unique_ptr<webrtc::NoiseSuppressor> ns;
    std::array<std::array<float, kBandSize>, kBands> bands{};
    std::array<float*, kBands> bandPtrs{};
    std::array<std::span<float>, kBands> bandViews{};
    std::array<float, kBlock> scaled{};
    std::array<float, kBlock> synthesis{};
};

// ------------------------------------------------------------------ small helpers

void VoiceProcessor::Biquad::SetHighPass(float hz, float sampleRate) noexcept
{
    // RBJ cookbook high-pass, Q = 1/sqrt(2) (2nd-order Butterworth).
    const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
    // alpha = sin(w0) / (2Q); with Q = 1/sqrt(2) that is sin(w0) / sqrt(2).
    const double cosw = std::cos(w0), alpha = std::sin(w0) / std::numbers::sqrt2;
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 + cosw) / 2.0 / a0);
    b1 = static_cast<float>(-(1.0 + cosw) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * cosw / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

float VoiceProcessor::Biquad::Run(float x) noexcept
{
    // Transposed direct form II.
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

float VoiceProcessor::DbToLinear(float db) noexcept
{
    return std::pow(10.0f, std::clamp(db, -60.0f, 40.0f) / 20.0f);
}

void VoiceProcessor::Delay::Run(const float* in, float* out) noexcept
{
    constexpr int L = kAlgorithmLatency;
    std::memcpy(out, hist.data(), L * sizeof(float));
    std::memcpy(out + L, in, (kBlock - L) * sizeof(float));
    std::memcpy(hist.data(), in + (kBlock - L), L * sizeof(float));
}

// ------------------------------------------------------------------ resampling (non-48 kHz devices)

namespace
{
    constexpr int kResampleQuality = 5;  // same as the AEC3 plugin
    constexpr int kRingMargin = 16;      // frames of slack for +-1 output-count jitter of the resampler
    struct SpeexDeleter { void operator()(SpeexResamplerState* s) const noexcept { if (s) speex_resampler_destroy(s); } };
}

struct VoiceProcessor::Resampling
{
    std::unique_ptr<SpeexResamplerState, SpeexDeleter> up, down;   // device rate <-> 48 kHz
    std::vector<float> buf48;   // interleaved 48 kHz scratch, sized for maxFrames
    std::vector<float> bufDev;  // interleaved device-rate scratch after down-sampling
    std::vector<float> ring;    // interleaved device-rate output FIFO
    size_t ringFrames = 0, readPos = 0, fill = 0;
    int prefill = 0;

    void Push(const float* src, size_t frames, int ch) noexcept
    {
        for (size_t f = 0; f < frames && fill < ringFrames; ++f, ++fill)
        {
            const size_t w = (readPos + fill) % ringFrames;
            std::memcpy(&ring[w * ch], &src[f * ch], sizeof(float) * ch);
        }
    }
    // Returns false on underrun (missing frames are filled with silence).
    bool Pop(float* dst, size_t frames, int ch) noexcept
    {
        bool ok = true;
        for (size_t f = 0; f < frames; ++f)
        {
            if (fill == 0) { std::memset(&dst[f * ch], 0, sizeof(float) * ch); ok = false; continue; }
            std::memcpy(&dst[f * ch], &ring[readPos * ch], sizeof(float) * ch);
            readPos = (readPos + 1) % ringFrames;
            --fill;
        }
        return ok;
    }
    void Reset(int ch) noexcept
    {
        speex_resampler_reset_mem(up.get());
        speex_resampler_reset_mem(down.get());
        std::fill(ring.begin(), ring.end(), 0.0f);
        readPos = 0;
        fill = static_cast<size_t>(prefill);  // constant latency: start with `prefill` frames of silence
        (void)ch;
    }
};

// ------------------------------------------------------------------ lifecycle

VoiceProcessor::VoiceProcessor() = default;
VoiceProcessor::~VoiceProcessor() = default;

bool VoiceProcessor::Prepare(int channels, int sampleRate, const Settings& initial, int maxFrames)
{
    m_active = false;
    m_ch.clear();
    m_rs.reset();
    if (sampleRate < kMinRate || sampleRate > kMaxRate || channels < 1 || channels > kMaxChannels || maxFrames < 1)
        return false;

    m_rate = sampleRate;
    if (sampleRate != kSampleRate)
    {
        auto rs = std::make_unique<Resampling>();
        int err = 0;
        rs->up.reset(speex_resampler_init(static_cast<spx_uint32_t>(channels), static_cast<spx_uint32_t>(sampleRate), kSampleRate, kResampleQuality, &err));
        rs->down.reset(speex_resampler_init(static_cast<spx_uint32_t>(channels), kSampleRate, static_cast<spx_uint32_t>(sampleRate), kResampleQuality, &err));
        if (!rs->up || !rs->down)
            return false;
        const size_t max48 = static_cast<size_t>(std::ceil(static_cast<double>(maxFrames) * kSampleRate / sampleRate)) + 64;
        rs->buf48.assign(max48 * channels, 0.0f);
        rs->bufDev.assign((static_cast<size_t>(maxFrames) + 64) * channels, 0.0f);
        rs->ringFrames = static_cast<size_t>(maxFrames) * 2 + 256;
        rs->ring.assign(rs->ringFrames * channels, 0.0f);
        rs->prefill = kRingMargin;
        m_rs = std::move(rs);
    }

    m_channels = channels;
    m_ch.resize(static_cast<size_t>(channels));
    for (auto& c : m_ch)
        for (int l = 0; l < kNsLevels; ++l)
            c.ns[l] = std::make_unique<NsChain>(l);

    m_settings = initial;
    m_level = static_cast<int>(initial.nsLevel);
    m_hpfHzApplied = 0;
    Reset();

    if (m_rs)
    {
        // Up- and down-sampler group delays plus the core's 48 kHz latency, all at the device rate.
        const double coreDev = static_cast<double>(kTotalLatency) * sampleRate / kSampleRate;
        const int upOut = speex_resampler_get_output_latency(m_rs->up.get());     // at 48 kHz
        const int downIn = speex_resampler_get_input_latency(m_rs->down.get());   // at 48 kHz
        const double rsDev = static_cast<double>(upOut + downIn) * sampleRate / kSampleRate;
        m_latencyFrames = static_cast<int>(std::lround(coreDev + rsDev)) + m_rs->prefill;
    }
    else
        m_latencyFrames = kTotalLatency;
    m_active = true;
    return true;
}

void VoiceProcessor::Reset() noexcept
{
    for (auto& c : m_ch)
    {
        c.hpf.z1 = c.hpf.z2 = 0;
        for (auto& n : c.ns) n->Reset();
        c.dryAfterHpf.hist.fill(0.0f);
        c.dryInput.hist.fill(0.0f);
        c.in.fill(0.0f);
        c.out.fill(0.0f);
    }
    m_pos = 0;
    m_nextLevel = -1;
    m_underruns = 0;
    if (m_rs)
        m_rs->Reset(m_channels);
    // Start directly in the requested state (no fade-in from silence).
    m_hpfMix.current = m_hpfMix.target = m_settings.hpfEnabled ? 1.0f : 0.0f;
    m_nsMix.current = m_nsMix.target = m_settings.nsEnabled ? 1.0f : 0.0f;
    m_wet.current = m_wet.target = m_settings.bypass ? 0.0f : 1.0f;
    m_gain.current = m_gain.target = DbToLinear(m_gainTargetDb);
    ApplySettings(m_settings);
}

void VoiceProcessor::ApplySettings(const Settings& s) noexcept
{
    m_settings = s;
    if (s.hpfHz != m_hpfHzApplied)
    {
        for (auto& c : m_ch)
            c.hpf.SetHighPass(static_cast<float>(s.hpfHz), static_cast<float>(kSampleRate));
        m_hpfHzApplied = s.hpfHz;
    }
    m_hpfMix.target = s.hpfEnabled ? 1.0f : 0.0f;
    m_nsMix.target = s.nsEnabled ? 1.0f : 0.0f;
    m_wet.target = s.bypass ? 0.0f : 1.0f;
    if (static_cast<int>(s.nsLevel) != m_level && m_nextLevel < 0)
    {
        m_nextLevel = static_cast<int>(s.nsLevel);
        for (auto& c : m_ch)
            c.ns[m_nextLevel]->Reset();  // fresh start; re-adapts in ~0.5 s like the plugin
    }
}

// ------------------------------------------------------------------ processing

void VoiceProcessor::ProcessBlock() noexcept
{
    for (auto& c : m_ch)
    {
        // Sanitize once at the input: nothing non-finite enters any filter state. Then apply the
        // input gain (mic volume slider) first, so it also applies when bypassed: bypass means
        // "no processing", not "ignore the volume control".
        for (int i = 0; i < kBlock; ++i)
        {
            const float x = c.in[i];
            c.in[i] = (std::isfinite(x) ? x : 0.0f) * m_gain.At(i);
        }

        // 1. High-pass filter (always runs; mixed by its on/off ramp).
        for (int i = 0; i < kBlock; ++i)
        {
            const float y = c.hpf.Run(c.in[i]);
            const float g = m_hpfMix.At(i);
            c.s1[i] = c.in[i] + g * (y - c.in[i]);
        }

        // 2. Noise suppression (+ level crossfade).
        c.ns[m_level]->Run(c.s1.data(), c.nsA.data());
        if (m_nextLevel >= 0)
        {
            c.ns[m_nextLevel]->Run(c.s1.data(), c.nsB.data());
            for (int i = 0; i < kBlock; ++i)
            {
                const float t = static_cast<float>(i + 1) / kBlock;
                c.nsA[i] += t * (c.nsB[i] - c.nsA[i]);
            }
        }

        // Latency-matched dry paths.
        c.dryAfterHpf.Run(c.s1.data(), c.d1.data());
        c.dryInput.Run(c.in.data(), c.d0.data());

        for (int i = 0; i < kBlock; ++i)
        {
            const float chain = c.d1[i] + m_nsMix.At(i) * (c.nsA[i] - c.d1[i]);
            c.out[i] = c.d0[i] + m_wet.At(i) * (chain - c.d0[i]);
        }
    }
    if (m_nextLevel >= 0)
    {
        m_level = m_nextLevel;
        m_nextLevel = -1;
    }
    m_hpfMix.Finish();
    m_nsMix.Finish();
    m_wet.Finish();
    m_gain.Finish();
}

void VoiceProcessor::Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb) noexcept
{
    if (inputGainDb != m_gainTargetDb)
    {
        m_gainTargetDb = inputGainDb;
        m_gain.target = DbToLinear(inputGainDb);  // picked up by the next block's ramp
    }
    if (!m_active)
    {
        if (in != out)
            std::memcpy(out, in, static_cast<size_t>(frames) * static_cast<size_t>(m_channels > 0 ? m_channels : 1) * sizeof(float));
        return;
    }
    if (!m_rs)
    {
        ProcessCore(in, out, frames, s);
        return;
    }

    // Device rate -> 48 kHz -> core -> device rate -> FIFO with fixed prefill.
    // speex_resampler_process_interleaved_float consumes all input when the output buffer is large
    // enough (sized from maxFrames in Prepare). Loop defensively in case it stops early.
    const int ch = m_channels;
    Resampling& rs = *m_rs;
    spx_uint32_t inLeft = static_cast<spx_uint32_t>(frames);
    const float* src = in;
    while (inLeft > 0)
    {
        spx_uint32_t inLen = inLeft;
        spx_uint32_t out48 = static_cast<spx_uint32_t>(rs.buf48.size() / ch);
        speex_resampler_process_interleaved_float(rs.up.get(), src, &inLen, rs.buf48.data(), &out48);
        if (inLen == 0 && out48 == 0)
            break;
        ProcessCore(rs.buf48.data(), rs.buf48.data(), static_cast<int>(out48), s);

        spx_uint32_t in48 = out48;
        const float* p48 = rs.buf48.data();
        while (in48 > 0)
        {
            spx_uint32_t len = in48;
            spx_uint32_t outDev = static_cast<spx_uint32_t>(rs.bufDev.size() / ch);
            speex_resampler_process_interleaved_float(rs.down.get(), p48, &len, rs.bufDev.data(), &outDev);
            rs.Push(rs.bufDev.data(), outDev, ch);
            if (len == 0 && outDev == 0)
                break;
            p48 += static_cast<size_t>(len) * ch;
            in48 -= len;
        }
        src += static_cast<size_t>(inLen) * ch;
        inLeft -= inLen;
    }
    // Read before this point means `in` is fully consumed, so writing `out` in place is safe.
    if (!rs.Pop(out, static_cast<size_t>(frames), ch))
        ++m_underruns;
}

void VoiceProcessor::ProcessCore(const float* in, float* out, int frames, const Settings& s) noexcept
{
    const int ch = m_channels;
    for (int f = 0; f < frames; ++f)
    {
        const size_t base = static_cast<size_t>(f) * static_cast<size_t>(ch);
        for (int c = 0; c < ch; ++c)
        {
            auto& st = m_ch[static_cast<size_t>(c)];
            const float x = in[base + c];  // read before write: safe in-place
            st.in[static_cast<size_t>(m_pos)] = x;
            out[base + c] = st.out[static_cast<size_t>(m_pos)];
        }
        if (++m_pos == kBlock)
        {
            if (!(s == m_settings))
                ApplySettings(s);
            ProcessBlock();
            m_pos = 0;
        }
    }
}
