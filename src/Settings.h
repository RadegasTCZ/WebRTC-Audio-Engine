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
}
