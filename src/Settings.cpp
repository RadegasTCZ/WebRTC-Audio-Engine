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
            apolog::Write(L"[settings] bypass=%u hpf=%u@%uHz ns=%u level=%u gainMode=%u", s.bypass, s.hpfEnabled, s.hpfHz, s.nsEnabled, s.nsLevel, s.gainMode);
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
