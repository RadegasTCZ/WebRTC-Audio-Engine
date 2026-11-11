// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoiceProcessor.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include "AecConfig.h"
#include "api/audio/audio_processing.h"
#include "api/audio/echo_canceller3_factory.h"
#include "api/audio/echo_control.h"
#include "api/environment/environment_factory.h"
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/ns/noise_suppressor.h"
#include "speex/speex_resampler.h"

static_assert(VoiceProcessor::kAecLatency % 3 == 0, "AEC stand-in delays each 16 kHz band by kAecLatency / 3");

// ------------------------------------------------------------------ AEC3 instance

struct VoiceProcessor::AecInstance
{
    explicit AecInstance(unsigned strength)
    {
        const auto env = webrtc::CreateEnvironment();
        webrtc::EchoCanceller3Factory factory(aecconfig::Build(aecconfig::ForStrength(strength)));
        controller = factory.Create(env, kSampleRate, 1, 1);
    }
    std::unique_ptr<webrtc::EchoControl> controller;
};

// ------------------------------------------------------------------ live-processor registry
// Only touched on non-RT threads (Prepare/Deactivate/destructor and the settings watcher).

namespace
{
    SRWLOCK g_registryLock = SRWLOCK_INIT;
    std::vector<VoiceProcessor*>* g_registry = nullptr;  // leaked on purpose (process lifetime)

    AgcParams ParamsFrom(const Settings& s) noexcept
    {
        AgcParams p;
        p.headroomDb = static_cast<float>(s.agcHeadroomDb);
        p.maxGainDb = static_cast<float>(s.agcMaxGainDb);
        p.adaptSpeedDbPerSec = static_cast<float>(s.agcSpeedDbPerSec);
        p.noiseCeilingDbfs = static_cast<float>(s.agcNoiseCeilingDbfs);
        return p.Sanitised();
    }

    struct HookInstaller
    {
        HookInstaller() { settings::SetChangeHook(&VoiceProcessor::OnSettingsChanged); }
    } g_hookInstaller;


