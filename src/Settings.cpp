#include "Settings.h"
#include "Log.h"
#include <algorithm>
#include <cstring>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<Settings>);

namespace
{
    // Seqlock: single writer (watcher callback), many lock-free readers.
    // Fields are stored as relaxed atomics so concurrent reads are well-defined.
    constexpr size_t kWords = sizeof(Settings) / sizeof(uint32_t);
    static_assert(sizeof(Settings) % sizeof(uint32_t) == 0);
    std::atomic<uint32_t> g_seq{0};
    std::atomic<uint32_t> g_words[kWords];
    std::atomic<bool> g_initialised{false};

    std::atomic<bool> g_watcherStarted{false};
    std::atomic<settings::ChangeHook> g_hook{nullptr};
    HKEY g_key = nullptr;
    HANDLE g_event = nullptr;
    HANDLE g_wait = nullptr;

    void Arm() noexcept
    {
        if (g_key)
            RegNotifyChangeKeyValue(g_key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, g_event, TRUE);
    }

    void CALLBACK OnChanged(PVOID, BOOLEAN) noexcept
    {
        const Settings s = Settings::ReadFromRegistry();
        const Settings old = settings::Current();
        settings::Publish(s);
        if (!(s == old))
        {
            apolog::Write(L"[settings] bypass=%u hpf=%u@%uHz ns=%u level=%u gainMode=%u agc=%u(%d/%d/%d/%d) limiter=%u aec=%u strength=%u",
                          s.bypass, s.hpfEnabled, s.hpfHz, s.nsEnabled, s.nsLevel, s.gainMode, s.agcEnabled, s.agcHeadroomDb,
                          s.agcMaxGainDb, s.agcSpeedDbPerSec, s.agcNoiseCeilingDbfs, s.limiterEnabled, s.aecEnabled, s.aecStrength);
            if (auto hook = g_hook.load())
                hook(s);
        }
        Arm();
    }
}

Settings Settings::ReadFromRegistry() noexcept
{
    Settings s;
    auto read = [](const wchar_t* name, uint32_t& value) {
        DWORD v = 0, size = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, settings::kRegistryPath, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
            value = v;
    };
    read(L"Bypass", s.bypass);
    read(L"HpfEnabled", s.hpfEnabled);
    read(L"HpfHz", s.hpfHz);
    read(L"NsEnabled", s.nsEnabled);
    read(L"NsLevel", s.nsLevel);
    read(L"GainMode", s.gainMode);
    if (s.gainMode > 2) s.gainMode = 0;
    auto readInt = [&](const wchar_t* name, int32_t& value, int32_t lo, int32_t hi) {
        uint32_t v = static_cast<uint32_t>(value);
        read(name, v);
        value = std::clamp(static_cast<int32_t>(v), lo, hi);
    };
    read(L"AgcEnabled", s.agcEnabled);
    readInt(L"AgcHeadroomDb", s.agcHeadroomDb, 0, 20);
    readInt(L"AgcMaxGainDb", s.agcMaxGainDb, 1, 50);
    readInt(L"AgcSpeedDbPerSec", s.agcSpeedDbPerSec, 1, 30);
    readInt(L"AgcNoiseCeilingDbfs", s.agcNoiseCeilingDbfs, -80, -20);
    read(L"LimiterEnabled", s.limiterEnabled);
    read(L"AecEnabled", s.aecEnabled);
    read(L"AecStrength", s.aecStrength);
    s.aecEnabled = s.aecEnabled ? 1u : 0u;
    if (s.aecStrength > 2) s.aecStrength = 1;
    s.agcEnabled = s.agcEnabled ? 1u : 0u;
    s.limiterEnabled = s.limiterEnabled ? 1u : 0u;

    s.bypass = s.bypass ? 1u : 0u;
    s.hpfEnabled = s.hpfEnabled ? 1u : 0u;
    s.nsEnabled = s.nsEnabled ? 1u : 0u;
    s.hpfHz = std::clamp<uint32_t>(s.hpfHz, 20, 300);
    if (s.nsLevel > 3) s.nsLevel = 1;
    return s;
}

namespace settings
{
    void Publish(const Settings& s) noexcept
    {
        uint32_t words[kWords];
        std::memcpy(words, &s, sizeof(s));
        const uint32_t seq = g_seq.load(std::memory_order_relaxed);
        g_seq.store(seq + 1, std::memory_order_relaxed);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i)
            g_words[i].store(words[i], std::memory_order_relaxed);
        g_seq.store(seq + 2, std::memory_order_release);  // even: stable
        g_initialised.store(true, std::memory_order_release);
    }

    Settings Current() noexcept
    {
        if (!g_initialised.load(std::memory_order_acquire))
            return Settings{};
        uint32_t words[kWords];
        for (;;)
        {
            const uint32_t s1 = g_seq.load(std::memory_order_acquire);
            if (s1 & 1u)
                continue;  // writer active; it finishes within microseconds
            for (size_t i = 0; i < kWords; ++i)
                words[i] = g_words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_seq.load(std::memory_order_relaxed) == s1)
                break;
        }
        Settings s;
        std::memcpy(&s, words, sizeof(s));
        return s;
    }

    bool WatcherStarted() noexcept { return g_watcherStarted.load(); }

    void SetChangeHook(ChangeHook hook) noexcept { g_hook.store(hook); }

    void EnsureWatcher() noexcept
    {
        bool expected = false;
        if (!g_watcherStarted.compare_exchange_strong(expected, true))
            return;
        Publish(Settings::ReadFromRegistry());
        // The key may not exist yet (defaults apply); then we only have the initial snapshot.
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &g_key) != ERROR_SUCCESS)
        {
            g_key = nullptr;
            apolog::Write(L"[settings] key missing, using defaults (no live updates until next stream)");
            g_watcherStarted.store(false);  // retry on the next stream start
            return;
        }
        g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Arm();
        // Lives for the rest of the process; the DLL is then never unloaded (DllCanUnloadNow).
        RegisterWaitForSingleObject(&g_wait, g_event, OnChanged, nullptr, INFINITE, WT_EXECUTEDEFAULT);
        apolog::Write(L"[settings] live watcher started");
    }
}
