#include "VoiceProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include "modules/audio_processing/audio_buffer.h"  // third_party/webrtc/shim: minimal stand-in
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "modules/audio_processing/three_band_filter_bank.h"

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

void VoiceProcessor::Delay::Run(const float* in, float* out) noexcept
{
    constexpr int L = kAlgorithmLatency;
    std::memcpy(out, hist.data(), L * sizeof(float));
    std::memcpy(out + L, in, (kBlock - L) * sizeof(float));
    std::memcpy(hist.data(), in + (kBlock - L), L * sizeof(float));
}

// ------------------------------------------------------------------ lifecycle

VoiceProcessor::VoiceProcessor() = default;
VoiceProcessor::~VoiceProcessor() = default;

bool VoiceProcessor::Prepare(int channels, int sampleRate, const Settings& initial)
{
    m_active = false;
    m_ch.clear();
    if (sampleRate != kSampleRate || channels < 1 || channels > kMaxChannels)
        return false;

    m_channels = channels;
    m_ch.resize(static_cast<size_t>(channels));
    for (auto& c : m_ch)
        for (int l = 0; l < kNsLevels; ++l)
            c.ns[l] = std::make_unique<NsChain>(l);

    m_settings = initial;
    m_level = static_cast<int>(initial.nsLevel);
    m_hpfHzApplied = 0;
    Reset();
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
    // Start directly in the requested state (no fade-in from silence).
    m_hpfMix.current = m_hpfMix.target = m_settings.hpfEnabled ? 1.0f : 0.0f;
    m_nsMix.current = m_nsMix.target = m_settings.nsEnabled ? 1.0f : 0.0f;
    m_wet.current = m_wet.target = m_settings.bypass ? 0.0f : 1.0f;
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
        // Sanitize once at the input: nothing non-finite enters any filter state.
        for (float& x : c.in)
            if (!std::isfinite(x)) x = 0.0f;

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
}

void VoiceProcessor::Process(const float* in, float* out, int frames, const Settings& s) noexcept
{
    if (!m_active)
    {
        if (in != out)
            std::memcpy(out, in, static_cast<size_t>(frames) * static_cast<size_t>(m_channels > 0 ? m_channels : 1) * sizeof(float));
        return;
    }
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
