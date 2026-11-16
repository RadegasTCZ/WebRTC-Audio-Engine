// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#include "Settings.h"
#include "Log.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<Settings>);

namespace
{
    // One seqlock snapshot for the defaults (index -1 -> slot 0) and one per registered device.
    // Single writer (registration / watcher callback), many lock-free readers.
    constexpr size_t kWords = sizeof(Settings) / sizeof(uint32_t);
    static_assert(sizeof(Settings) % sizeof(uint32_t) == 0);
    struct Snapshot
    {
        std::atomic<uint32_t> seq{0};
        std::atomic<uint32_t> words[kWords];
        std::atomic<bool> valid{false};
        wchar_t device[64]{};  // endpoint GUID ("{...}"); empty for the defaults slot
    };
    Snapshot g_snap[1 + settings::kMaxDevices];  // [0] = defaults
    SRWLOCK g_snapLock = SRWLOCK_INIT;            // guards device registration (non-RT only)
    SRWLOCK g_changeLock = SRWLOCK_INIT;          // one OnChanged at a time (thread-pool callbacks can overlap)

    std::atomic<bool> g_watcherStarted{false};
    std::atomic<settings::ChangeHook> g_hook{nullptr};
    HKEY g_key = nullptr;
    HANDLE g_event = nullptr;
    HANDLE g_wait = nullptr;

