// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "AgcStage.h"
#include "EchoTiming.h"
#include "ReferenceRing.h"
#include "ReferenceSync.h"
#include "Settings.h"

namespace webrtc { class AudioBuffer; class EchoControl; class NoiseSuppressor; }

// Real-time voice processing core, independent of COM/APO plumbing (unit-testable).
//
// 48 kHz, 10 ms blocks, voice path in MONO (like the AEC3 plugin), WebRTC APM order:
//   input (N ch) -> input gain -> downmix -> HPF -> band split ->
//   [NS analyse (pre-AEC)] -> AEC3 (echo reference from the render loopback) -> NS process ->
//   band merge -> AGC2 -> limiter -> every channel
// - Every stage always runs; switching a stage on/off crossfades (one block) against a latency-matched
//   dry path, so toggling never clicks and the overall delay never changes. "AEC off" feeds AEC3 a
//   silent reference (it stops removing echo, the delay stays the same).
// - Without a reference (non-Communications streams) AEC3 is skipped and replaced by its exact delay.
// - All four NS levels run in parallel: a level change is an instant crossfade (no re-adaptation).
// - AGC2 / AEC3 parameter changes rebuild those stages on the settings watcher thread (they allocate);
//   the real-time thread swaps them in at a block boundary.
// - Other sample rates are resampled to 48 kHz and back (SpeexDSP) with constant latency.
// - Automatic echo timing (streams with a reference): if the echo reaches the mic too early for AEC3,
//   every input channel is delayed (0..20 ms, crossfaded) in front of the whole chain. EchoTimingTick()
//   measures and decides on a non-RT thread; see EchoTiming.h.
// - FIXED LATENCY of 50 ms (design choice): chain (25 ms) + mic delay + output padding always add
//   up to kTotalLatency. A mic delay change moves the padding exactly when the shifted audio reaches the end
//   of the chain, so the output timing never moves and the reported latency is always true.
// - The echo reference is read through ReferenceSync (rate-locked to the microphone).
//
// Threading: Prepare()/SetReference() allocate or configure (LockForProcess). Process() never
// allocates, locks or blocks.
class VoiceProcessor
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBlock = 480;              // 10 ms
    static constexpr int kMaxChannels = 8;
    static constexpr int kNsLevels = 4;
    static constexpr int kBands = 3, kBandSize = 160;
    static constexpr int kSplitLatency = 46;        // three-band analysis + synthesis
    static constexpr int kAecLatency = 384;         // AEC3 block framing, measured by ProcessorTests [11]
    static constexpr int kNsLatency = 288;          // NS overlap, 96 samples per band * 3
    static constexpr int kAlgorithmLatency = kSplitLatency + kAecLatency + kNsLatency;   // 718
    static constexpr int kChainLatency = kBlock + kAlgorithmLatency;   // 1198 (25 ms)
    static constexpr int kTotalLatency = 2400;                         // fixed budget: 50 ms
    static constexpr int kPadLatency = kTotalLatency - kChainLatency;  // padding when the mic delay is 0
    static constexpr int kMinRate = 8000;
    static constexpr int kMaxRate = 192000;

    VoiceProcessor();
    ~VoiceProcessor();

    bool Prepare(int channels, int sampleRate, const Settings& initial, int maxFrames = 4096);
    void Deactivate() noexcept;
    bool IsActive() const noexcept { return m_active; }
    void Reset() noexcept;

    // Echo reference (48 kHz mono ring filled by the auxiliary input). nullptr = this stream has none.
    // Call before Prepare or between Deactivate and Prepare (non-RT).
    void SetReference(ReferenceRing* ring) noexcept { m_refRing = ring; }
    // Which settings snapshot this processor follows (settings::RegisterDevice; -1 = defaults).
    void SetSettingsIndex(int index) noexcept { m_settingsIndex = index; }

    int LatencyFrames() const noexcept { return m_latencyFrames; }   // fixed (the mic delay is inside the budget)
    // Clock reading at the start of the next Process call (e.g. QueryPerformanceCounter), for ReferenceSync.
    void SetClock(int64_t nowTicks, int64_t freq) noexcept { m_callTicks = nowTicks; m_tickFreq = freq; }
    const ReferenceSync& Sync() const noexcept { return m_sync; }

    // Automatic echo timing. Call before Prepare (non-RT). initialDelay = 48 kHz samples remembered for this
    // microphone (applied from the first block and included in the latency); ignored without a reference.
    // initialRatePpm = the reference rate correction remembered for this microphone.
    void SetEchoTimingSetup(bool automatic, int initialDelay, double initialRatePpm = 0.0) noexcept
    {
        m_autoTiming = automatic;
        m_initialMicDelay = initialDelay;
        m_initialRatePpm = initialRatePpm;
    }
    struct EchoTimingEvent
    {
        bool measured = false;                 // a measurement ran (enough audio collected)
        EchoTimingResult result;
        EchoTimingController::Decision decision;
        int delaySamples = 0;                  // the delay after this tick (48 kHz samples)
        bool agreed = false;                   // the controller has an agreed echo timing this stream
        bool rateChanged = false;              // the reference rate was corrected from the echo slide
        double slidePpm = 0.0;                 // the slide measured (before the correction)
        double ratePpm = 0.0;                  // the reference rate correction now
    };
    // Non-RT, about once per second, from ONE thread, only while active (stop it before Prepare/Deactivate).
    EchoTimingEvent EchoTimingTick() noexcept;
    int MicDelaySamples() const noexcept { return m_micDelayApplied.load(std::memory_order_relaxed); }
    bool EchoTimingActive() const noexcept { return m_probe != nullptr && m_refRing != nullptr && m_autoTiming; }
    int SampleRate() const noexcept { return m_rate; }
    uint64_t Underruns() const noexcept { return m_underruns; }
    uint64_t ReferenceUnderruns() const noexcept { return m_refUnderruns; }
    uint64_t NonFiniteBlocks() const noexcept { return m_nonFiniteBlocks.load(std::memory_order_relaxed); }
    bool HasAec() const noexcept { return m_hasAec.load(std::memory_order_relaxed); }
    float AgcGainDb() const noexcept { return m_agcGainDb.load(std::memory_order_relaxed); }
    float LimiterReductionDb() const noexcept { return m_limiterDb.load(std::memory_order_relaxed); }

    // Live meters, refreshed every 10 blocks (100 ms). RT-safe: call after Process(); returns true once
    // per refresh.
    struct Meters
    {
        float inputDbfs = -120, outputDbfs = -120, referenceDbfs = -120;
        float echoRemovedDb = 0, agcGainDb = 0, limiterDb = 0;
        float speech = 0;  // mean speech probability (AGC2 voice detector, runs even when AGC is off)
        int aecDelayMs = -1;  // AEC3's own estimate of the echo delay (reference -> microphone); -1 = no AEC3
        float refBacklog = 0;    // echo reference backlog (samples, smoothed) held by ReferenceSync
        int refTarget = 0;       // the backlog it holds
        float refPpm = 0;        // its rate correction
        uint64_t refResyncs = 0; // jumps back to the target since the stream started
        bool aecRunning = false;
    };

    // Diagnostics: records the last `seconds` of exactly what AEC3 receives (capture after the cleanup
    // filters, and the echo reference), 48 kHz, interleaved pairs. Arm before Prepare (allocates, non-RT);
    // the real-time thread fills a ring. TakeDump (non-RT, after the stream stopped) returns the pairs
    // oldest first.
    void ArmDump(int seconds) { m_dump.assign(static_cast<size_t>(seconds) * kSampleRate * 2, 0.0f); m_dumpPos = 0; m_dumpFull = false; }
    std::vector<float> TakeDump();
    bool TakeMeters(Meters& out) noexcept
    {
        if (!m_metersReady) return false;
        out = m_meters;
        m_metersReady = false;
        return true;
    }

    // Interleaved float32; in and out may alias. `s` applies at the next block boundary.
    // inputGainDb = mic volume slider gain; ignored (0 dB) while AGC2 is enabled. kMutedDb = mic muted
    // (silence, also with AGC2 on).
    static constexpr float kMutedDb = -1.0e30f;
    void Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb = 0.0f) noexcept;

    // Non-RT: rebuild AGC2 / AEC3 if their parameters differ from `s` (settings watcher hook).
    void RequestStageParams(const Settings& s);
    static void OnSettingsChanged(const Settings& s);  // walks all live processors

    static float DbToLinear(float db) noexcept;
    static float InputGain(float sliderDb, bool agcEnabled) noexcept;

