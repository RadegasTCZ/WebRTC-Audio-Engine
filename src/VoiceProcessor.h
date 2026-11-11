#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include "Settings.h"

// Real-time voice processing core, independent of COM/APO plumbing (unit-testable).
//
// Chain per channel, in 10 ms blocks at 48 kHz:
//   input -> [HPF 2nd-order Butterworth] -> [WebRTC NS: ThreeBandFilterBank + NoiseSuppressor] -> output
// Every stage always runs; switching a stage on/off crossfades (one block) between its output and
// a latency-matched dry path, so toggling never clicks and never changes the overall delay.
// Master bypass crossfades to the input delayed by the same constant latency.
// A fixed 480-sample FIFO decouples the engine's period size (128, 441, 480, ...) from the block.
//
// Threading: Prepare() allocates (call from LockForProcess). Process() never allocates,
// locks or blocks (call from APOProcess).
class VoiceProcessor
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBlock = 480;              // 10 ms
    static constexpr int kMaxChannels = 8;
    static constexpr int kNsLevels = 4;
    static constexpr int kAlgorithmLatency = 334;   // filter bank 46 + NS overlap 3*96
    static constexpr int kTotalLatency = kBlock + kAlgorithmLatency;

    static constexpr int kMinRate = 8000;
    static constexpr int kMaxRate = 192000;

    VoiceProcessor();
    ~VoiceProcessor();

    // Any rate in [8 kHz, 192 kHz]: 48 kHz is processed directly, other rates are resampled to
    // 48 kHz and back (SpeexDSP, quality 5) with a fixed output buffer, so latency stays constant.
    // maxFrames = the largest period the engine will hand us (LockForProcess u32MaxFrameCount).
    // Returns false (and stays in pass-through) if the format is unsupported.
    bool Prepare(int channels, int sampleRate, const Settings& initial, int maxFrames = 4096);
    void Deactivate() noexcept { m_active = false; }
    bool IsActive() const noexcept { return m_active; }
    void Reset() noexcept;

    // Total latency in frames at the device rate (constant for a given format).
    int LatencyFrames() const noexcept { return m_latencyFrames; }
    int SampleRate() const noexcept { return m_rate; }
    uint64_t Underruns() const noexcept { return m_underruns; }

    // Interleaved float32. in and out may be the same buffer (in-place).
    // `s` is applied at the next 10 ms block boundary.
    void Process(const float* in, float* out, int frames, const Settings& s) noexcept;

private:
    struct Resampling;
    void ProcessCore(const float* in, float* out, int frames, const Settings& s) noexcept;  // 48 kHz stream
    std::unique_ptr<Resampling> m_rs;   // null at 48 kHz
    int m_rate = 0;
    int m_latencyFrames = 0;
    uint64_t m_underruns = 0;

    struct NsChain;
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
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
        Biquad hpf;
        std::array<std::unique_ptr<NsChain>, kNsLevels> ns;
        Delay dryAfterHpf, dryInput;
        std::array<float, kBlock> in{}, out{};
        std::array<float, kBlock> s1{}, nsA{}, nsB{}, d1{}, d0{};
    };

    void ProcessBlock() noexcept;
    void ApplySettings(const Settings& s) noexcept;

    bool m_active = false;
    int m_channels = 0;
    int m_pos = 0;
    std::vector<Channel> m_ch;

    Settings m_settings{};
    uint32_t m_hpfHzApplied = 0;
    int m_level = 1;         // active NS level
    int m_nextLevel = -1;    // level being crossfaded in this block, or -1
    Ramp m_hpfMix, m_nsMix, m_wet;   // 1 = stage on / chain active
};