    void Store(Snapshot& snap, const Settings& s) noexcept
    {
        uint32_t words[kWords];
        std::memcpy(words, &s, sizeof(s));
        const uint32_t seq = snap.seq.load(std::memory_order_relaxed);
        snap.seq.store(seq + 1, std::memory_order_relaxed);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i)
            snap.words[i].store(words[i], std::memory_order_relaxed);
        snap.seq.store(seq + 2, std::memory_order_release);  // even: stable
        snap.valid.store(true, std::memory_order_release);
    }

    Settings Load(const Snapshot& snap) noexcept
    {
        if (!snap.valid.load(std::memory_order_acquire))
            return Settings{};
        // Bounded: the real-time thread must never spin on a writer that got preempted mid-update. If the
        // retries run out, the words are a mix of the old and the new snapshot; both were sanitised field
        // by field, so every value is still in range and the next block reads a clean copy.
        uint32_t words[kWords];
        for (int tries = 0; tries < 64; ++tries)
        {
            const uint32_t s1 = snap.seq.load(std::memory_order_acquire);
            for (size_t i = 0; i < kWords; ++i)
                words[i] = snap.words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (!(s1 & 1u) && snap.seq.load(std::memory_order_relaxed) == s1)
                break;
            YieldProcessor();
        }
        Settings s;
        std::memcpy(&s, words, sizeof(s));
        return s;
    }

    // Fixed buffer, no allocation: these paths are built inside noexcept functions that run in audiodg.
    struct DevicePath
    {
        wchar_t path[160];
        explicit DevicePath(const wchar_t* device) noexcept
        {
            path[0] = 0;
            swprintf(path, std::size(path), L"%s\\Devices\\%.64s", settings::kRegistryPath, device);
        }
        const wchar_t* c_str() const noexcept { return path; }
    };

    // Devices\{guid} subkeys can be created by any user, so one could be a registry link pointing at
    // another key. REG_OPTION_OPEN_LINK opens the link itself; we then refuse it instead of writing there.
    LONG CreateDeviceKey(const wchar_t* device, HKEY* key) noexcept
    {
        *key = nullptr;
        LONG err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, DevicePath(device).c_str(), 0, nullptr, REG_OPTION_OPEN_LINK,
                                   KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, key, nullptr);
        if (err != ERROR_SUCCESS)
            return err;
        DWORD type = 0;
        if (RegQueryValueExW(*key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS && type == REG_LINK)
        {
            RegCloseKey(*key);
            *key = nullptr;
            return ERROR_ACCESS_DENIED;
        }
        return ERROR_SUCCESS;
    }

    // Applies every value present under `path` onto `s` (missing values keep what `s` has).
    void ReadValues(const wchar_t* path, Settings& s) noexcept
    {
        auto read = [path](const wchar_t* name, uint32_t& value) {
            DWORD v = 0, size = sizeof(v);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
                value = v;
        };
        auto readInt = [&](const wchar_t* name, int32_t& value) {
            uint32_t v = static_cast<uint32_t>(value);
            read(name, v);
            value = static_cast<int32_t>(v);
        };
        read(L"Bypass", s.bypass);
        read(L"HpfEnabled", s.hpfEnabled);   // older key; CleanupFilters below wins when present
        read(L"HpfHz", s.hpfHz);
        uint32_t cleanup = 0xFFFFFFFFu;
        read(L"CleanupFilters", cleanup);
        if (cleanup <= 3)
        {
            s.hpfEnabled = (cleanup & 1u) ? 1u : 0u;
            s.lpfEnabled = (cleanup & 2u) ? 1u : 0u;
        }
        read(L"LpfHz", s.lpfHz);
        read(L"NsEnabled", s.nsEnabled);
        read(L"NsLevel", s.nsLevel);
        read(L"GainMode", s.gainMode);
        read(L"AgcEnabled", s.agcEnabled);
        readInt(L"AgcHeadroomDb", s.agcHeadroomDb);
        readInt(L"AgcMaxGainDb", s.agcMaxGainDb);
        readInt(L"AgcSpeedDbPerSec", s.agcSpeedDbPerSec);
        readInt(L"AgcNoiseCeilingDbfs", s.agcNoiseCeilingDbfs);
        read(L"LimiterEnabled", s.limiterEnabled);
        read(L"AecEnabled", s.aecEnabled);
        read(L"AecStrength", s.aecStrength);
        read(L"AecRoom", s.aecRoom);
    }

    Settings Sanitised(Settings s) noexcept
    {
        s.bypass = s.bypass ? 1u : 0u;
        s.hpfEnabled = s.hpfEnabled ? 1u : 0u;
        s.lpfEnabled = s.lpfEnabled ? 1u : 0u;
        s.nsEnabled = s.nsEnabled ? 1u : 0u;
        s.agcEnabled = s.agcEnabled ? 1u : 0u;
        s.limiterEnabled = s.limiterEnabled ? 1u : 0u;
        s.aecEnabled = s.aecEnabled ? 1u : 0u;
        s.hpfHz = std::clamp<uint32_t>(s.hpfHz, 20, 300);
        s.lpfHz = std::clamp<uint32_t>(s.lpfHz, 8000, 16000);
        if (s.nsLevel > 3) s.nsLevel = 1;
        if (s.gainMode > 2) s.gainMode = 0;
        if (s.aecStrength > 2) s.aecStrength = 1;
        if (s.aecRoom > 2) s.aecRoom = 0;
        s.agcHeadroomDb = std::clamp(s.agcHeadroomDb, 0, 20);
        s.agcMaxGainDb = std::clamp(s.agcMaxGainDb, 1, 50);
        s.agcSpeedDbPerSec = std::clamp(s.agcSpeedDbPerSec, 1, 30);
        s.agcNoiseCeilingDbfs = std::clamp(s.agcNoiseCeilingDbfs, -80, -20);
        return s;
    }

    void Log(const wchar_t* who, const Settings& s) noexcept
    {
        apolog::Write(L"[settings %s] bypass=%u lpf=%u@%uHz hpf=%u@%uHz ns=%u level=%u gainMode=%u agc=%u(%d/%d/%d/%d) limiter=%u aec=%u strength=%u room=%u",
                      who, s.bypass, s.lpfEnabled, s.lpfHz, s.hpfEnabled, s.hpfHz, s.nsEnabled, s.nsLevel, s.gainMode, s.agcEnabled,
                      s.agcHeadroomDb, s.agcMaxGainDb, s.agcSpeedDbPerSec, s.agcNoiseCeilingDbfs, s.limiterEnabled, s.aecEnabled,
                      s.aecStrength, s.aecRoom);
    }

    std::atomic<bool> g_keyLost{false};  // the watched key was deleted: reopen it when it exists again

    bool Arm() noexcept
    {
        // Watch the whole tree: per-device subkeys live under Devices\. THREAD_AGNOSTIC: we arm from
        // thread-pool threads, which may exit (without it, every thread exit fires a spurious change).
        return g_key &&
               RegNotifyChangeKeyValue(g_key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC,
                                       g_event, TRUE) == ERROR_SUCCESS;
    }

    // With g_changeLock held. The key was deleted (reinstall, a user): watch the new one if it exists.
    bool Reopen() noexcept
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        if (g_key)
            RegCloseKey(g_key);
        g_key = key;
        return Arm();
    }

    void CALLBACK OnChanged(PVOID, BOOLEAN) noexcept
    {
        // Re-arm first, then read: a value written while we read fires the next notification instead of
        // being missed (writers set several values in a row; the first one wakes us up).
        // Callbacks can overlap (the re-arm above can fire while this one runs): Store() has a single
        // writer per snapshot, and the hook must see changes in order.
        AcquireSRWLockExclusive(&g_changeLock);
        // Coalesce bursts (the app writes ~17 values per change; a script may flip values in a loop): every
        // value written during this pause is picked up by the one read below, so stages are rebuilt once.
        Sleep(150);
        if (!Arm() && !Reopen())
        {
            if (!g_keyLost.exchange(true))
                apolog::Write(L"[settings] settings key deleted: live updates paused until it exists again");
        }
        else if (g_keyLost.exchange(false))
            apolog::Write(L"[settings] settings key back: live updates resumed");
        bool any = false;
        AcquireSRWLockShared(&g_snapLock);
        for (int i = 0; i <= settings::kMaxDevices; ++i)
        {
            Snapshot& snap = g_snap[i];
            if (i > 0 && snap.device[0] == 0)
                continue;
            const Settings s = Settings::ReadFromRegistry(i == 0 ? nullptr : snap.device);
            const Settings old = Load(snap);
            Store(snap, s);
            if (!(s == old))
            {
                any = true;
                Log(i == 0 ? L"default" : snap.device, s);
            }
        }
        ReleaseSRWLockShared(&g_snapLock);
        if (any)
            if (auto hook = g_hook.load())
            {
                // The hook rebuilds processing stages; out of memory there must not end audiodg.
                try { hook(settings::Current()); }
                catch (...) { apolog::Write(L"[settings] out of memory while applying new settings (kept the old ones)"); }
            }
        ReleaseSRWLockExclusive(&g_changeLock);
    }
}