private:
    struct Resampling;
    struct AecInstance;
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void SetHighPass(float hz, float sampleRate) noexcept;
        void SetLowPass(float hz, float sampleRate) noexcept;
        float Run(float x) noexcept;
    };
    struct Ramp
    {
        float current = 0, target = 0;
        float At(int i) const noexcept { return current + (target - current) * (static_cast<float>(i + 1) / kBlock); }
        void Finish() noexcept { current = target; }
    };
    struct VarDelay  // fixed delay of any length (allocated in Prepare)
    {
        std::vector<float> buf;
        size_t pos = 0;
        void Init(size_t len) { buf.assign(len, 0.0f); pos = 0; }
        void Clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
        void Run(const float* in, float* out, int n) noexcept;
    };
    // Hands a stage rebuilt on the settings watcher thread (it allocates) to the real-time thread, and the
    // replaced stage back for freeing. Only the watcher frees; only the RT thread fills `retired`. Two retired
    // slots suffice: every Offer empties both before publishing a new stage, and at most two swaps can
    // happen between two Offers (one of the previous request, one of the new).
    template <class T>
    struct Handoff
    {
        std::atomic<T*> pending{nullptr};
        std::array<std::atomic<T*>, 2> retired{};
        ~Handoff() { Clear(); }
        void Clear() noexcept   // non-RT
        {
            delete pending.exchange(nullptr);
            for (auto& r : retired) delete r.exchange(nullptr);
        }
        void Offer(T* fresh) noexcept   // non-RT (watcher)
        {
            for (auto& r : retired) delete r.exchange(nullptr);
            delete pending.exchange(fresh);   // a request the RT thread never picked up
        }
        void TakeInto(std::unique_ptr<T>& current) noexcept   // RT: swap in a pending stage, if any
        {
            std::atomic<T*>* slot = nullptr;
            for (auto& r : retired)
                if (!r.load(std::memory_order_acquire)) { slot = &r; break; }
            if (!slot) return;   // no room to hand the old one back yet: keep it for one more block
            T* fresh = pending.exchange(nullptr);
            if (!fresh) return;
            slot->store(current.release(), std::memory_order_release);
            current.reset(fresh);
        }
    };
    struct MicDelay  // 0..25 ms (also the output padding), changes crossfade over 480 samples
    {
        static constexpr size_t kSize = 2048;   // > max delay + one block, power of two
        std::array<float, kSize> buf{};
        size_t w = 0;
        void Clear() noexcept { buf.fill(0.0f); w = 0; }
        // One block, in place. The crossfade from `from` to `to` starts at sample `rampStart` of this block
        // (negative: it started in an earlier block; >= kBlock: not yet).
        void Run(float* x, int from, int to, int64_t rampStart = 0) noexcept;
    };
    struct Channel
    {
        VarDelay dry;
        MicDelay mic;
        MicDelay pad;   // output padding: kPadLatency - mic delay
        std::array<float, kBlock> in{}, out{}, delayed{};
    };

    void ProcessCore(const float* in, float* out, int frames, const Settings& s) noexcept;  // 48 kHz
    void ProcessBlock() noexcept;
    void ApplySettings(const Settings& s) noexcept;
    void Register();
    void Unregister();

    std::atomic<bool> m_active{false};           // read by the timer thread too
    bool m_registered = false;
    int m_settingsIndex = -1;
    int m_channels = 0;
    int m_pos = 0;
    std::vector<Channel> m_ch;

    // Mono voice path.
    Biquad m_hpf, m_lpf;
    std::unique_ptr<webrtc::AudioBuffer> m_cap, m_render;
    std::array<std::unique_ptr<webrtc::AudioBuffer>, kNsLevels> m_nsBuf;
    std::array<std::unique_ptr<webrtc::NoiseSuppressor>, kNsLevels> m_ns;
    std::array<VarDelay, kBands> m_aecStandIn;   // replaces AEC3's delay when there is no reference
    VarDelay m_nsDry;                            // post-AEC signal delayed by the NS latency
    std::array<float, kBlock> m_mono{}, m_s1{}, m_ref{}, m_postAec{}, m_d1{}, m_nsOut{}, m_nsAlt{}, m_voice{}, m_tmp{};

    std::unique_ptr<AecInstance> m_aec;           // null when the stream has no echo reference
    Handoff<AecInstance> m_aecHandoff;
    std::atomic<bool> m_hasAec{false};            // m_aec != nullptr, readable from the watcher thread
    uint32_t m_aecStrengthRequested = 1;
    uint32_t m_aecRoomRequested = 0;
    ReferenceRing* m_refRing = nullptr;
    std::atomic<uint64_t> m_refUnderruns{0};     // RT writes, other threads read (logs)

    std::unique_ptr<AgcStage> m_agc;
    Handoff<AgcStage> m_agcHandoff;
    AgcParams m_agcRequested{};
    std::unique_ptr<OutputLimiter> m_limiter;

    Settings m_settings{};
    uint32_t m_hpfHzApplied = 0, m_lpfHzApplied = 0;
    int m_level = 1, m_nextLevel = -1;
    Ramp m_hpfMix, m_lpfMix, m_nsMix, m_agcMix, m_limMix, m_wet;
    float m_gainTargetDb = 0.0f;
    Ramp m_gain{1.0f, 1.0f};
    std::atomic<float> m_agcGainDb{0.0f}, m_limiterDb{0.0f};

    Meters m_meters{};
    bool m_metersReady = false;
    std::vector<float> m_dump;   // diagnostics ring (empty = off)
    size_t m_dumpPos = 0;
    bool m_dumpFull = false;
    int m_meterBlocks = 0;
    double m_inSq = 0, m_outSq = 0, m_refSq = 0;
    float m_limPeakDb = 0;
    float m_speechSum = 0;
    ReferenceSync m_sync;
    int64_t m_callTicks = 0, m_tickFreq = 0, m_blockTicks = 0;

    std::unique_ptr<Resampling> m_rs;   // null at 48 kHz
    int m_rate = 0;
    int m_latencyFrames = 0;
    int m_pad = kPadLatency, m_padTo = kPadLatency;   // output padding (RT)
    int64_t m_padSwitchAt = 0;                       // absolute output sample where its crossfade starts
    uint64_t m_blockIndex = 0;

    // Automatic echo timing.
    bool m_autoTiming = true;
    int m_initialMicDelay = 0;
    int m_micDelay = 0;                          // RT: applied
    std::atomic<int> m_micDelayTarget{0};        // tick -> RT
    std::atomic<int> m_micDelayApplied{0};       // RT -> anyone (latency, logs)
    std::unique_ptr<EchoTimingProbe> m_probe;    // allocated in Prepare when there is a reference
    std::unique_ptr<EchoTimingEstimator> m_estimator;
    EchoTimingController m_timing;
    std::vector<float> m_t8mic, m_t8ref;
    double m_initialRatePpm = 0.0;
    EchoDriftEstimator m_drift;
    int m_driftTicks = 0;
    uint64_t m_driftGuard = 0;   // resyncs + dry blocks at the last tick (a change = a jump: start over)
    std::atomic<uint64_t> m_underruns{0};        // RT writes, other threads read (logs)
    std::atomic<uint64_t> m_nonFiniteBlocks{0};   // blocks silenced by the output watchdog (should stay 0)
};
