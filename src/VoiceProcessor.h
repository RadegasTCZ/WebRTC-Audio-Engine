#pragma once
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// Real-time voice processing core, independent of COM/APO plumbing (unit-testable).
//
// WebRTC NS at 48 kHz:
//   48 kHz block of 480 -> ThreeBandFilterBank -> NoiseSuppressor (3 x 160 @ 16 kHz) -> synthesis
// A fixed 480-sample FIFO decouples the engine's period size (128, 441, 480, ...) from the
// 10 ms NS frame, so the total latency is CONSTANT: 480 (FIFO) + 334 (filter bank + NS) samples.
//
// Threading: Prepare() allocates (call from LockForProcess). Process() never allocates,
// locks or blocks (call from APOProcess).
class VoiceProcessor
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kBlock = 480;              // 10 ms
    static constexpr int kMaxChannels = 8;
    static constexpr int kAlgorithmLatency = 334;   // filter bank 46 + NS overlap 3*96
    static constexpr int kTotalLatency = kBlock + kAlgorithmLatency;

    enum class Level { k6dB = 0, k12dB = 1, k18dB = 2, k21dB = 3 };

    VoiceProcessor();
    ~VoiceProcessor();

    // Returns false (and stays in pass-through) if the format is unsupported.
    bool Prepare(int channels, int sampleRate, Level level);
    bool IsActive() const noexcept { return m_active; }
    void Reset() noexcept;

    // Interleaved float32. in and out may be the same buffer (in-place).
    void Process(const float* in, float* out, int frames) noexcept;

private:
    struct Chain;
    void ProcessBlock() noexcept;

    bool m_active = false;
    int m_channels = 0;
    int m_pos = 0;
    std::vector<std::unique_ptr<Chain>> m_chains;
    // Per channel: input being collected, output being played out.
    std::vector<std::array<float, kBlock>> m_inBlock;
    std::vector<std::array<float, kBlock>> m_outBlock;
};
