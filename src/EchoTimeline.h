// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

// How much echo AEC3 removed over a recording, for the log: one value per 10 s step (the average ERLE
// while something was playing), up to 2 hours, plus min / average / max over the whole recording.
// Add() is real-time safe (no allocation, no locks); Report() runs after the stream stopped.
class EchoTimeline
{
public:
    static constexpr int kMetersPerStep = 100;   // meters arrive every 100 ms -> 10 s steps
    static constexpr int kMaxSteps = 720;        // 2 hours
    static constexpr float kPlayingDbfs = -60.0f; // reference above this = something is playing
    static constexpr int16_t kQuiet = -1;        // step with (almost) nothing playing

    void Reset() noexcept
    {
        m_steps = 0;
        m_totalSteps = 0;
        m_meters = m_active = 0;
        m_sum = 0;
        m_min = 1e9f;
        m_max = -1e9f;
        m_overallSum = 0;
        m_overallSteps = 0;
    }

    // Real-time: one call per meter update (~100 ms).
    void Add(float erleDb, float referenceDbfs, bool aecRunning) noexcept
    {
        if (aecRunning && std::isfinite(erleDb) && std::isfinite(referenceDbfs) && referenceDbfs > kPlayingDbfs)
        {
            m_sum += std::clamp(erleDb, 0.0f, 99.0f);
            ++m_active;
        }
        if (++m_meters < kMetersPerStep)
            return;
        // A step counts when something played for at least 3 of its 10 seconds.
        int16_t value = kQuiet;
        if (m_active >= kMetersPerStep * 3 / 10)
        {
            const float avg = m_sum / static_cast<float>(m_active);
            value = static_cast<int16_t>(std::lround(avg));
            m_min = std::min(m_min, avg);
            m_max = std::max(m_max, avg);
            m_overallSum += avg;
            ++m_overallSteps;
        }
        if (m_steps < kMaxSteps)
            m_values[static_cast<size_t>(m_steps++)] = value;
        ++m_totalSteps;
        m_meters = m_active = 0;
        m_sum = 0;
    }

    bool HasData() const noexcept { return m_totalSteps > 0; }

    // Non-real-time: calls write(line) for a summary line and one line per 5 minutes:
    //   "  05:00  32 35 38  .  . 40 ..."   (dB removed per 10 s; "." = nothing playing)
    template <class Write>
    void Report(Write&& write) const
    {
        char line[256];
        if (m_overallSteps > 0)
            std::snprintf(line, sizeof(line),
                          "echo removed while playing: avg %.1f dB, min %.1f, max %.1f over %d of %d steps of 10 s%s",
                          m_overallSum / m_overallSteps, m_min, m_max, m_overallSteps, m_totalSteps,
                          m_totalSteps > m_steps ? " (timeline shows the first 2 h)" : "");
        else
            std::snprintf(line, sizeof(line), "echo removed: nothing played during %d steps of 10 s", m_totalSteps);
        write(line);
        if (m_overallSteps == 0)
            return;
        constexpr int kPerLine = 30;  // 5 minutes
        for (int start = 0; start < m_steps; start += kPerLine)
        {
            int n = std::snprintf(line, sizeof(line), "  %02d:%02d ", start * 10 / 60, start * 10 % 60);
            for (int i = start; i < std::min(start + kPerLine, m_steps) && n < static_cast<int>(sizeof(line)) - 4; ++i)
            {
                const int16_t v = m_values[static_cast<size_t>(i)];
                n += v == kQuiet ? std::snprintf(line + n, sizeof(line) - n, "  .")
                                 : std::snprintf(line + n, sizeof(line) - n, "%3d", static_cast<int>(v));
            }
            write(line);
        }
    }

private:
    std::array<int16_t, kMaxSteps> m_values{};
    int m_steps = 0, m_totalSteps = 0;
    int m_meters = 0, m_active = 0;
    float m_sum = 0;
    float m_min = 1e9f, m_max = -1e9f;
    float m_overallSum = 0;
    int m_overallSteps = 0;
};