bool Settings::IsDeviceCustom(const wchar_t* device) noexcept
{
    if (!device || !*device)
        return false;
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, DevicePath(device).c_str(), L"Custom", RRF_RT_REG_DWORD, nullptr, &v, &size) ==
               ERROR_SUCCESS &&
           v != 0;
}

Settings Settings::ReadFromRegistry(const wchar_t* device) noexcept
{
    Settings s;
    ReadValues(settings::kRegistryPath, s);           // defaults for every microphone
    if (IsDeviceCustom(device))
        ReadValues(DevicePath(device).c_str(), s);    // this microphone's own values
    return Sanitised(s);
}

LONG Settings::WriteToRegistry(const Settings& s, const wchar_t* device) noexcept
{
    HKEY key = nullptr;
    LONG err;
    if (device && *device)
        // Users / the audio engine may create subkeys under Devices (install scripts set the ACL).
        err = CreateDeviceKey(device, &key);
    else
        err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, 0, KEY_SET_VALUE, &key);
    if (err != ERROR_SUCCESS)
        return err;
    auto put = [&](const wchar_t* name, uint32_t v) {
        DWORD d = v;
        const LONG e = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), sizeof(d));
        if (e != ERROR_SUCCESS) err = e;
    };
    put(L"Bypass", s.bypass);
    put(L"CleanupFilters", (s.hpfEnabled ? 1u : 0u) | (s.lpfEnabled ? 2u : 0u));
    put(L"HpfEnabled", s.hpfEnabled);
    put(L"HpfHz", s.hpfHz);
    put(L"LpfHz", s.lpfHz);
    put(L"NsEnabled", s.nsEnabled);
    put(L"NsLevel", s.nsLevel);
    put(L"GainMode", s.gainMode);
    put(L"AgcEnabled", s.agcEnabled);
    put(L"AgcHeadroomDb", static_cast<uint32_t>(s.agcHeadroomDb));
    put(L"AgcMaxGainDb", static_cast<uint32_t>(s.agcMaxGainDb));
    put(L"AgcSpeedDbPerSec", static_cast<uint32_t>(s.agcSpeedDbPerSec));
    put(L"AgcNoiseCeilingDbfs", static_cast<uint32_t>(s.agcNoiseCeilingDbfs));
    put(L"LimiterEnabled", s.limiterEnabled);
    put(L"AecEnabled", s.aecEnabled);
    put(L"AecStrength", s.aecStrength);
    put(L"AecRoom", s.aecRoom);
    RegCloseKey(key);
    return err;
}

