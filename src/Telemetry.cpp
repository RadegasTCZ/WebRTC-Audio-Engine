// SPDX-License-Identifier: GPL-3.0-or-later
#include "Telemetry.h"
#include <sddl.h>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace telemetry
{
    namespace
    {
        Section* OpenSection() noexcept
        {
            static std::atomic<Section*> s_section{nullptr};
            if (Section* s = s_section.load())
                return s;
            // SYSTEM full; LOCAL SERVICE + app containers (audiodg) read/write (no WRITE_DAC); Users, Admins read.
            SECURITY_ATTRIBUTES sa{sizeof(sa)};
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:(A;;GA;;;SY)(A;;GRGW;;;LS)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)(A;;GR;;;BU)(A;;GR;;;BA)", SDDL_REVISION_1,
                    &sd, nullptr))
                return nullptr;
            sa.lpSecurityDescriptor = sd;
            HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Section), kSectionName);
            LocalFree(sd);
            if (!h)
                return nullptr;
            auto* s = static_cast<Section*>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Section)));
            if (!s)
            {
                CloseHandle(h);
                return nullptr;
            }
            s->version = kVersion;  // handle kept for the process lifetime
            Section* expected = nullptr;
            if (!s_section.compare_exchange_strong(expected, s))
            {
                UnmapViewOfFile(s);
                CloseHandle(h);
                return expected;
            }
            return s;
        }

    }

    LONG64 KeyOf(const wchar_t* id) noexcept
    {
        uint64_t h = 1469598103934665603ull;  // FNV-1a
        for (; id && *id; ++id) { h ^= static_cast<uint16_t>(*id); h *= 1099511628211ull; }
        return static_cast<LONG64>(h ? h : 1);
    }

    bool Owns(const Slot* slot, LONG64 key) noexcept { return slot && slot->key == key; }

    Slot* ClaimSlot(const wchar_t* endpointId, const wchar_t* friendlyName, const wchar_t* deviceGuid) noexcept
    {
        Section* s = OpenSection();
        if (!s)
            return nullptr;
        const LONG64 key = KeyOf(endpointId);
        auto setName = [&](Slot& slot) {
            wcsncpy_s(slot.name, friendlyName ? friendlyName : L"Microphone", _TRUNCATE);
            wcsncpy_s(slot.device, deviceGuid ? deviceGuid : L"", _TRUNCATE);
        };
        for (Slot& slot : s->slots)  // same device already has a slot (another stream of it)
            if (slot.key == key) { setName(slot); return &slot; }
        const uint64_t now = GetTickCount64();
        for (Slot& slot : s->slots)  // free or stale slot
        {
            const LONG64 old = slot.key;
            if ((old == 0 || now - slot.tickMs > kSlotStaleMs) && InterlockedCompareExchange64(&slot.key, key, old) == old)
            {
                setName(slot);
                slot.tickMs = now;  // fresh: not stale for another device until it stops publishing
                slot.seq &= ~1u;    // never inherit a half-finished update
                return &slot;
            }
        }
        return nullptr;  // more than kMaxSlots active devices: no meters for this one
    }

    // Several streams of one microphone share its slot: a writer takes the slot by moving seq from even
    // to odd with a compare-exchange, and skips this update (no waiting: real-time) if another writer
    // holds it or the slot now belongs to another device.
    void Publish(Slot* slot, LONG64 key, const Values& v) noexcept
    {
        if (!slot || slot->key != key)
            return;
        const uint32_t s = slot->seq;
        if ((s & 1u) || static_cast<uint32_t>(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&slot->seq),
                                                                          static_cast<LONG>(s + 1), static_cast<LONG>(s))) != s)
            return;
        slot->values = v;
        slot->tickMs = GetTickCount64();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot->seq), static_cast<LONG>(s + 2));  // full barrier
    }

    int ReadAll(Device (&out)[kMaxSlots], uint64_t maxAgeMs) noexcept
    {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, kSectionName);
        if (!h)
            return 0;
        const auto* s = static_cast<const Section*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Section)));
        int n = 0;
        if (s && s->version == kVersion)
        {
            const uint64_t now = GetTickCount64();
            for (const Slot& slot : s->slots)
            {
                if (slot.key == 0)
                    continue;
                for (int tries = 0; tries < 8; ++tries)
                {
                    const uint32_t s1 = slot.seq;
                    if (s1 & 1u)
                        continue;
                    std::atomic_thread_fence(std::memory_order_acquire);
                    Device d{};
                    std::memcpy(d.name, slot.name, sizeof(d.name));
                    std::memcpy(d.device, slot.device, sizeof(d.device));
                    std::memcpy(&d.values, const_cast<const Values*>(&slot.values), sizeof(Values));
                    d.name[std::size(d.name) - 1] = d.device[std::size(d.device) - 1] = 0;  // never trust the other side to terminate
                    const uint64_t tick = slot.tickMs;
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (slot.seq != s1)
                        continue;
                    if (tick && now - tick <= maxAgeMs)
                        out[n++] = d;
                    break;
                }
            }
        }
        if (s)
            UnmapViewOfFile(s);
        CloseHandle(h);
        return n;
    }
}
