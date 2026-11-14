// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace webrtc { class Pffft; }

// Automatic echo timing (2026-11-14, see ..\docs\aec-echo-timing.md).
//
// AEC3 can only cancel echo that reaches the microphone AFTER the echo reference. Some microphones
// (USB, e.g. the Behringer C-1U) hand Windows their audio a few ms "late", so the echo comes before
// the reference and AEC3 removes almost nothing. The fix: measure where the echo sits relative to the
// reference and delay the microphone just enough to keep a safety margin.
//
//   EchoTimingProbe       real-time side: 48 kHz mic + reference -> 8 kHz rings (no allocation)
//   EchoTimingEstimator   non-RT: echo offset in ms from the last ~3 s (Welch-averaged, coherence-
//                         weighted cross-correlation, i.e. GCC-SCOT), plus a quality score
//   EchoTimingController  non-RT: turns measurements into a mic delay (agreement, margin, cap, settle)

// Positive offset = the echo comes after the reference (fine); negative = before (AEC3 cannot model it).
struct EchoTimingResult
{
    bool valid = false;        // enough reference activity and a clear peak
    float offsetMs = 0.0f;     // echo vs reference, as AEC3 sees it (i.e. after any mic delay)
    float quality = 0.0f;      // peak height in standard deviations above the other lags
    float peak = 0.0f;         // peak coherence-weighted correlation (0..1)
    int activeFrames = 0;      // analysis frames with the speakers playing
    bool outOfRange = false;   // a clear peak, but outside the range we act on
};

class EchoTimingProbe
{
public:
    static constexpr int kDecimation = 6;          // 48 kHz -> 8 kHz
    static constexpr int kRate = 8000;
    static constexpr size_t kRingSize = 65536;     // 8.2 s at 8 kHz (power of two); reads take at most half

    // Real-time: 48 kHz mono mic and reference, any n (decimated by summing groups of 6).
    void Push(const float* mic48, const float* ref48, int n) noexcept;
    void Clear() noexcept;
    // Non-RT: copies the newest `n` 8 kHz samples (n <= kRingSize / 2). False if not that many yet.
    bool Latest(float* mic8, float* ref8, size_t n) const noexcept;
    uint64_t Written() const noexcept { return m_written.load(std::memory_order_acquire); }

private:
    float m_mic[kRingSize]{}, m_ref[kRingSize]{};
    float m_accMic = 0.0f, m_accRef = 0.0f;
    int m_phase = 0;
    std::atomic<uint64_t> m_written{0};
};

class EchoTimingEstimator
{
public:
    static constexpr int kFrame = 4096;            // 512 ms at 8 kHz
    static constexpr int kHop = 2048;
    static constexpr int kFrames = 11;
    static constexpr size_t kSpan = static_cast<size_t>(kFrame + (kFrames - 1) * kHop);   // 24576 = 3.07 s
    static constexpr float kMinLagMs = -60.0f, kMaxLagMs = 250.0f;   // searched
    static constexpr float kActMinMs = -20.0f, kActMaxMs = 120.0f;   // acted on
    static constexpr float kMinQuality = 9.0f;
    static constexpr float kMinPeak = 0.03f;
    static constexpr int kMinActiveFrames = 5;
    static constexpr float kRefActiveDbfs = -60.0f;   // per frame, 48 kHz full scale

    EchoTimingEstimator();   // allocates (non-RT)
    ~EchoTimingEstimator();
    EchoTimingResult Estimate(const float* mic8, const float* ref8) noexcept;   // kSpan samples each

private:
    std::unique_ptr<webrtc::Pffft> m_fft;
    struct Buffers;
    std::unique_ptr<Buffers> m_b;
};

// Turns measurements into a delay (in 48 kHz samples). Pure logic, unit-tested.
class EchoTimingController
{
public:
    static constexpr float kMarginMs = 8.0f;      // keep the echo at least this far after the reference
    static constexpr float kMaxDelayMs = 25.0f;     // fits the fixed 50 ms latency budget
    static constexpr float kAgreeMs = 1.0f;       // 3 results within this spread
    static constexpr int kAgreeCount = 3;
    static constexpr float kChangeMs = 3.0f;      // change only when the needed delay moves this much
    static constexpr int kSettleTicks = 4;        // results ignored after a change (span = 3 s, 1 tick/s)
    static constexpr int kMaxDelaySamples = static_cast<int>(kMaxDelayMs * 48);   // 1200

    struct Decision
    {
        bool change = false;
        int delaySamples = 0;      // the new delay (when change)
        float rawOffsetMs = 0.0f;  // echo vs reference without our delay (the agreed value)
        bool capped = false;       // the delay needed was more than kMaxDelayMs
    };

    void Reset(int currentDelaySamples) noexcept;
    Decision OnResult(const EchoTimingResult& r) noexcept;
    int Delay() const noexcept { return m_delay; }
    bool HasAgreed() const noexcept { return m_agreed; }
    bool Settling() const noexcept { return m_settle > 0; }
    float LastRawOffsetMs() const noexcept { return m_lastRaw; }

private:
    int m_delay = 0;
    int m_settle = 0;
    float m_recent[kAgreeCount]{};
    int m_count = 0;
    bool m_agreed = false;
    float m_lastRaw = 0.0f;
};

// How fast the echo slides against the reference (ppm; - = the reference content runs fast, so the echo
// comes ever earlier). Theil-Sen (median of pairwise slopes) over the raw echo positions, with pairs only
// inside stretches without a jump (> 1.5 ms between neighbours), so a resync or a packet jump does not bend it.
class EchoDriftEstimator
{
public:
    static constexpr int kMax = 64;
    void Clear() noexcept { m_n = 0; }
    void Add(double seconds, float rawMs) noexcept;
    // False until there are enough points. used = pairs' points, span = seconds covered.
    bool Slope(double& ppm, int& points, double& span) const noexcept;

private:
    double m_t[kMax]{};
    float m_v[kMax]{};
    int m_n = 0;
};
