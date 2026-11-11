// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>

// Live meters published by the audio effect (inside audiodg) for the settings tray app.
// A named, read-only (for Users) shared-memory section; the audio thread only writes plain fields
// guarded by a sequence counter (odd = write in progress). No locks, no allocation on the RT path.
namespace telemetry
{
    inline constexpr wchar_t kSectionName[] = L"Global\\WebrtcAudioEngineTelemetry";
    inline constexpr uint32_t kVersion = 1;

    struct Data
    {
        volatile uint32_t version;
        volatile uint32_t seq;          // odd while the writer updates the fields below
        volatile uint64_t tickMs;       // GetTickCount64 of the last update (staleness check)
        volatile float inputDbfs;       // mono input RMS (after the slider gain), last ~100 ms
        volatile float outputDbfs;      // processed voice RMS
        volatile float referenceDbfs;   // far-end (loudspeaker) reference RMS; -120 if none
        volatile float echoRemovedDb;   // AEC3 ERLE (echo return loss enhancement)
        volatile float agcGainDb;       // current AGC2 gain
        volatile float limiterDb;       // limiter gain reduction
        volatile uint32_t aecRunning;   // 1 = this stream has an echo reference and AEC3
        volatile uint32_t sampleRate;
    };

    // Writer side (audio effect). Open/Create are non-RT; Publish is RT-safe.
    Data* OpenWriter() noexcept;
    void Publish(Data* d, const Data& values) noexcept;

    // Reader side (app). Returns false if no effect is running or the data is older than maxAgeMs.
    bool Read(Data& out, uint64_t maxAgeMs = 1000) noexcept;
}
