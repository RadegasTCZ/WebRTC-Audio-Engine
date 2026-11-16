// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#include "ReferenceSync.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace
{
    constexpr int kRate = 48000;
    constexpr int kSettleBlocks = 50;           // 0.5 s to learn the natural backlog
    constexpr double kEma = 1.0 / 200.0;        // backlog smoothing for the meters (2 s)
    constexpr double kStallSeconds = 0.1;       // no reference write for this long: the speakers' stream stalled

    double BesselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }
}

ReferenceSync::ReferenceSync()
{
    // Row p holds the taps for a fractional position p / kPhases; tap j sits at offset j - (kHalf - 1).
    constexpr double beta = 8.0;
    const double i0b = BesselI0(beta);
    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        double h[kTaps];
        for (int j = 0; j < kTaps; ++j)
        {
            const double x = static_cast<double>(j - (kHalf - 1)) - frac;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            const double r = x / kHalf;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : BesselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            h[j] = sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < kTaps; ++j) m_table[p][j] = static_cast<float>(h[j] / sum);   // unity gain at DC
    }
    Reset();
}

void ReferenceSync::Reset() noexcept
{
    m_buf.fill(0.0f);
    m_len = 0;
    m_pos = 0.0;
    m_state = State::Waiting;
    m_settleBlocks = 0;
    m_settleSum = 0.0;
    m_target = kStartCap;
    m_fillEma = 0.0;
    m_ratePpb.store(0);
    m_ratioPpm = 0.0;
    m_resyncs = m_dry = 0;
    m_dryRun = 0;
    m_lastStampW = 0;
    m_maxChunk = 0;
}

double ReferenceSync::Backlog(const ReferenceRing& ring, int64_t nowTicks, int64_t freq) const noexcept
{
    const double local = static_cast<double>(m_len) - m_pos;   // pulled from the ring, not yet consumed
    size_t written = 0;
    int64_t ticks = 0;
    if (nowTicks != 0 && freq > 0 && ring.LastStamp(written, ticks))
    {
        // Reference written "by now": the last write plus the time since, so the backlog moves smoothly
        // instead of in 10 ms steps. The extrapolation stops after a stall.
        const double since = std::clamp(static_cast<double>(nowTicks - ticks) / static_cast<double>(freq), 0.0, kStallSeconds);
        return static_cast<double>(written) + since * kRate - static_cast<double>(ring.ReadCount()) + local;
    }
    return static_cast<double>(ring.Available()) + local;
}

void ReferenceSync::Restart(ReferenceRing& ring) noexcept
{
    ring.TrimTo(kStartCap);
    // kHalf - 1 samples of silent history, so the first output is exactly the first reference sample.
    m_buf.fill(0.0f);
    m_len = kHalf - 1;
    m_pos = static_cast<double>(kHalf - 1);
    m_state = State::Settling;
    m_settleBlocks = 0;
    m_settleSum = 0.0;
}

bool ReferenceSync::Fill(ReferenceRing& ring, size_t upTo) noexcept
{
    if (upTo <= m_len) return true;
    const size_t need = upTo - m_len;
    if (upTo > kBuf || !ring.Read(&m_buf[m_len], need)) return false;
    m_len = upTo;
    return true;
}

bool ReferenceSync::Pull(ReferenceRing& ring, float* out, int n, int64_t nowTicks, int64_t freq) noexcept
{
    auto silence = [&] {
        std::fill(out, out + n, 0.0f);
        ++m_dry;
        return false;
    };
    if (m_state == State::Waiting)
    {
        if (ring.Available() < static_cast<size_t>(n))
            return silence();
        Restart(ring);
    }

    const double ratio = 1.0 + RatePpm() * 1e-6;
    const double backlog = Backlog(ring, nowTicks, freq);
    size_t written = 0;
    int64_t ticks = 0;
    const bool stamped = nowTicks != 0 && freq > 0 && ring.LastStamp(written, ticks);
    const bool stalled = stamped && static_cast<double>(nowTicks - ticks) / static_cast<double>(freq) > kStallSeconds;
    if (stamped)
    {
        // Largest write seen between two of our reads (an over-estimate when writes bunch up: that is safe).
        if (m_lastStampW != 0 && written > m_lastStampW)
            m_maxChunk = std::max(m_maxChunk, static_cast<int>(std::min<size_t>(written - m_lastStampW, kMaxTarget)));
        m_lastStampW = written;
    }
    if (m_state == State::Settling)
    {
        // The second half only: the first blocks still carry the start-up transient.
        if (++m_settleBlocks > kSettleBlocks / 2) m_settleSum += backlog;
        if (m_settleBlocks >= kSettleBlocks)
        {
            int target = static_cast<int>(std::lround(m_settleSum / (kSettleBlocks - kSettleBlocks / 2)));
            if (stamped)   // the stamped backlog counts reference not yet delivered: keep a whole chunk + a block spare
                target = std::max(target, m_maxChunk + n + kHalf + kRate / 200);
            m_target = std::clamp(target, kMinTarget, kMaxTarget);
            m_fillEma = m_target;
            m_state = State::Running;
        }
    }
    else if (!stalled)
    {
        if (backlog > m_target + kResyncSamples)
        {
            // A burst or a capture stall: drop straight back to the target instead of slewing for minutes.
            const size_t avail = ring.Available();
            const size_t drop = std::min(avail, static_cast<size_t>(backlog - m_target));
            ring.TrimTo(avail - drop);
            m_fillEma = backlog - static_cast<double>(drop);
            ++m_resyncs;
        }
        else if (backlog < m_target - kResyncSamples)
        {
            ++m_resyncs;
            return silence();   // the reference fell far behind: wait for it (consume nothing)
        }
        else
            m_fillEma += kEma * (backlog - m_fillEma);   // inside the band: the rate alone decides (no chasing)
    }
    m_ratioPpm = (ratio - 1.0) * 1e6;

    // Make room (keep kHalf - 1 samples of history before the read position).
    if (m_pos > 2048.0)
    {
        const size_t shift = static_cast<size_t>(m_pos) - (kHalf - 1);
        std::memmove(m_buf.data(), m_buf.data() + shift, sizeof(float) * (m_len - shift));
        m_len -= shift;
        m_pos -= static_cast<double>(shift);
    }
    const size_t last = static_cast<size_t>(m_pos + (n - 1) * ratio) + kHalf;
    if (!Fill(ring, last + 1))
    {
        // Ran dry. Once: the backlog was too small for the delivery jitter, allow one block more from now on.
        // Several blocks in a row: the speakers' stream stalled; start over when it comes back.
        if (m_dryRun++ == 0 && m_state == State::Running)
            m_target = std::min(m_target + 480, kMaxTarget);
        else if (m_dryRun >= 5)
        {
            m_state = State::Waiting;
            m_dryRun = 0;
        }
        return silence();
    }
    m_dryRun = 0;

    for (int i = 0; i < n; ++i)
    {
        const size_t idx = static_cast<size_t>(m_pos);
        const double ph = (m_pos - static_cast<double>(idx)) * kPhases;
        const int p = std::min(static_cast<int>(ph), kPhases - 1);
        const float t = static_cast<float>(ph - p);
        const float* a = m_table[p].data();
        const float* b = m_table[p + 1].data();
        const float* x = &m_buf[idx - (kHalf - 1)];
        float acc = 0.0f;
        for (int j = 0; j < kTaps; ++j) acc += (a[j] + t * (b[j] - a[j])) * x[j];
        out[i] = acc;
        m_pos += ratio;
    }
    return true;
}
