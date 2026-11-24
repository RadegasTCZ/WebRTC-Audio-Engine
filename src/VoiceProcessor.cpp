// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
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
static_assert(VoiceProcessor::kPadLatency >= EchoTimingController::kMaxDelaySamples, "the mic delay must fit the latency budget");

// ------------------------------------------------------------------ AEC3 instance

struct VoiceProcessor::AecInstance
{
    AecInstance(unsigned strength, unsigned room)
    {
        const auto env = webrtc::CreateEnvironment();
        webrtc::EchoCanceller3Factory factory(aecconfig::Build(aecconfig::ForStrength(strength), aecconfig::ForRoom(room)));
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


    // The AGC's learnt gain is carried into rebuilt stages: never carry a NaN along.
    float FiniteOr0(float v) noexcept { return std::isfinite(v) ? v : 0.0f; }

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
    try
    {
        if (!g_registry) g_registry = new std::vector<VoiceProcessor*>();
        g_registry->push_back(this);
        m_registered = true;
        // A settings change that landed between the caller's snapshot and now was applied to the other
        // processors only: catch up (a no-op when nothing changed). Only with the live watcher running:
        // otherwise nothing can change, and the caller's settings (e.g. a test's) are the truth.
        // Best effort: out of memory here keeps the stages the caller built, it must not fail the stream.
        if (settings::WatcherStarted())
            try { RequestStageParams(settings::Current(m_settingsIndex)); }
            catch (...) {}
    }
    catch (...)  // out of memory: never leave the lock held (every other stream would hang on it)
    {
        ReleaseSRWLockExclusive(&g_registryLock);
        throw;
    }
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

// Optional extra hook (set by the APO DLL): runs after the stage rebuilds, e.g. to notify Windows Settings.
void (*g_afterSettingsChanged)() noexcept = nullptr;

void VoiceProcessor::OnSettingsChanged(const Settings& s)
{
    AcquireSRWLockExclusive(&g_registryLock);  // held while rebuilding, so no processor can be destroyed meanwhile
    if (g_registry)
        for (VoiceProcessor* p : *g_registry)
        {
            // Each processor follows its microphone's settings. Out of memory for a new stage: that processor
            // keeps its current stages (the lock must be released either way).
            try { p->RequestStageParams(settings::Current(p->m_settingsIndex)); }
            catch (...) {}
        }
    (void)s;
    ReleaseSRWLockExclusive(&g_registryLock);
    if (g_afterSettingsChanged)
        g_afterSettingsChanged();
}

void VoiceProcessor::RequestStageParams(const Settings& s)
{
    const AgcParams want = ParamsFrom(s);
    if (!(want == m_agcRequested))
    {
        m_agcHandoff.Offer(new AgcStage(want, m_agcGainDb.load(std::memory_order_relaxed)));  // keep the learnt gain
        m_agcRequested = want;
    }
    if (m_hasAec.load() && (s.aecStrength != m_aecStrengthRequested || s.aecRoom != m_aecRoomRequested))
    {
        m_aecHandoff.Offer(new AecInstance(s.aecStrength, s.aecRoom));
        m_aecStrengthRequested = s.aecStrength;
        m_aecRoomRequested = s.aecRoom;
    }
}

std::vector<float> VoiceProcessor::TakeDump()
{
    std::vector<float> out;
    if (m_dump.empty())
        return out;
    if (m_dumpFull)
    {
        out.assign(m_dump.begin() + static_cast<std::ptrdiff_t>(m_dumpPos), m_dump.end());
        out.insert(out.end(), m_dump.begin(), m_dump.begin() + static_cast<std::ptrdiff_t>(m_dumpPos));
    }
    else
        out.assign(m_dump.begin(), m_dump.begin() + static_cast<std::ptrdiff_t>(m_dumpPos));
    m_dump.clear();
    m_dump.shrink_to_fit();
    m_dumpPos = 0;
    m_dumpFull = false;
    return out;
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
    if (std::isnan(db)) return 1.0f;  // a broken value from the engine: unity, never NaN into the chain
    return std::pow(10.0f, std::clamp(db, -60.0f, 40.0f) / 20.0f);
}

// Input gain for the slider value: muted -> 0, AGC on -> unity (AGC2 owns the level), else the slider.
float VoiceProcessor::InputGain(float sliderDb, bool agcEnabled) noexcept
{
    if (sliderDb == kMutedDb) return 0.0f;
    return DbToLinear(agcEnabled ? 0.0f : sliderDb);
}

void VoiceProcessor::Biquad::SetPass2(bool highPass, double hz, double q, double sampleRate) noexcept
{
    // RBJ cookbook high/low-pass: alpha = sin(w0) / (2Q).
    const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    const double edge = highPass ? (1.0 + cosw) / 2.0 : (1.0 - cosw) / 2.0;
    b0 = static_cast<float>(edge / a0);
    b1 = static_cast<float>((highPass ? -2.0 : 2.0) * edge / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * cosw / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
    z1 = z2 = 0;
}

void VoiceProcessor::Biquad::SetPass1(bool highPass, double hz, double sampleRate) noexcept
{
    // 1st order (6 dB/octave), bilinear transform with pre-warping: -3 dB exactly at `hz`.
    const double k = std::tan(std::numbers::pi * hz / sampleRate);
    b0 = static_cast<float>((highPass ? 1.0 : k) / (1.0 + k));
    b1 = highPass ? -b0 : b0;
    b2 = 0.0f;
    a1 = static_cast<float>((k - 1.0) / (k + 1.0));
    a2 = 0.0f;
    z1 = z2 = 0;
}

void VoiceProcessor::Cascade::Design(bool highPass, float hz, int order, double sampleRate) noexcept
{
    // Butterworth: order 1 = one 1st-order section; 2 = one biquad, Q 1/sqrt(2);
    // 3 = a biquad with Q 1 plus a 1st-order section at the same corner (-3 dB at `hz` overall).
    if (order <= 1) { s[0].SetPass1(highPass, hz, sampleRate); n = 1; }
    else if (order == 2) { s[0].SetPass2(highPass, hz, 1.0 / std::numbers::sqrt2, sampleRate); n = 1; }
    else
    {
        s[0].SetPass2(highPass, hz, 1.0, sampleRate);
        s[1].SetPass1(highPass, hz, sampleRate);
        n = 2;
    }
}

float VoiceProcessor::Cascade::Run(float x) noexcept
{
    x = s[0].Run(x);
    return n > 1 ? s[1].Run(x) : x;
}

void VoiceProcessor::Cascade::Clear() noexcept
{
    for (auto& b : s) b.z1 = b.z2 = 0;
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

void VoiceProcessor::MicDelay::Run(float* x, int from, int to, int64_t rampStart) noexcept
{
    constexpr size_t mask = kSize - 1;
    for (int i = 0; i < kBlock; ++i) buf[(w + i) & mask] = x[i];
    if (from == to)
    {
        if (from != 0)
            for (int i = 0; i < kBlock; ++i) x[i] = buf[(w + i - static_cast<size_t>(from)) & mask];
    }
    else
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float a = buf[(w + i - static_cast<size_t>(from)) & mask], b = buf[(w + i - static_cast<size_t>(to)) & mask];
            const float t = std::clamp(static_cast<float>(i - rampStart + 1) / kBlock, 0.0f, 1.0f);
            x[i] = a + (b - a) * t;
        }
    }
    w += kBlock;
}

VoiceProcessor::EchoTimingEvent VoiceProcessor::EchoTimingTick() noexcept
{
    EchoTimingEvent ev;
    ev.delaySamples = m_timing.Delay();
    if (!m_active || !EchoTimingActive() || !m_estimator)
        return ev;
    if (!m_probe->Latest(m_t8mic.data(), m_t8ref.data(), EchoTimingEstimator::kSpan))
        return ev;
    ev.measured = true;
    ev.result = m_estimator->Estimate(m_t8mic.data(), m_t8ref.data());

    // Echo slide -> reference rate. Points only from clean spans: not while the delay controller settles,
    // and start over after any jump of the reference (resync / dry block) or of the delay.
    const uint64_t guard = m_sync.Resyncs() + m_sync.DryBlocks();
    if (guard != m_driftGuard) { m_drift.Clear(); m_driftGuard = guard; }
    if (ev.result.valid && !m_timing.Settling())
        m_drift.Add(static_cast<double>(m_probe->Written()) / EchoTimingProbe::kRate,
                    ev.result.offsetMs - static_cast<float>(m_timing.Delay()) / 48.0f);

    ev.decision = m_timing.OnResult(ev.result);
    if (ev.decision.change)
    {
        m_micDelayTarget.store(ev.decision.delaySamples, std::memory_order_relaxed);
        m_drift.Clear();
    }
    if (++m_driftTicks >= 10)   // every 10 s
    {
        m_driftTicks = 0;
        double slide = 0.0, span = 0.0;
        int points = 0;
        if (m_drift.Slope(slide, points, span) && points >= 20 && span >= 20.0 && std::fabs(slide) > 1.5)
        {
            // Echo comes ever earlier (slide < 0) = the reference content runs fast: read it faster.
            const double rate = std::clamp(m_sync.RatePpm() - 0.8 * slide, -ReferenceSync::kMaxRatio * 1e6, ReferenceSync::kMaxRatio * 1e6);
            m_sync.SetRatePpm(rate);
            m_drift.Clear();
            ev.rateChanged = true;
            ev.slidePpm = slide;
        }
    }
    ev.ratePpm = m_sync.RatePpm();
    ev.delaySamples = m_timing.Delay();
    ev.agreed = m_timing.HasAgreed();
    return ev;
}

// ------------------------------------------------------------------ lifecycle

VoiceProcessor::VoiceProcessor() = default;

VoiceProcessor::~VoiceProcessor()
{
    Unregister();  // the handoffs free their stages in their own destructors
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

    m_aecHandoff.Clear();
    m_aecStrengthRequested = initial.aecStrength;
    m_aecRoomRequested = initial.aecRoom;
    m_aec = m_refRing ? std::make_unique<AecInstance>(initial.aecStrength, initial.aecRoom) : nullptr;
    m_hasAec.store(m_aec != nullptr);
    m_refUnderruns = 0;
    m_sync.Reset();
    m_sync.SetRatePpm(m_refRing && m_autoTiming ? m_initialRatePpm : 0.0);
    m_drift.Clear();
    m_driftTicks = 0;
    m_driftGuard = 0;
    m_blockTicks = 0;

    // Automatic echo timing (only with a reference: without one there is no echo to line up).
    int delay = 0;
    if (m_refRing && m_autoTiming)
    {
        if (!m_probe) m_probe = std::make_unique<EchoTimingProbe>();
        if (!m_estimator) m_estimator = std::make_unique<EchoTimingEstimator>();
        m_t8mic.resize(EchoTimingEstimator::kSpan);
        m_t8ref.resize(EchoTimingEstimator::kSpan);
        m_probe->Clear();
        delay = std::clamp(m_initialMicDelay, 0, EchoTimingController::kMaxDelaySamples);
    }
    m_micDelay = delay;
    m_micDelayTarget.store(delay);
    m_micDelayApplied.store(delay);
    m_timing.Reset(delay);
    m_pad = m_padTo = kPadLatency - delay;   // mic delay + padding = kPadLatency: the total never changes
    m_padSwitchAt = 0;
    m_blockIndex = 0;

    if (!m_limiter)
        m_limiter = std::make_unique<OutputLimiter>();
    m_agcHandoff.Clear();
    m_agcRequested = ParamsFrom(initial);
    m_agc = std::make_unique<AgcStage>(m_agcRequested);
    m_agcGainDb.store(FiniteOr0(m_agc->LastGainDb()));

    m_settings = initial;
    m_level = static_cast<int>(initial.nsLevel);
    for (int l = 0; l < settings::kFilterLevels; ++l)
    {
        m_hpf[l].Design(true, settings::kRumble[l].hz, settings::kRumble[l].order, kSampleRate);
        m_lpf[l].Design(false, settings::kHiss[l].hz, settings::kHiss[l].order, kSampleRate);
    }
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
        c.mic.Clear();
        c.pad.Clear();
        c.in.fill(0.0f);
        c.out.fill(0.0f);
    }
    for (auto& f : m_hpf) f.Clear();
    for (auto& f : m_lpf) f.Clear();
    m_hpfLevel = static_cast<int>(m_settings.hpfLevel);
    m_lpfLevel = static_cast<int>(m_settings.lpfLevel);
    m_hpfNext = m_lpfNext = -1;
    for (auto& d : m_aecStandIn) d.Clear();
    m_nsDry.Clear();
    if (m_limiter) m_limiter->Reset();
    m_pos = 0;
    m_nextLevel = -1;
    m_underruns = 0;
    m_nonFiniteBlocks = 0;
    if (m_rs) m_rs->Reset();
    // Start directly in the requested state (no fade-in from silence).
    m_gain.current = m_gain.target = InputGain(m_gainTargetDb, m_settings.agcEnabled != 0);
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
    if (static_cast<int>(s.hpfLevel) != m_hpfLevel && s.hpfLevel < settings::kFilterLevels)
        m_hpfNext = static_cast<int>(s.hpfLevel);
    if (static_cast<int>(s.lpfLevel) != m_lpfLevel && s.lpfLevel < settings::kFilterLevels)
        m_lpfNext = static_cast<int>(s.lpfLevel);
    m_hpfMix.target = s.hpfEnabled ? 1.0f : 0.0f;
    m_lpfMix.target = s.lpfEnabled ? 1.0f : 0.0f;
    m_nsMix.target = s.nsEnabled ? 1.0f : 0.0f;
    m_agcMix.target = s.agcEnabled ? 1.0f : 0.0f;
    m_limMix.target = s.limiterEnabled ? 1.0f : 0.0f;
    m_wet.target = s.bypass ? 0.0f : 1.0f;
    m_gain.target = InputGain(m_gainTargetDb, s.agcEnabled != 0);  // AGC2 owns the level when on
    if (static_cast<int>(s.nsLevel) != m_level && m_nextLevel < 0)
        m_nextLevel = static_cast<int>(s.nsLevel);
}

// ------------------------------------------------------------------ processing

void VoiceProcessor::ProcessBlock() noexcept
{
    // Pick up stages rebuilt by the settings watcher. The old ones go back through the "retired"
    // slots for freeing on a non-RT thread (the watcher empties those before every rebuild).
    m_agcHandoff.TakeInto(m_agc);
    m_aecHandoff.TakeInto(m_aec);

    // Automatic echo timing: a new mic delay (from EchoTimingTick) starts at this block, crossfaded. The
    // output padding changes the other way when this block's first sample reaches the end of the chain
    // (kAlgorithmLatency later), so the total latency stays exactly kTotalLatency.
    const int delayFrom = m_micDelay, delayTo = m_micDelayTarget.load(std::memory_order_relaxed);
    if (delayTo != delayFrom)
    {
        m_padTo = kPadLatency - delayTo;
        m_padSwitchAt = static_cast<int64_t>(m_blockIndex) * kBlock + kAlgorithmLatency;
    }

    const float invCh = 1.0f / static_cast<float>(m_channels);
    m_mono.fill(0.0f);
    for (auto& c : m_ch)
    {
        // Sanitize, apply the input gain (also in bypass: it is the volume control), delay, downmix.
        for (int i = 0; i < kBlock; ++i)
        {
            // The raw sample is clamped BEFORE the gain: a finite but absurd value (driver bug) would overflow
            // the energy sums downstream. After the gain (up to +40 dB) the chain may run far above full scale
            // on purpose; the limiter at the end brings it back, so nothing is clipped here.
            const float x = c.in[i];
            c.in[i] = std::clamp(std::isfinite(x) ? x : 0.0f, -4.0f, 4.0f) * m_gain.At(i);
        }
        c.mic.Run(c.in.data(), delayFrom, delayTo);   // before everything, so bypass stays aligned
        for (int i = 0; i < kBlock; ++i)
            m_mono[i] += c.in[i] * invCh;
        c.dry.Run(c.in.data(), c.delayed.data(), kBlock);  // latency-matched original channel (bypass)
    }

    // 1. Cleanup filters: high-pass ("rumble") then low-pass ("hiss"). Every level of both always runs (warm
    //    state), a level change crossfades from the old level's output to the new one over this block, and
    //    the on/off ramps mix the result in.
    {
        const int hFrom = m_hpfLevel, hTo = m_hpfNext >= 0 ? m_hpfNext : hFrom;
        const int lFrom = m_lpfLevel, lTo = m_lpfNext >= 0 ? m_lpfNext : lFrom;
        std::array<float, settings::kFilterLevels> h{}, l{};
        for (int i = 0; i < kBlock; ++i)
        {
            const float w = static_cast<float>(i + 1) / kBlock;
            const float in = m_mono[i];
            for (int k = 0; k < settings::kFilterLevels; ++k) h[k] = m_hpf[k].Run(in);
            const float hp = h[hFrom] + w * (h[hTo] - h[hFrom]);
            const float x = in + m_hpfMix.At(i) * (hp - in);
            for (int k = 0; k < settings::kFilterLevels; ++k) l[k] = m_lpf[k].Run(x);
            const float lp = l[lFrom] + w * (l[lTo] - l[lFrom]);
            m_s1[i] = x + m_lpfMix.At(i) * (lp - x);
        }
        m_hpfLevel = hTo;
        m_lpfLevel = lTo;
        m_hpfNext = m_lpfNext = -1;
    }

    // 2. Echo reference for this block (silence when AEC is off or the loopback ran dry).
    if (m_aec)
    {
        bool have = false;
        if (m_refRing)   // rate-locked to the microphone (see ReferenceSync.h)
            have = m_sync.Pull(*m_refRing, m_ref.data(), kBlock, m_blockTicks, m_tickFreq);
        if (!have) { m_ref.fill(0.0f); ++m_refUnderruns; }
        if (m_probe && m_autoTiming)
            m_probe->Push(m_s1.data(), m_ref.data(), kBlock);   // the real reference, also while AEC is off
        if (!m_settings.aecEnabled) m_ref.fill(0.0f);
        if (!m_dump.empty())  // diagnostics: what AEC3 sees (capture, reference), no allocation
        {
            for (int i = 0; i < kBlock; ++i)
            {
                m_dump[m_dumpPos] = m_s1[i];
                m_dump[m_dumpPos + 1] = m_ref[i];
                m_dumpPos += 2;
                if (m_dumpPos >= m_dump.size()) { m_dumpPos = 0; m_dumpFull = true; }
            }
        }
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
    m_agcGainDb.store(FiniteOr0(m_agc->LastGainDb()), std::memory_order_relaxed);
    for (int i = 0; i < kBlock; ++i)
        m_voice[i] += m_agcMix.At(i) * (m_tmp[i] - m_voice[i]);

    // 5. Limiter (-1 dBFS), last.
    m_limiter->Process(m_voice.data(), m_tmp.data());
    m_limiterDb.store(m_limiter->LastReductionDb(), std::memory_order_relaxed);
    for (int i = 0; i < kBlock; ++i)
        m_voice[i] += m_limMix.At(i) * (m_tmp[i] - m_voice[i]);

    // Watchdog: the inputs are sanitised, so this should never fire; if a stage ever produces NaN/Inf
    // anyway, the apps get the latency-matched unprocessed mic for this block (clipped to full scale)
    // instead of garbage or silence, and the count shows up in the log at stream stop.
    float check = 0.0f;
    for (int i = 0; i < kBlock; ++i)
        check += m_voice[i] * 0.0f;   // stays 0 unless some sample is NaN or Inf
    if (check != 0.0f || !std::isfinite(check))
    {
        for (int i = 0; i < kBlock; ++i)
            m_voice[i] = std::clamp(m_ch[0].delayed[i], -1.0f, 1.0f);
        m_nonFiniteBlocks.fetch_add(1, std::memory_order_relaxed);
    }

    // Mono voice to every channel; master bypass crossfades to the delayed original channels.
    const int64_t blockStart = static_cast<int64_t>(m_blockIndex) * kBlock;
    for (auto& c : m_ch)
    {
        for (int i = 0; i < kBlock; ++i)
            c.out[i] = c.delayed[i] + m_wet.At(i) * (m_voice[i] - c.delayed[i]);
        c.pad.Run(c.out.data(), m_pad, m_padTo, m_padSwitchAt - blockStart);   // fixed total latency
    }
    if (m_pad != m_padTo && blockStart + kBlock >= m_padSwitchAt + kBlock)
        m_pad = m_padTo;   // crossfade finished

    // Meters (every 10 blocks).
    for (int i = 0; i < kBlock; ++i)
    {
        m_inSq += static_cast<double>(m_mono[i]) * m_mono[i];
        m_outSq += static_cast<double>(m_voice[i]) * m_voice[i];
        if (m_aec) m_refSq += static_cast<double>(m_ref[i]) * m_ref[i];
    }
    m_limPeakDb = std::max(m_limPeakDb, m_limiter->LastReductionDb());
    m_speechSum += m_agc->LastSpeechProbability();
    if (++m_meterBlocks == 10)
    {
        constexpr double n = 10.0 * kBlock;
        auto db = [](double sumSq) { return static_cast<float>(sumSq > 1e-12 * n ? 10.0 * std::log10(sumSq / n) : -120.0); };
        m_meters.inputDbfs = db(m_inSq);
        m_meters.outputDbfs = db(m_outSq);
        m_meters.referenceDbfs = m_aec ? db(m_refSq) : -120.0f;
        m_meters.aecRunning = m_aec != nullptr && m_settings.aecEnabled;
        if (m_aec)
        {
            const auto metrics = m_aec->controller->GetMetrics();
            m_meters.echoRemovedDb = static_cast<float>(metrics.echo_return_loss_enhancement);
            m_meters.aecDelayMs = metrics.delay_ms;
        }
        else
        {
            m_meters.echoRemovedDb = 0.0f;
            m_meters.aecDelayMs = -1;
        }
        m_meters.agcGainDb = m_settings.agcEnabled ? m_agc->LastGainDb() : 0.0f;
        m_meters.limiterDb = m_settings.limiterEnabled ? m_limPeakDb : 0.0f;
        m_meters.speech = m_speechSum / 10.0f;
        m_meters.refBacklog = m_sync.BacklogSamples();
        m_meters.refTarget = m_sync.TargetSamples();
        m_meters.refPpm = static_cast<float>(m_sync.RatioPpm());
        m_meters.refResyncs = m_sync.Resyncs();
        m_metersReady = true;
        m_meterBlocks = 0;
        m_inSq = m_outSq = m_refSq = 0;
        m_limPeakDb = 0;
        m_speechSum = 0;
    }

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
    if (delayTo != delayFrom)
    {
        m_micDelay = delayTo;
        m_micDelayApplied.store(delayTo, std::memory_order_relaxed);
    }
    ++m_blockIndex;
}

void VoiceProcessor::Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb) noexcept
{
    if (std::isnan(inputGainDb)) inputGainDb = 0.0f;
    if (inputGainDb != m_gainTargetDb)
    {
        m_gainTargetDb = inputGainDb;
        m_gain.target = InputGain(inputGainDb, m_settings.agcEnabled != 0);
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
            // Clock at this block's last sample (the call's clock is for its last frame).
            if (m_tickFreq > 0)
                m_blockTicks = m_callTicks - static_cast<int64_t>(frames - 1 - f) * m_tickFreq / kSampleRate;
            if (!(s == m_settings))
                ApplySettings(s);
            ProcessBlock();
            m_pos = 0;
        }
    }
}
