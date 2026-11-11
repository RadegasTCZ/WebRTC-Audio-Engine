#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

// User settings, stored as DWORDs in HKLM\SOFTWARE\WebrtcVoiceApo (key ACL lets Users write,
// so the settings app needs no elevation). A watcher in audiodg re-reads the key whenever it
// changes and publishes a snapshot that the real-time thread reads lock-free (seqlock).
struct Settings
{
    uint32_t bypass = 0;        // 1 = whole chain bypassed (latency kept constant)
    uint32_t hpfEnabled = 1;    // high-pass filter, first in the chain
    uint32_t hpfHz = 80;        // 20..300
    uint32_t nsEnabled = 1;     // WebRTC noise suppression
    uint32_t nsLevel = 1;       // 0 = 6 dB, 1 = 12 dB, 2 = 18 dB, 3 = 21 dB
    uint32_t gainMode = 0;      // mic volume slider in software (effect pack only):
                                // 0 = auto (not for USB/Bluetooth, whose volume is in hardware), 1 = always, 2 = never
                                // While AGC2 is on, the slider gain is not applied (AGC sets the level).
    uint32_t agcEnabled = 0;    // WebRTC AGC2 adaptive digital gain (after NS)
    int32_t agcHeadroomDb = 5;      // 0..20      presets: gentle 8 / normal 5 / strong 3
    int32_t agcMaxGainDb = 30;      // 1..50      presets: gentle 20 / normal 30 / strong 50
    int32_t agcSpeedDbPerSec = 6;   // 1..30      presets: gentle 3 / normal 6 / strong 10
    int32_t agcNoiseCeilingDbfs = -50; // -80..-20 presets: gentle -60 / normal -50 / strong -45
    uint32_t limiterEnabled = 1;    // -1 dBFS safety limiter, last in the chain
    uint32_t aecEnabled = 1;        // WebRTC AEC3 (Communications streams with an echo reference)
    uint32_t aecStrength = 1;       // 0 = gentle, 1 = normal, 2 = strong (AEC3-Echo-Canceller presets)

    bool operator==(const Settings&) const = default;

    static Settings ReadFromRegistry() noexcept;  // missing values keep their defaults; clamps ranges
};

namespace settings
{
    inline constexpr wchar_t kRegistryPath[] = L"SOFTWARE\\WebrtcVoiceApo";

    // Starts the registry watcher once per process (idempotent, non-RT callers only).
    void EnsureWatcher() noexcept;
    bool WatcherStarted() noexcept;

    // Lock-free read of the latest snapshot (safe on the real-time thread).
    Settings Current() noexcept;

    // For tests: publish a snapshot directly.
    void Publish(const Settings& s) noexcept;

    // Called on the watcher thread (non-RT) after each change, e.g. to rebuild stages that allocate.
    using ChangeHook = void (*)(const Settings&);
    void SetChangeHook(ChangeHook hook) noexcept;
}
