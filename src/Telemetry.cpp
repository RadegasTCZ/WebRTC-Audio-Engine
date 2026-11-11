// SPDX-License-Identifier: GPL-3.0-or-later
#include "Telemetry.h"
#include <sddl.h>
#include <atomic>
#include <cstring>

namespace telemetry
{
    Data* OpenWriter() noexcept
    {
        static std::atomic<Data*> s_data{nullptr};
        if (Data* d = s_data.load())
            return d;
        // SYSTEM + LOCAL SERVICE + app containers (audiodg) full access; Users, Admins read only.
        SECURITY_ATTRIBUTES sa{sizeof(sa)};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;AC)(A;;GA;;;S-1-15-2-2)(A;;GR;;;BU)(A;;GR;;;BA)", SDDL_REVISION_1, &sd,
                nullptr))
            return nullptr;
        sa.lpSecurityDescriptor = sd;
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Data), kSectionName);
        LocalFree(sd);
        if (!h)
            return nullptr;
        auto* d = static_cast<Data*>(MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, sizeof(Data)));
        // The handle is kept for the process lifetime (the section must outlive every effect instance).
        if (!d)
        {
            CloseHandle(h);
            return nullptr;
        }
        d->version = kVersion;
        Data* expected = nullptr;
        if (!s_data.compare_exchange_strong(expected, d))
        {
            UnmapViewOfFile(d);
            CloseHandle(h);
            return expected;
        }
        return d;
    }

    void Publish(Data* d, const Data& v) noexcept
    {
        if (!d)
            return;
        const uint32_t s = d->seq;
        d->seq = s + 1;
        std::atomic_thread_fence(std::memory_order_release);
        d->tickMs = GetTickCount64();
        d->inputDbfs = v.inputDbfs;
        d->outputDbfs = v.outputDbfs;
        d->referenceDbfs = v.referenceDbfs;
        d->echoRemovedDb = v.echoRemovedDb;
        d->agcGainDb = v.agcGainDb;
        d->limiterDb = v.limiterDb;
        d->aecRunning = v.aecRunning;
        d->sampleRate = v.sampleRate;
        std::atomic_thread_fence(std::memory_order_release);
        d->seq = s + 2;
    }

    bool Read(Data& out, uint64_t maxAgeMs) noexcept
    {
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, kSectionName);
        if (!h)
            return false;
        const auto* d = static_cast<const Data*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(Data)));
        bool ok = false;
        if (d && d->version == kVersion)
        {
            for (int tries = 0; tries < 8 && !ok; ++tries)
            {
                const uint32_t s1 = d->seq;
                if (s1 & 1u)
                    continue;
                std::atomic_thread_fence(std::memory_order_acquire);
                std::memcpy(&out, const_cast<const Data*>(d), sizeof(Data));
                std::atomic_thread_fence(std::memory_order_acquire);
                ok = d->seq == s1;
            }
            ok = ok && GetTickCount64() - out.tickMs <= maxAgeMs;
        }
        if (d)
            UnmapViewOfFile(d);
        CloseHandle(h);
        return ok;
    }
}
