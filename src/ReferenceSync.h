// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "ReferenceRing.h"

// Keeps the echo reference in step with the microphone (2026-11-14).
//
// Measured on the owner's laptop: Windows hands the loopback reference over in lockstep with the mic
// (480 samples per call), but its CONTENT runs ~60 ppm fast; the surplus arrives as one extra 10 ms packet
// every ~165 s. Read 1:1, the echo slides ~0.6 ms per 10 s against the reference and jumps back at each
// extra packet, so AEC3 keeps re-learning. The queue length cannot show the content rate (it only steps
// at the extra packets), but the echo can: EchoTiming measures the slide precisely while sound plays.
//
// So ReferenceSync reads the reference through a fractional resampler (16-tap Kaiser-windowed sinc, 256
// phases) at a rate set from outside (SetRatePpm: the echo-slide estimate, remembered per microphone).
// With the right rate the echo holds still and the extra packets merely top the queue up: the backlog is
// allowed to saw by a packet. The queue itself is only a guard:
// - Backlog target = what the stream starts with (second half of the first 0.5 s, after trimming to 40 ms
//   as before), at least one write chunk + one block + the look-ahead + 5 ms.
// - |backlog - target| > 25 ms (a stall, a burst, or a wrong rate with no echo to correct it): resync at once.
// - An isolated dry block raises the target by one block; 5+ in a row are a stall (relearn on return).
// Real-time safe: no allocation, no locks.
class ReferenceSync
{
public:
    static constexpr int kTaps = 16, kHalf = kTaps / 2, kPhases = 256;
    static constexpr int kStartCap = 1920;     // trimmed to this at (re)start: the old fixed bound (40 ms)
    static constexpr int kMinTarget = 240, kMaxTarget = 4800;
    static constexpr int kResyncSamples = 1200; // 25 ms
    static constexpr double kMaxRatio = 1e-3;  // +-1000 ppm

    ReferenceSync();
    void Reset() noexcept;   // new stream (keeps nothing)

    // Produces n (<= 960) reference samples at 48 kHz. nowTicks/freq: clock at the end of this capture
    // block (0 = no clock). Returns false when it had to output silence (no reference yet / ran dry).
    bool Pull(ReferenceRing& ring, float* out, int n, int64_t nowTicks, int64_t freq) noexcept;

    double RatioPpm() const noexcept { return m_ratioPpm; }
    // Reference rate correction (ppm; + = the reference content runs fast). Any thread.
    void SetRatePpm(double ppm) noexcept
    {
        m_ratePpb.store(static_cast<int64_t>(std::llround(std::fmax(-kMaxRatio * 1e6, std::fmin(kMaxRatio * 1e6, ppm)) * 1000.0)),
                        std::memory_order_relaxed);
    }
    double RatePpm() const noexcept { return static_cast<double>(m_ratePpb.load(std::memory_order_relaxed)) / 1000.0; }
    float BacklogSamples() const noexcept { return static_cast<float>(m_fillEma); }
    int TargetSamples() const noexcept { return m_target; }
    bool Locked() const noexcept { return m_state == State::Running; }
    uint64_t Resyncs() const noexcept { return m_resyncs; }
    uint64_t DryBlocks() const noexcept { return m_dry; }

private:
    enum class State { Waiting, Settling, Running };
    double Backlog(const ReferenceRing& ring, int64_t nowTicks, int64_t freq) const noexcept;
    bool Fill(ReferenceRing& ring, size_t upTo) noexcept;   // pull from the ring into m_buf
    void Restart(ReferenceRing& ring) noexcept;

    std::array<std::array<float, kTaps>, kPhases + 1> m_table{};
    static constexpr size_t kBuf = 4096;
    std::array<float, kBuf> m_buf{};
    size_t m_len = 0;      // valid samples in m_buf
    double m_pos = 0.0;    // fractional read position in m_buf (centre of the next output sample)

    State m_state = State::Waiting;
    int m_settleBlocks = 0;
    double m_settleSum = 0.0;
    int m_target = kStartCap;
    double m_fillEma = 0.0;
    std::atomic<int64_t> m_ratePpb{0};   // rate correction (from the echo slide), parts per billion
    double m_ratioPpm = 0.0;
    uint64_t m_resyncs = 0, m_dry = 0;
    int m_dryRun = 0;          // consecutive blocks without reference while running
    size_t m_lastStampW = 0;   // to learn the reference write size (chunk)
    int m_maxChunk = 0;
};