    // AudioBuffer::CopyFrom/CopyTo clamp to +-1.0 (FloatToFloatS16 / FloatS16ToFloat). Our chain may run
    // above full scale before the limiter (e.g. +30 dB slider gain), so load/store the int16-range data
    // directly, without clamping. Mono, same rate: no resampling or downmixing is involved.
    void Load(webrtc::AudioBuffer& buf, const float* src) noexcept
    {
        float* d = buf.channels()[0];
        for (int i = 0; i < VoiceProcessor::kBlock; ++i) d[i] = src[i] * 32768.0f;
    }
    void Store(const webrtc::AudioBuffer& buf, float* dst) noexcept
    {
        const float* s = buf.channels_const()[0];
        for (int i = 0; i < VoiceProcessor::kBlock; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
    }

    void CopyBands(const webrtc::AudioBuffer& from, webrtc::AudioBuffer& to) noexcept
    {
        const float* const* a = from.split_bands_const(0);
        float* const* b = to.split_bands(0);
        for (int band = 0; band < VoiceProcessor::kBands; ++band)
            std::memcpy(b[band], a[band], sizeof(float) * VoiceProcessor::kBandSize);
    }
}

void VoiceProcessor::Register()
{
    AcquireSRWLockExclusive(&g_registryLock);
    if (!g_registry) g_registry = new std::vector<VoiceProcessor*>();
    g_registry->push_back(this);
    m_registered = true;
    ReleaseSRWLockExclusive(&g_registryLock);
}

void VoiceProcessor::Unregister()
{
    if (!m_registered) return;
    AcquireSRWLockExclusive(&g_registryLock);
    std::erase(*g_registry, this);
    m_registered = false;
    ReleaseSRWLockExclusive(&g_registryLock);
}

void VoiceProcessor::OnSettingsChanged(const Settings& s)
{
    AcquireSRWLockExclusive(&g_registryLock);  // held while rebuilding, so no processor can be destroyed meanwhile
    if (g_registry)
        for (VoiceProcessor* p : *g_registry)
            p->RequestStageParams(s);
    ReleaseSRWLockExclusive(&g_registryLock);
}

void VoiceProcessor::RequestStageParams(const Settings& s)
{
    const AgcParams want = ParamsFrom(s);
    if (!(want == m_agcRequested))
    {
        delete m_agcRetired.exchange(nullptr);  // the stage the RT thread swapped out last time
        auto* fresh = new AgcStage(want, m_agcGainDb.load(std::memory_order_relaxed));  // keep the learnt gain
        delete m_agcPending.exchange(fresh);    // an older request the RT thread never picked up
        m_agcRequested = want;
    }
    if (m_aec && s.aecStrength != m_aecStrengthRequested)
    {
        delete m_aecRetired.exchange(nullptr);
        delete m_aecPending.exchange(new AecInstance(s.aecStrength));
        m_aecStrengthRequested = s.aecStrength;
    }
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
    std::unique_ptr<SpeexResamplerState, SpeexDeleter> up, down;
    std::vector<float> buf48, bufDev, ring;
    size_t ringFrames = 0, readPos = 0, fill = 0;
    int prefill = 0;

    void Push(const float* src, size_t frames, int ch) noexcept
    {
        for (size_t f = 0; f < frames && fill < ringFrames; ++f, ++fill)
            std::memcpy(&ring[((readPos + fill) % ringFrames) * ch], &src[f * ch], sizeof(float) * ch);
    }
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
    void Reset() noexcept
    {
        speex_resampler_reset_mem(up.get());
        speex_resampler_reset_mem(down.get());
        std::fill(ring.begin(), ring.end(), 0.0f);
        readPos = 0;
        fill = static_cast<size_t>(prefill);
    }
};

// ------------------------------------------------------------------ small helpers

float VoiceProcessor::DbToLinear(float db) noexcept
{
    return std::pow(10.0f, std::clamp(db, -60.0f, 40.0f) / 20.0f);
}

void VoiceProcessor::Biquad::SetHighPass(float hz, float sampleRate) noexcept
{
    // RBJ high-pass, Q = 1/sqrt(2) (2nd-order Butterworth): alpha = sin(w0) / (2Q) = sin(w0) / sqrt(2).
    const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosw = std::cos(w0), alpha = std::sin(w0) / std::numbers::sqrt2;
    const double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 + cosw) / 2.0 / a0);
    b1 = static_cast<float>(-(1.0 + cosw) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * cosw / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
}

void VoiceProcessor::Biquad::SetLowPass(float hz, float sampleRate) noexcept
{
    // 1st-order low-pass (6 dB/octave), bilinear transform with pre-warping: -3 dB exactly at `hz`.
    // Deliberately gentle: it only takes the piercing edge off bright laptop mics.
    const double k = std::tan(std::numbers::pi * hz / sampleRate);
    b0 = static_cast<float>(k / (1.0 + k));
    b1 = b0;
    b2 = 0.0f;
    a1 = static_cast<float>((k - 1.0) / (k + 1.0));
    a2 = 0.0f;
}

