// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "AgcStage.h"
#include "Settings.h"

// Real-time voice processing core, independent of COM/APO plumbing (unit-testable).
//
// 48 kHz, 10 ms blocks, voice path in MONO (like the AEC3 plugin):
//   input (N ch) -> input gain (slider) -> mono downmix -> HPF -> WebRTC NS -> AGC2 -> limiter -> all N channels
// - Every stage always runs; switching a stage on/off crossfades (one block) against a latency-matched
//   dry path, so toggling never clicks and the overall delay never changes.
// - All four NS levels run in parallel, so a level change is an instant crossfade between adapted
//   suppressors (no re-adaptation, no allocation).
// - AGC2 parameter changes rebuild the AGC stage on the settings watcher thread (it allocates); the
//   real-time thread swaps it in at a block boundary.
// - Master bypass crossfades to the original channels delayed by the same latency (input gain kept).
// - Other sample rates are resampled to 48 kHz and back (SpeexDSP) with constant latency.
//
// Threading: Prepare() allocates (LockForProcess). Process() never allocates, locks or blocks.
class VoiceProcessor
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBlock = 480;              // 10 ms
    static constexpr int kMaxChannels = 8;
    static constexpr int kNsLevels = 4;
    static constexpr int kAlgorithmLatency = 334;   // band split 46 + NS overlap 3*96
    static constexpr int kTotalLatency = kBlock + kAlgorithmLatency;
    static constexpr int kMinRate = 8000;
    static constexpr int kMaxRate = 192000;

    VoiceProcessor();
    ~VoiceProcessor();

    bool Prepare(int channels, int sampleRate, const Settings& initial, int maxFrames = 4096);
    void Deactivate() noexcept;
    bool IsActive() const noexcept { return m_active; }
    void Reset() noexcept;

    int LatencyFrames() const noexcept { return m_latencyFrames; }
    int SampleRate() const noexcept { return m_rate; }
    uint64_t Underruns() const noexcept { return m_underruns; }
    float AgcGainDb() const noexcept { return m_agcGainDb.load(std::memory_order_relaxed); }
    float LimiterReductionDb() const noexcept { return m_limiterDb.load(std::memory_order_relaxed); }

    // Interleaved float32; in and out may alias. `s` applies at the next block boundary.
    // inputGainDb = mic volume slider gain; ignored (0 dB) while AGC2 is enabled.
    void Process(const float* in, float* out, int frames, const Settings& s, float inputGainDb = 0.0f) noexcept;

    // Non-RT: (re)build the AGC stage if its parameters differ from `s` (called by the settings
    // watcher hook for all live processors; tests may call it directly).
    void RequestAgcParams(const Settings& s);
    static void OnSettingsChanged(const Settings& s);  // walks all live processors

    static float DbToLinear(float db) noexcept;

private:
    struct NsLevel;
    struct Resampling;
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void SetHighPass(float hz, float sampleRate) noexcept;
        float Run(float x) noexcept;
    };
    struct Ramp
    {
        float current = 0, target = 0;
        float At(int i) const noexcept { return current + (target - current) * (static_cast<float>(i + 1) / kBlock); }
        void Finish() noexcept { current = target; }
    };
    struct Delay  // fixed kAlgorithmLatency delay over one block
    {
        std::array<float, kAlgorithmLatency> hist{};
        void Run(const float* in, float* out) noexcept;
    };
    struct Channel
    {
        Delay dry;
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
    Biquad m_hpf;
    std::array<std::unique_ptr<NsLevel>, kNsLevels> m_ns;
    Delay m_dryAfterHpf;
    std::array<float, kBlock> m_mono{}, m_s1{}, m_nsOut{}, m_nsAlt{}, m_d1{}, m_voice{}, m_tmp{};
    std::unique_ptr<AgcStage> m_agc;
    std::atomic<AgcStage*> m_agcPending{nullptr};
    std::atomic<AgcStage*> m_agcRetired{nullptr};
    AgcParams m_agcRequested{};
    std::unique_ptr<OutputLimiter> m_limiter;

    Settings m_settings{};
    uint32_t m_hpfHzApplied = 0;
    int m_level = 1, m_nextLevel = -1;
    Ramp m_hpfMix, m_nsMix, m_agcMix, m_limMix, m_wet;
    float m_gainTargetDb = 0.0f;
    Ramp m_gain{1.0f, 1.0f};
    std::atomic<float> m_agcGainDb{0.0f}, m_limiterDb{0.0f};

    std::unique_ptr<Resampling> m_rs;   // null at 48 kHz
    int m_rate = 0;
    int m_latencyFrames = 0;
    uint64_t m_underruns = 0;
};
