// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>

// Live meters published by the audio effect (inside audiodg) for the settings tray app.
// A named, read-only (for Users) shared-memory section with one slot per microphone (several devices
// can use the effect pack at once). Each slot is written by the audio thread of that device's effect:
// plain fields guarded by a sequence counter (odd = write in progress). No locks, no allocation on the
// real-time path; slots are claimed on non-real-time threads.
namespace telemetry
{
    inline constexpr wchar_t kSectionName[] = L"Global\\WebrtcAudioEngineTelemetry4";
    inline constexpr uint32_t kVersion = 4;
    inline constexpr int kMaxSlots = 8;
    inline constexpr uint64_t kSlotStaleMs = 5000;  // a slot not updated for this long may be reused

    struct Values
    {
        float inputDbfs = -120;       // mono input RMS (after the slider gain), last ~100 ms
        float outputDbfs = -120;      // processed voice RMS
        float referenceDbfs = -120;   // far-end (loudspeaker) reference RMS; -120 if none
        float echoRemovedDb = 0;      // AEC3 ERLE (echo return loss enhancement)
        float agcGainDb = 0;          // current AGC2 gain
        float limiterDb = 0;          // limiter gain reduction
        uint32_t aecRunning = 0;      // 1 = this stream has an echo reference and AEC3
        uint32_t sampleRate = 0;
        float speech = 0;             // mean speech probability 0..1 over the last ~100 ms (AGC2 voice detector)
    };

    struct Slot
    {
        volatile LONG64 key;          // hash of the endpoint id; 0 = free
        volatile uint32_t seq;        // odd while the writer updates
        volatile uint64_t tickMs;     // GetTickCount64 of the last update
        wchar_t name[64];             // endpoint friendly name (written when claimed)
        wchar_t device[64];           // endpoint GUID "{...}" (per-device settings key)
        Values values;
    };

    struct Section
    {
        volatile uint32_t version;
        Slot slots[kMaxSlots];
    };

    struct Device
    {
        wchar_t name[64];
        wchar_t device[64];
        Values values;
    };

    // Writer side (audio effect). Claim is non-RT; Publish is RT-safe.
    Slot* ClaimSlot(const wchar_t* endpointId, const wchar_t* friendlyName, const wchar_t* deviceGuid) noexcept;
    void Publish(Slot* slot, const Values& values) noexcept;

    // Reader side (app): fills up to kMaxSlots devices updated within maxAgeMs; returns the count.
    int ReadAll(Device (&out)[kMaxSlots], uint64_t maxAgeMs = 1500) noexcept;
}