float VoiceProcessor::Biquad::Run(float x) noexcept
{
    const float y = b0 * x + z1;  // transposed direct form II
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void VoiceProcessor::VarDelay::Run(const float* in, float* out, int n) noexcept
{
    if (buf.empty())
    {
        if (in != out) std::memcpy(out, in, sizeof(float) * n);
        return;
    }
    const size_t len = buf.size();
    for (int i = 0; i < n; ++i)
    {
        const float x = in[i];
        out[i] = buf[pos];
        buf[pos] = x;
        if (++pos == len) pos = 0;
    }
}

// ------------------------------------------------------------------ lifecycle

VoiceProcessor::VoiceProcessor() = default;

VoiceProcessor::~VoiceProcessor()
{
    Unregister();
    delete m_agcPending.exchange(nullptr);
    delete m_agcRetired.exchange(nullptr);
    delete m_aecPending.exchange(nullptr);
    delete m_aecRetired.exchange(nullptr);
}

void VoiceProcessor::Deactivate() noexcept
{
    m_active = false;
    Unregister();
}

bool VoiceProcessor::Prepare(int channels, int sampleRate, const Settings& initial, int maxFrames)
{
    Deactivate();
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
        c.dry.Init(kAlgorithmLatency);

    auto newBuffer = [] { return std::make_unique<webrtc::AudioBuffer>(kSampleRate, 1, kSampleRate, 1, kSampleRate, 1); };
    m_cap = newBuffer();
    m_render = newBuffer();
    for (int l = 0; l < kNsLevels; ++l)
    {
        m_nsBuf[l] = newBuffer();
        webrtc::NsConfig c;
        c.target_level = static_cast<webrtc::NsConfig::SuppressionLevel>(l);
        m_ns[l] = std::make_unique<webrtc::NoiseSuppressor>(c, static_cast<size_t>(kSampleRate), static_cast<size_t>(1));
    }
    for (auto& d : m_aecStandIn)
        d.Init(kAecLatency / 3);
    m_nsDry.Init(kNsLatency);

    delete m_aecPending.exchange(nullptr);
    delete m_aecRetired.exchange(nullptr);
    m_aecStrengthRequested = initial.aecStrength;
    m_aec = m_refRing ? std::make_unique<AecInstance>(initial.aecStrength) : nullptr;
    m_refUnderruns = 0;

    if (!m_limiter)
        m_limiter = std::make_unique<OutputLimiter>();
    delete m_agcPending.exchange(nullptr);
    delete m_agcRetired.exchange(nullptr);
    m_agcRequested = ParamsFrom(initial);
    m_agc = std::make_unique<AgcStage>(m_agcRequested);
    m_agcGainDb.store(m_agc->LastGainDb());

    m_settings = initial;
    m_level = static_cast<int>(initial.nsLevel);
    m_hpfHzApplied = m_lpfHzApplied = 0;
    Reset();

    if (m_rs)
    {
        const double coreDev = static_cast<double>(kTotalLatency) * sampleRate / kSampleRate;
        const int upOut = speex_resampler_get_output_latency(m_rs->up.get());
        const int downIn = speex_resampler_get_input_latency(m_rs->down.get());
        const double rsDev = static_cast<double>(upOut + downIn) * sampleRate / kSampleRate;
        m_latencyFrames = static_cast<int>(std::lround(coreDev + rsDev)) + m_rs->prefill;
    }
    else
        m_latencyFrames = kTotalLatency;
    m_active = true;
    Register();
    return true;
}

void VoiceProcessor::Reset() noexcept
{
    for (auto& c : m_ch)
    {
        c.dry.Clear();
        c.in.fill(0.0f);
        c.out.fill(0.0f);
    }
    m_hpf.z1 = m_hpf.z2 = 0;
    m_lpf.z1 = m_lpf.z2 = 0;
    for (auto& d : m_aecStandIn) d.Clear();
    m_nsDry.Clear();
    if (m_limiter) m_limiter->Reset();
    m_pos = 0;
    m_nextLevel = -1;
    m_underruns = 0;
    if (m_rs) m_rs->Reset();
    // Start directly in the requested state (no fade-in from silence).
    const float gainDb = m_settings.agcEnabled ? 0.0f : m_gainTargetDb;
    m_gain.current = m_gain.target = DbToLinear(gainDb);
    m_hpfMix.current = m_hpfMix.target = m_settings.hpfEnabled ? 1.0f : 0.0f;
    m_lpfMix.current = m_lpfMix.target = m_settings.lpfEnabled ? 1.0f : 0.0f;
    m_nsMix.current = m_nsMix.target = m_settings.nsEnabled ? 1.0f : 0.0f;
    m_agcMix.current = m_agcMix.target = m_settings.agcEnabled ? 1.0f : 0.0f;
    m_limMix.current = m_limMix.target = m_settings.limiterEnabled ? 1.0f : 0.0f;
    m_wet.current = m_wet.target = m_settings.bypass ? 0.0f : 1.0f;
    ApplySettings(m_settings);
}

void VoiceProcessor::ApplySettings(const Settings& s) noexcept
{
    m_settings = s;
    if (s.hpfHz != m_hpfHzApplied)
    {
        m_hpf.SetHighPass(static_cast<float>(s.hpfHz), static_cast<float>(kSampleRate));
        m_hpfHzApplied = s.hpfHz;
    }
    if (s.lpfHz != m_lpfHzApplied)
    {
        m_lpf.SetLowPass(static_cast<float>(s.lpfHz), static_cast<float>(kSampleRate));
        m_lpfHzApplied = s.lpfHz;
    }
    m_hpfMix.target = s.hpfEnabled ? 1.0f : 0.0f;
    m_lpfMix.target = s.lpfEnabled ? 1.0f : 0.0f;
    m_nsMix.target = s.nsEnabled ? 1.0f : 0.0f;
    m_agcMix.target = s.agcEnabled ? 1.0f : 0.0f;
    m_limMix.target = s.limiterEnabled ? 1.0f : 0.0f;
    m_wet.target = s.bypass ? 0.0f : 1.0f;
    m_gain.target = DbToLinear(s.agcEnabled ? 0.0f : m_gainTargetDb);  // AGC2 owns the level when on
    if (static_cast<int>(s.nsLevel) != m_level && m_nextLevel < 0)
        m_nextLevel = static_cast<int>(s.nsLevel);
}

// ------------------------------------------------------------------ processing

void VoiceProcessor::ProcessBlock() noexcept
{
    // Pick up stages rebuilt by the settings watcher. The old ones go back through the "retired"
    // slots for freeing on a non-RT thread (the watcher empties those before every rebuild).
    if (AgcStage* fresh = m_agcPending.exchange(nullptr))
    {
        AgcStage* expected = nullptr;
        m_agcRetired.compare_exchange_strong(expected, m_agc.release());
        m_agc.reset(fresh);
    }
    if (AecInstance* fresh = m_aecPending.exchange(nullptr))
    {
        AecInstance* expected = nullptr;
        m_aecRetired.compare_exchange_strong(expected, m_aec.release());
        m_aec.reset(fresh);
    }

    const float invCh = 1.0f / static_cast<float>(m_channels);
    m_mono.fill(0.0f);
    for (auto& c : m_ch)
    {
        // Sanitize, apply the input gain (also in bypass: it is the volume control), downmix.
        for (int i = 0; i < kBlock; ++i)
        {
            const float x = c.in[i];
            c.in[i] = (std::isfinite(x) ? x : 0.0f) * m_gain.At(i);
            m_mono[i] += c.in[i] * invCh;
        }
        c.dry.Run(c.in.data(), c.delayed.data(), kBlock);  // latency-matched original channel (bypass)
    }

    // 1. Cleanup filters: high-pass ("rumble") then gentle low-pass ("hiss"); both always run and
    //    are mixed in by their on/off ramps.
    for (int i = 0; i < kBlock; ++i)
    {
        const float h = m_hpf.Run(m_mono[i]);
        const float x = m_mono[i] + m_hpfMix.At(i) * (h - m_mono[i]);
        const float l = m_lpf.Run(x);
        m_s1[i] = x + m_lpfMix.At(i) * (l - x);
    }

    // 2. Echo reference for this block (silence when AEC is off or the loopback ran dry).
    if (m_aec)
    {
        bool have = false;
        if (m_refRing)
        {
            m_refRing->TrimTo(4 * kBlock);  // bound the reference queue (AEC3 tracks the remaining delay)
            have = m_refRing->Read(m_ref.data(), kBlock);
        }
        if (!have) { m_ref.fill(0.0f); ++m_refUnderruns; }
        if (!m_settings.aecEnabled) m_ref.fill(0.0f);
        Load(*m_render, m_ref.data());
        m_render->SplitIntoFrequencyBands();
        m_aec->controller->AnalyzeRender(m_render.get());
    }

    // 3. Capture in the band domain, WebRTC APM order: AEC analyse -> split -> NS analyse (pre-AEC)
    //    -> AEC process -> NS process.
    Load(*m_cap, m_s1.data());
    if (m_aec) m_aec->controller->AnalyzeCapture(m_cap.get());
    m_cap->SplitIntoFrequencyBands();
    for (int l = 0; l < kNsLevels; ++l)
    {
        CopyBands(*m_cap, *m_nsBuf[l]);
        m_ns[l]->Analyze(*m_nsBuf[l]);
    }
    if (m_aec)
    {
        m_aec->controller->SetAudioBufferDelay(0);
        m_aec->controller->ProcessCapture(m_cap.get(), false);
    }
    else
    {
        float* const* b = m_cap->split_bands(0);
        for (int band = 0; band < kBands; ++band)
            m_aecStandIn[band].Run(b[band], b[band], kBandSize);  // same delay as AEC3 would add
    }
    for (int l = 0; l < kNsLevels; ++l)
    {
        CopyBands(*m_cap, *m_nsBuf[l]);
        m_ns[l]->Process(m_nsBuf[l].get());  // all levels keep their state current
    }
    auto mergeTo = [](webrtc::AudioBuffer& buf, float* dst) {
        buf.MergeFrequencyBands();
        Store(buf, dst);
    };
    mergeTo(*m_nsBuf[m_level], m_nsOut.data());
    if (m_nextLevel >= 0)
    {
        mergeTo(*m_nsBuf[m_nextLevel], m_nsAlt.data());
        for (int i = 0; i < kBlock; ++i)
            m_nsOut[i] += (static_cast<float>(i + 1) / kBlock) * (m_nsAlt[i] - m_nsOut[i]);
    }
    mergeTo(*m_cap, m_postAec.data());                   // post-AEC, pre-NS (NS off path)
    m_nsDry.Run(m_postAec.data(), m_d1.data(), kBlock);  // aligned with the NS output
    for (int i = 0; i < kBlock; ++i)
        m_voice[i] = m_d1[i] + m_nsMix.At(i) * (m_nsOut[i] - m_d1[i]);

    // 4. AGC2 (always runs so its gain stays adapted; mixed in when enabled).
    std::memcpy(m_tmp.data(), m_voice.data(), sizeof(m_tmp));
    m_agc->Process(m_tmp.data());
    m_agcGainDb.store(m_agc->LastGainDb(), std::memory_order_relaxed);
    for (int i = 0; i < kBlock; ++i)
        m_voice[i] += m_agcMix.At(i) * (m_tmp[i] - m_voice[i]);

    // 5. Limiter (-1 dBFS), last.
    m_limiter->Process(m_voice.data(), m_tmp.data());
    m_limiterDb.store(m_limiter->LastReductionDb(), std::memory_order_relaxed);
    for (int i = 0; i < kBlock; ++i)
        m_voice[i] += m_limMix.At(i) * (m_tmp[i] - m_voice[i]);

    // Mono voice to every channel; master bypass crossfades to the delayed original channels.
    for (auto& c : m_ch)
        for (int i = 0; i < kBlock; ++i)
            c.out[i] = c.delayed[i] + m_wet.At(i) * (m_voice[i] - c.delayed[i]);

    if (m_nextLevel >= 0)
    {
        m_level = m_nextLevel;
        m_nextLevel = -1;
    }
    m_hpfMix.Finish();
    m_lpfMix.Finish();
    m_nsMix.Finish();
    m_agcMix.Finish();
    m_limMix.Finish();
    m_wet.Finish();
    m_gain.Finish();
}

void VoiceProcessor::Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb) noexcept
{
    if (inputGainDb != m_gainTargetDb)
    {
        m_gainTargetDb = inputGainDb;
        m_gain.target = DbToLinear(m_settings.agcEnabled ? 0.0f : inputGainDb);
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
    if (!rs.Pop(out, static_cast<size_t>(frames), ch))  // input fully consumed above: in-place is safe
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