LONG Settings::SetDeviceCustom(const wchar_t* device, bool custom, const wchar_t* name) noexcept
{
    if (!device || !*device)
        return ERROR_INVALID_PARAMETER;
    if (custom && !IsDeviceCustom(device))
    {
        // Start from what the microphone uses right now (the defaults).
        const LONG e = WriteToRegistry(ReadFromRegistry(nullptr), device);
        if (e != ERROR_SUCCESS)
            return e;
    }
    HKEY key = nullptr;
    LONG err = CreateDeviceKey(device, &key);
    if (err != ERROR_SUCCESS)
        return err;
    const DWORD v = custom ? 1u : 0u;
    err = RegSetValueExW(key, L"Custom", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    if (name && *name)
        RegSetValueExW(key, L"Name", 0, REG_SZ, reinterpret_cast<const BYTE*>(name),
                       static_cast<DWORD>((wcslen(name) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return err;
}

namespace settings
{
    void Publish(const Settings& s) noexcept { Store(g_snap[0], s); }

    Settings Current(int deviceIndex) noexcept
    {
        if (deviceIndex >= 0 && deviceIndex < kMaxDevices && g_snap[1 + deviceIndex].valid.load(std::memory_order_acquire))
            return Load(g_snap[1 + deviceIndex]);
        return Load(g_snap[0]);
    }

    int RegisterDevice(const wchar_t* device) noexcept
    {
        if (!device || !*device)
            return -1;
        AcquireSRWLockExclusive(&g_snapLock);
        int idx = -1;
        for (int i = 0; i < kMaxDevices && idx < 0; ++i)
            if (_wcsicmp(g_snap[1 + i].device, device) == 0)
                idx = i;
        for (int i = 0; i < kMaxDevices && idx < 0; ++i)
            if (g_snap[1 + i].device[0] == 0)
            {
                wcsncpy_s(g_snap[1 + i].device, device, _TRUNCATE);
                idx = i;
            }
        if (idx >= 0)
            Store(g_snap[1 + idx], Settings::ReadFromRegistry(device));
        ReleaseSRWLockExclusive(&g_snapLock);
        return idx;  // -1: table full -> this device follows the defaults
    }

    bool WatcherStarted() noexcept { return g_watcherStarted.load(); }

    void SetChangeHook(ChangeHook hook) noexcept { g_hook.store(hook); }

    void EnsureWatcher() noexcept
    {
        bool expected = false;
        if (!g_watcherStarted.compare_exchange_strong(expected, true))
        {
            // Running, but the key was deleted earlier: watch the new one and re-read everything.
            if (g_keyLost.load())
            {
                AcquireSRWLockExclusive(&g_changeLock);
                const bool ok = g_keyLost.load() && Reopen();
                ReleaseSRWLockExclusive(&g_changeLock);
                if (ok && g_event)
                    SetEvent(g_event);  // OnChanged re-reads and logs "resumed"
            }
            return;
        }
        Publish(Settings::ReadFromRegistry(nullptr));
        // The key may not exist yet (defaults apply); then we only have the initial snapshot.
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &g_key) != ERROR_SUCCESS)
        {
            g_key = nullptr;
            apolog::Write(L"[settings] key missing, using defaults (no live updates until next stream)");
            g_watcherStarted.store(false);  // retry on the next stream start
            return;
        }
        g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_event)
            Arm();
        // Lives for the rest of the process; the DLL is then never unloaded (DllCanUnloadNow).
        if (!g_event || !RegisterWaitForSingleObject(&g_wait, g_event, OnChanged, nullptr, INFINITE, WT_EXECUTEDEFAULT))
        {
            apolog::Write(L"[settings] could not start the live watcher (error %lu): retrying at the next stream", GetLastError());
            if (g_event) { CloseHandle(g_event); g_event = nullptr; }
            RegCloseKey(g_key);
            g_key = nullptr;
            g_wait = nullptr;
            g_watcherStarted.store(false);
            return;
        }
        apolog::Write(L"[settings] live watcher started");
    }
}
