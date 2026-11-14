// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "ReferenceRing.h"

// Keeps the echo reference in step with the microphone (2026-11-14).
//
// Windows may deliver the loopback reference a little faster or slower than the microphone consumes it
// (measured: +62 ppm on the owner's laptop with two hardware clocks only 2 ppm apart). Read at a fixed
// rate, the reference queue then fills up (or runs dry) and every ~2.5 minutes a whole 10 ms block is
// dropped: the echo timing slides by 10 ms and jumps back, and AEC3 never settles.
//
// ReferenceSync reads the queue through a fractional resampler (16-tap Kaiser-windowed sinc, 256 phases)
// whose rate a slow PI loop adjusts so the backlog stays where it was when the stream started:
//   backlog = reference written "by now" (last stamped write + clock time since, at 48 kHz) - consumed.
// The clock stamps make the backlog continuous (not chunked in 10 ms writes). Without stamps (tests) the
// queue length itself is used.
// - Backlog target = the backlog seen over the first 0.5 s (after trimming to 40 ms, as before), so the
//   timing AEC3 sees does not change; an isolated underrun raises the target by one block (jitter headroom),
//   5+ dry blocks in a row are a stall: the backlog is learnt again when the reference comes back.
// - The target is at least one write chunk + one block + the resampler look-ahead + 5 ms: with the two rates
//   apart, the phase between writes and reads rotates and must never leave a block short.
// - Big errors get proportionally stronger correction (fast catch-up), small ones stay gentle.
// - |error| > 20 ms (a stall, a burst): resync at once (drop or wait), not slewing for minutes.
// - Rate limited to +-1000 ppm. Real-time safe: no allocation, no locks.
class ReferenceSync
{
public:
    static constexpr int kTaps = 16, kHalf = kTaps / 2, kPhases = 256;
    static constexpr int kStartCap = 1920;     // trimmed to this at (re)start: the old fixed bound (40 ms)
    static constexpr int kMinTarget = 240, kMaxTarget = 4800;
    static constexpr int kResyncSamples = 960; // 20 ms
    static constexpr double kMaxRatio = 1e-3;  // +-1000 ppm

    ReferenceSync();
    void Reset() noexcept;   // new stream (keeps nothing)

    // Produces n (<= 960) reference samples at 48 kHz. nowTicks/freq: clock at the end of this capture
    // block (0 = no clock). Returns false when it had to output silence (no reference yet / ran dry).
    bool Pull(ReferenceRing& ring, float* out, int n, int64_t nowTicks, int64_t freq) noexcept;

    double RatioPpm() const noexcept { return m_ratioPpm; }
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
    double m_integ = 0.0;      // learnt rate offset (kept across restarts within a stream)
    double m_ratioPpm = 0.0;
    uint64_t m_resyncs = 0, m_dry = 0;
    int m_dryRun = 0;          // consecutive blocks without reference while running
    size_t m_lastStampW = 0;   // to learn the reference write size (chunk)
    int m_maxChunk = 0;
};
