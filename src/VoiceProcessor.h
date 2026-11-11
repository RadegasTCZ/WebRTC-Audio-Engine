// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "AgcStage.h"
#include "ReferenceRing.h"
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
    static constexpr int kTotalLatency = kBlock + kAlgorithmLatency;
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

    int LatencyFrames() const noexcept { return m_latencyFrames; }
    int SampleRate() const noexcept { return m_rate; }
    uint64_t Underruns() const noexcept { return m_underruns; }
    uint64_t ReferenceUnderruns() const noexcept { return m_refUnderruns; }
    bool HasAec() const noexcept { return m_aec != nullptr; }
    float AgcGainDb() const noexcept { return m_agcGainDb.load(std::memory_order_relaxed); }
    float LimiterReductionDb() const noexcept { return m_limiterDb.load(std::memory_order_relaxed); }

    // Interleaved float32; in and out may alias. `s` applies at the next block boundary.
    // inputGainDb = mic volume slider gain; ignored (0 dB) while AGC2 is enabled.
    void Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb = 0.0f) noexcept;

    // Non-RT: rebuild AGC2 / AEC3 if their parameters differ from `s` (settings watcher hook).
    void RequestStageParams(const Settings& s);
    static void OnSettingsChanged(const Settings& s);  // walks all live processors

    static float DbToLinear(float db) noexcept;

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
    struct Channel
    {
        VarDelay dry;
        std::array<float, kBlock> in{}, out{}, delayed{};
    };

    void ProcessCore(const float* in, float* out, int frames, const Settings& s) noexcept;  // 48 kHz
    void ProcessBlock() noexcept;
    void ApplySettings(const Settings& s) noexcept;
    void Register();
    void Unregister();

    bool m_active = false;
    bool m_registered = false;
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
    std::atomic<AecInstance*> m_aecPending{nullptr}, m_aecRetired{nullptr};
    uint32_t m_aecStrengthRequested = 1;
    ReferenceRing* m_refRing = nullptr;
    uint64_t m_refUnderruns = 0;

    std::unique_ptr<AgcStage> m_agc;
    std::atomic<AgcStage*> m_agcPending{nullptr}, m_agcRetired{nullptr};
    AgcParams m_agcRequested{};
    std::unique_ptr<OutputLimiter> m_limiter;

    Settings m_settings{};
    uint32_t m_hpfHzApplied = 0, m_lpfHzApplied = 0;
    int m_level = 1, m_nextLevel = -1;
    Ramp m_hpfMix, m_lpfMix, m_nsMix, m_agcMix, m_limMix, m_wet;
    float m_gainTargetDb = 0.0f;
    Ramp m_gain{1.0f, 1.0f};
    std::atomic<float> m_agcGainDb{0.0f}, m_limiterDb{0.0f};

    std::unique_ptr<Resampling> m_rs;   // null at 48 kHz
    int m_rate = 0;
    int m_latencyFrames = 0;
    uint64_t m_underruns = 0;
};
