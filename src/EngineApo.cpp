// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#include "EngineApo.h"
#include "Guids.h"
#include "Log.h"
#include "Telemetry.h"
#include <xmmintrin.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <audiomediatype.h>
#include <string>
#include <cwctype>
#include <algorithm>
#include <vector>
#include <ksmedia.h>
#include <cstring>
#include <cmath>
#include <new>

namespace { void ForgetFxEvents(const void* owner) noexcept; }

std::atomic<long> CEngineApo::s_liveObjects{0};
std::atomic<long> CEngineApo::s_nextInstance{0};

namespace
{
    constexpr GUID kModeRaw = {0x9E90EA20, 0xB493, 0x4FD1, {0xA1, 0xA8, 0x7E, 0x13, 0x61, 0xA9, 0x56, 0xCF}};
#pragma warning(disable : 4815) // CRegAPOProperties<1> has a zero-length IID array by design
    CRegAPOProperties<1> g_regPack(CLSID_WebrtcAudioEnginePack, L"WebRTC Audio Engine", L"Copyright (c) RadegasTCZ",
                                   1, 0, __uuidof(IAudioProcessingObject), APO_FLAG_DEFAULT);

    // True the first time an interface ID is seen in this process. Every effect instance gets asked about
    // the same optional interfaces, so logging each one once keeps the log readable. No allocation.
    bool FirstUnsupportedQuery(REFIID riid) noexcept
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static GUID seen[128];
        static size_t count = 0;
        AcquireSRWLockExclusive(&lock);
        bool first = count < std::size(seen) && std::find(seen, seen + count, riid) == seen + count;
        if (first)
            seen[count++] = riid;
        ReleaseSRWLockExclusive(&lock);
        return first;
    }

    const wchar_t* ModeName(REFGUID m) noexcept
    {
        struct { GUID g; const wchar_t* name; } static const table[] = {
            {{0xC18E2F7E, 0x933D, 0x4965, {0xB7, 0xD1, 0x1E, 0xEF, 0x22, 0x8D, 0x2A, 0xF3}}, L"DEFAULT"},
            {{0x9E90EA20, 0xB493, 0x4FD1, {0xA1, 0xA8, 0x7E, 0x13, 0x61, 0xA9, 0x56, 0xCF}}, L"RAW"},
            {{0x98951333, 0xB9CD, 0x48B1, {0xA0, 0xA3, 0xFF, 0x40, 0x68, 0x2D, 0x73, 0xF7}}, L"COMMUNICATIONS"},
            {{0xFC1CFC9B, 0xB9D6, 0x4CFA, {0xB5, 0xE0, 0x4B, 0xB2, 0x16, 0x68, 0x78, 0xB2}}, L"SPEECH"},
            {{0x4780004E, 0x7133, 0x41D8, {0x8C, 0x74, 0x66, 0x0D, 0xAD, 0xD2, 0xC0, 0xEE}}, L"MEDIA"},
            {{0xB26FEB0D, 0xEC94, 0x477C, {0x94, 0x94, 0xD1, 0xAB, 0x8E, 0x75, 0x3F, 0x6E}}, L"MOVIE"},
            {{0x9CF2A70B, 0xF377, 0x403B, {0xBD, 0x6B, 0x36, 0x08, 0x63, 0xE0, 0x35, 0x5C}}, L"NOTIFICATION"},
        };
        for (const auto& e : table)
            if (IsEqualGUID(e.g, m))
                return e.name;
        return IsEqualGUID(m, GUID_NULL) ? L"(none)" : L"(unknown)";
    }
}

// ---------------------------------------------------------------- Remembered mic delay (per microphone)
// Kept in memory for this audio engine run and in the registry of the account audiodg runs as (LOCAL SERVICE:
// HKCU = its own hive, not watched by the settings watcher), so the next stream on that microphone starts
// lined up and reports its true latency right away.

namespace
{
    constexpr wchar_t kTimingKey[] = L"Software\\WebrtcAudioEngine\\EchoTiming3";   // 3: values from before the fixed reference target (2026-11-28) are stale
    struct RememberedDelay { wchar_t device[64]; int samples; double ratePpm; int shift; };
    SRWLOCK g_rememberedLock = SRWLOCK_INIT;
    RememberedDelay g_remembered[32] = {};

    // Developer switches (A/B tests, diagnostic recordings) live in their own key, NOT in the settings key
    // that any user may write: only administrators can create or change this one (HKLM\SOFTWARE default ACL).
    constexpr wchar_t kDiagKey[] = L"SOFTWARE\\WebrtcAudioEngine\\Diagnostics";

    // Read once at load: a function-local static would take its init guard on the real-time thread.
    const int64_t g_qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();

    // Flush-to-zero + denormals-are-zero while our processing runs. Recursive filters and smoothers fed
    // with digital silence (muted mic) decay into denormals, which cost 10-100x per operation; upstream
    // WebRTC's APM wraps its processing the same way (DenormalDisabler). The thread is shared with other
    // APOs, so the previous mode is restored on the way out.
    class DenormalGuard
    {
    public:
        DenormalGuard() noexcept : m_old(_mm_getcsr()) { _mm_setcsr(m_old | 0x8040u); }  // FTZ | DAZ
        ~DenormalGuard() { _mm_setcsr(m_old); }
        DenormalGuard(const DenormalGuard&) = delete;
        DenormalGuard& operator=(const DenormalGuard&) = delete;
    private:
        unsigned int m_old;
    };

    DWORD ReadDiagValue(const wchar_t* name, DWORD fallback) noexcept
    {
        DWORD v = 0, size = sizeof(v);
        return RegGetValueW(HKEY_LOCAL_MACHINE, kDiagKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : fallback;
    }

    // Remembered per microphone: the mic delay (48 kHz samples, value "{guid}") and the reference rate
    // correction (0.1 ppm units as a signed DWORD, value "{guid}.rate") and how far the reference was moved
    // earlier (48 kHz samples, value "{guid}.shift").
    int LoadRememberedDelay(const wchar_t* device, double& ratePpm, int& shift) noexcept
    {
        ratePpm = 0.0;
        shift = 0;
        if (!device || !*device) return 0;
        int found = -1;
        AcquireSRWLockShared(&g_rememberedLock);
        for (const auto& r : g_remembered)
            if (r.device[0] && _wcsicmp(r.device, device) == 0) { found = r.samples; ratePpm = r.ratePpm; shift = r.shift; break; }
        ReleaseSRWLockShared(&g_rememberedLock);
        if (found >= 0) return found;
        wchar_t shiftName[80];
        _snwprintf_s(shiftName, _TRUNCATE, L"%s.shift", device);
        DWORD sv = 0, ssize = sizeof(sv);
        if (RegGetValueW(HKEY_CURRENT_USER, kTimingKey, shiftName, RRF_RT_REG_DWORD, nullptr, &sv, &ssize) == ERROR_SUCCESS)
            shift = static_cast<int>(std::min<DWORD>(sv, ReferenceSync::kMaxTarget));
        wchar_t rateName[80];
        _snwprintf_s(rateName, _TRUNCATE, L"%s.rate", device);
        DWORD v = 0, size = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, kTimingKey, rateName, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
            ratePpm = std::clamp(static_cast<int32_t>(v) / 10.0, -1000.0, 1000.0);
        size = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, kTimingKey, device, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
            return static_cast<int>(std::min<DWORD>(v, EchoTimingController::kMaxDelaySamples));
        return 0;
    }

    void RememberDelay(const wchar_t* device, int samples, double ratePpm, int shift) noexcept
    {
        if (!device || !*device) return;
        AcquireSRWLockExclusive(&g_rememberedLock);
        RememberedDelay* slot = nullptr;
        for (auto& r : g_remembered)
            if (r.device[0] && _wcsicmp(r.device, device) == 0) { slot = &r; break; }
        if (!slot)
            for (auto& r : g_remembered)
                if (!r.device[0]) { slot = &r; break; }
        if (slot)
        {
            wcsncpy_s(slot->device, device, _TRUNCATE);
            slot->samples = samples;
            slot->ratePpm = ratePpm;
            slot->shift = shift;
        }
        ReleaseSRWLockExclusive(&g_rememberedLock);
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kTimingKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
        {
            const DWORD v = static_cast<DWORD>(samples);
            RegSetValueExW(key, device, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            wchar_t rateName[80];
            _snwprintf_s(rateName, _TRUNCATE, L"%s.rate", device);
            const DWORD r = static_cast<DWORD>(static_cast<int32_t>(std::lround(ratePpm * 10.0)));
            RegSetValueExW(key, rateName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&r), sizeof(r));
            wchar_t shiftName[80];
            _snwprintf_s(shiftName, _TRUNCATE, L"%s.shift", device);
            const DWORD sv = static_cast<DWORD>(std::max(0, shift));
            RegSetValueExW(key, shiftName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sv), sizeof(sv));
            RegCloseKey(key);
        }
    }
}

CEngineApo::CEngineApo(IUnknown* outer) noexcept
    : CBaseAudioProcessingObject(&g_regPack.m_Properties),
      m_instanceId(++s_nextInstance)
{
    m_inner.self = this;
    m_outer = outer ? outer : &m_inner;
    ++s_liveObjects;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    apolog::Write(L"[%s #%ld] created in host %s (aggregated=%d)", SlotName(), m_instanceId, exe, outer ? 1 : 0);
}

CEngineApo::~CEngineApo()
{
    StopTimingTimer();
    if (m_timingTimer)
        CloseThreadpoolTimer(m_timingTimer);
    ForgetFxEvents(this);
    if (m_device)
        m_device->Release();
    apolog::Write(L"[%s #%ld] destroyed", SlotName(), m_instanceId);
    --s_liveObjects;
}

// ---------------------------------------------------------------- IUnknown

STDMETHODIMP CEngineApo::QueryInterface(REFIID riid, void** ppv) { return m_outer->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CEngineApo::AddRef() { return m_outer->AddRef(); }
STDMETHODIMP_(ULONG) CEngineApo::Release() { return m_outer->Release(); }

HRESULT CEngineApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) noexcept
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown))
    {
        *ppv = &m_inner;  // identity of the inner object; refcount on the inner object
        m_inner.AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IAudioProcessingObject))
        *ppv = static_cast<IAudioProcessingObject*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectRT))
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    else if (riid == __uuidof(IAudioSystemEffects))
        *ppv = static_cast<IAudioSystemEffects*>(this);
    else if (riid == __uuidof(IAudioSystemEffects2))
        *ppv = static_cast<IAudioSystemEffects2*>(this);
    else if ((riid == __uuidof(IApoAcousticEchoCancellation) || riid == __uuidof(IApoAcousticEchoCancellation2)) && IsAecCandidate())
        *ppv = static_cast<IApoAcousticEchoCancellation2*>(this);
    else if (riid == __uuidof(IApoAuxiliaryInputConfiguration) && IsAecCandidate())
        *ppv = static_cast<IApoAuxiliaryInputConfiguration*>(this);
    else if (riid == __uuidof(IApoAuxiliaryInputRT) && IsAecCandidate())
        *ppv = static_cast<IApoAuxiliaryInputRT*>(this);
    else if (riid == __uuidof(IAudioSystemEffects3))
        *ppv = static_cast<IAudioSystemEffects3*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectNotifications))
        *ppv = static_cast<IAudioProcessingObjectNotifications*>(this);
    else
    {
        // Useful for discovery: shows which optional interfaces (CAPX, AEC, ...) the engine asks for.
        if (FirstUnsupportedQuery(riid))
        {
            wchar_t g[40];
            apolog::GuidToString(riid, g);
            apolog::Write(L"[%s #%ld] QueryInterface for unsupported %s (logged once per audio engine run)", SlotName(), m_instanceId, g);
        }
        return E_NOINTERFACE;
    }
    AddRef();  // goes to the outer object when aggregated (COM aggregation rule)
    return S_OK;
}

// ---------------------------------------------------------------- Initialize

STDMETHODIMP CEngineApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    if (cbDataSize > 0 && !pbyData)
        return E_POINTER;

    // Three possible init structures; pull the common fields out of whichever we got.
    const wchar_t* initKind = L"other";
    IPropertyStore* endpointProps = nullptr;
    IMMDeviceCollection* collection = nullptr;
    UINT ioIndex = 0;
    BOOL discoveryOnly = -1;
    if (cbDataSize == sizeof(APOInitSystemEffects3))
    {
        const auto* v3 = reinterpret_cast<const APOInitSystemEffects3*>(pbyData);
        initKind = L"APOInitSystemEffects3";
        endpointProps = v3->pAPOEndpointProperties;
        collection = v3->pDeviceCollection;
        ioIndex = v3->nSoftwareIoDeviceInCollection;
        m_mode = v3->AudioProcessingMode;
        discoveryOnly = v3->InitializeForDiscoveryOnly;
    }
    else if (cbDataSize == sizeof(APOInitSystemEffects2))
    {
        const auto* v2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        initKind = L"APOInitSystemEffects2";
        endpointProps = v2->pAPOEndpointProperties;
        collection = v2->pDeviceCollection;
        ioIndex = v2->nSoftwareIoDeviceInCollection;
        m_mode = v2->AudioProcessingMode;
        discoveryOnly = v2->InitializeForDiscoveryOnly;
    }
    else if (cbDataSize == sizeof(APOInitSystemEffects))
    {
        initKind = L"APOInitSystemEffects";
        endpointProps = reinterpret_cast<const APOInitSystemEffects*>(pbyData)->pAPOEndpointProperties;
    }

    // Endpoint name + id, for confirming which mic we sit on.
    wchar_t endpointName[256] = L"?";
    wchar_t endpointId[256] = L"?";
    if (endpointProps)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(endpointProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            wcsncpy_s(endpointName, pv.pwszVal, _TRUNCATE);
        PropVariantClear(&pv);
    }
    wchar_t mmEndpointId[128] = L"?";
    if (collection)
    {
        // Software IO device (the KS filter): its id tells us the bus (hdaudio#..., usb#..., bth...).
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(collection->Item(ioIndex, &dev)) && dev)
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id)
            {
                wcsncpy_s(endpointId, id, _TRUNCATE);
                wchar_t lower[512];  // no allocation here: an exception must not escape into audiodg
                wcsncpy_s(lower, id, _TRUNCATE);
                for (wchar_t* ch = lower; *ch; ++ch) *ch = static_cast<wchar_t>(towlower(*ch));
                m_hardwareVolumeBus = wcsstr(lower, L"usb#") || wcsstr(lower, L"usb\\") || wcsstr(lower, L"bth") ||
                                      wcsstr(lower, L"bluetooth");
                CoTaskMemFree(id);
            }
            dev->Release();
        }
        // The audio endpoint itself is the last device in the collection (Windows 11 APO docs).
        UINT count = 0;
        if (SUCCEEDED(collection->GetCount(&count)) && count > 0)
        {
            if (m_device) { m_device->Release(); m_device = nullptr; }
            if (SUCCEEDED(collection->Item(count - 1, &m_device)) && m_device)
            {
                LPWSTR id = nullptr;
                if (SUCCEEDED(m_device->GetId(&id)) && id)
                {
                    wcsncpy_s(mmEndpointId, id, _TRUNCATE);
                    CoTaskMemFree(id);
                }
            }
        }
    }

    wchar_t modeGuid[40];
    apolog::GuidToString(m_mode, modeGuid);
    wcsncpy_s(m_endpointName, endpointName, _TRUNCATE);
    wcsncpy_s(m_endpointKey, mmEndpointId, _TRUNCATE);
    if (const wchar_t* brace = wcsrchr(mmEndpointId, L'{'))  // "{0.0.1.00000000}.{guid}" -> "{guid}"
        wcsncpy_s(m_deviceGuid, brace, _TRUNCATE);
    apolog::Write(L"[%s #%ld] Initialize cb=%u (%s) mode=%s %s discoveryOnly=%d endpoint=\"%s\" mmdevice=%s io=%s hwVolumeBus=%d",
                  SlotName(), m_instanceId, cbDataSize, initKind,
                  ModeName(m_mode), modeGuid, discoveryOnly, endpointName, mmEndpointId, endpointId,
                  m_hardwareVolumeBus ? 1 : 0);

    // The echo reference ring (1 s of mono 48 kHz) is allocated here, once: the reference producer
    // (AcceptInput) can run while capture is stopped, so the ring must never be (re)allocated later.
    if (m_ref.Capacity() == 0)
    {
        try { m_ref.Init(VoiceProcessor::kSampleRate); }
        catch (...) { return E_OUTOFMEMORY; }
    }

    m_discoveryOnly = discoveryOnly == TRUE;
    m_bIsInitialized = true;
    return S_OK;
}

STDMETHODIMP CEngineApo::GetLatency(HNSTIME* pTime)
{
    if (!pTime)
        return E_POINTER;
    // HNSTIME is in 100 ns units.
    // Includes the automatic mic delay (remembered from the last stream on this microphone, or changed since).
    *pTime = m_proc.IsActive() ? static_cast<HNSTIME>(m_proc.LatencyFrames()) * 10000000 / m_proc.SampleRate() : 0;
    // Logged (first calls of a stream, and whenever the value changed) to learn when Windows asks.
    if (m_proc.IsActive() && (m_latencyLogged < 3 || *pTime != m_latencyLastLogged) && m_latencyLogged < 50)
    {
        ++m_latencyLogged;
        m_latencyLastLogged = *pTime;
        apolog::Write(L"[%s #%ld] GetLatency -> %.2f ms (%d frames, mic delay %.1f ms)", SlotName(), m_instanceId, *pTime / 10000.0,
                      m_proc.LatencyFrames(), m_proc.MicDelaySamples() / 48.0);
    }
    return S_OK;
}

// ---------------------------------------------------------------- Format negotiation (logged)

namespace
{
    void DescribeFormat(IAudioMediaType* t, wchar_t (&out)[96]) noexcept
    {
        UNCOMPRESSEDAUDIOFORMAT f{};
        if (!t)
            wcscpy_s(out, L"(null)");
        else if (FAILED(t->GetUncompressedAudioFormat(&f)))
            wcscpy_s(out, L"(compressed/unknown)");
        else
            _snwprintf_s(out, _TRUNCATE, L"%s %uch %ub/%ub %.0fHz",
                       IsEqualGUID(f.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? L"float" : L"pcm",
                       f.dwSamplesPerFrame, f.dwValidBitsPerSample, f.dwBytesPerSampleContainer * 8, f.fFramesPerSecond);
    }
}

STDMETHODIMP CEngineApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                               IAudioMediaType** ppSupportedInputFormat)
{
    HRESULT hr = CBaseAudioProcessingObject::IsInputFormatSupported(pOppositeFormat, pRequestedInputFormat, ppSupportedInputFormat);
    wchar_t opp[96], req[96], sup[96];
    DescribeFormat(pOppositeFormat, opp);
    DescribeFormat(pRequestedInputFormat, req);
    DescribeFormat(ppSupportedInputFormat ? *ppSupportedInputFormat : nullptr, sup);
    apolog::Write(L"[%s #%ld] IsInputFormatSupported mode=%s req=[%s] opposite=[%s] -> hr=0x%08X supported=[%s]",
                  SlotName(), m_instanceId, ModeName(m_mode), req, opp, hr, sup);
    return hr;
}

STDMETHODIMP CEngineApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                                IAudioMediaType** ppSupportedOutputFormat)
{
    HRESULT hr = CBaseAudioProcessingObject::IsOutputFormatSupported(pOppositeFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
    wchar_t opp[96], req[96], sup[96];
    DescribeFormat(pOppositeFormat, opp);
    DescribeFormat(pRequestedOutputFormat, req);
    DescribeFormat(ppSupportedOutputFormat ? *ppSupportedOutputFormat : nullptr, sup);
    apolog::Write(L"[%s #%ld] IsOutputFormatSupported mode=%s req=[%s] opposite=[%s] -> hr=0x%08X supported=[%s]",
                  SlotName(), m_instanceId, ModeName(m_mode), req, opp, hr, sup);
    return hr;
}

// ---------------------------------------------------------------- Lock / Unlock

void CEngineApo::ResetStats() noexcept
{
    m_processCalls = m_validFrames = m_silentCalls = 0;
    m_minPeriod = 0xFFFFFFFF;
    m_maxPeriod = 0;
    m_peak = 0.0f;
}

STDMETHODIMP CEngineApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                       UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(u32NumInputConnections, ppInputConnections,
                                                            u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr))
    {
        apolog::Write(L"[%s #%ld] LockForProcess FAILED in base class hr=0x%08X", SlotName(), m_instanceId, hr);
        return hr;
    }

    UNCOMPRESSEDAUDIOFORMAT fmt{};
    if (u32NumOutputConnections > 0 && ppOutputConnections[0]->pFormat)
        ppOutputConnections[0]->pFormat->GetUncompressedAudioFormat(&fmt);

    m_channels = fmt.dwSamplesPerFrame;
    m_bytesPerFrame = fmt.dwSamplesPerFrame * fmt.dwBytesPerSampleContainer;
    m_isFloat32 = IsEqualGUID(fmt.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && fmt.dwBytesPerSampleContainer == 4;
    ResetStats();
    m_echoTimeline.Reset();
    m_delayCount = m_delayMeters = 0;
    m_queueCount = m_queueMeters = 0;
    m_queueAvgAcc = 0;
    m_ppmAcc = 0;
    m_lastTarget = 0;
    m_lastTrimmed = m_lastUnderruns = 0;
    std::fill(std::begin(m_offsetSteps), std::end(m_offsetSteps), INT16_MIN);
    m_dumpArmed = false;
    m_lockTick = GetTickCount64();
    m_latencyLogged = 0;
    m_latencyLastLogged = -1;

    StopTimingTimer();
    m_proc.Deactivate();
    if (m_device)
    {
        // Starting slider position (notifications only arrive on changes).
        IAudioEndpointVolume* ev = nullptr;
        HRESULT hrv = m_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ev));
        float db = 0.0f;
        BOOL mute = FALSE;
        DWORD hw = 0;
        if (SUCCEEDED(hrv) && ev)
        {
            hrv = ev->GetMasterVolumeLevel(&db);
            if (FAILED(ev->GetMute(&mute))) mute = FALSE;
            m_hardwareVolume = SUCCEEDED(ev->QueryHardwareSupport(&hw)) ? ((hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? 1 : 0) : -1;
            ev->Release();
        }
        if (SUCCEEDED(hrv) && std::isfinite(db))
            m_sliderDb.store(db, std::memory_order_relaxed);
        m_muted.store(mute != FALSE, std::memory_order_relaxed);
        apolog::Write(L"[%s #%ld] initial slider: hr=0x%08X %.2f dB muted=%d, hardware volume=%d (bus guess %d)", SlotName(),
                      m_instanceId, hrv, db, mute ? 1 : 0, m_hardwareVolume, m_hardwareVolumeBus ? 1 : 0);
    }
    {
        // Voice processing (only float32 formats; anything else passes through untouched).
        settings::EnsureWatcher();  // live settings (non-RT here)
        m_devIdx = settings::RegisterDevice(m_deviceGuid);  // this microphone's settings (defaults or its own)
        m_proc.SetSettingsIndex(m_devIdx);
        const Settings s = settings::Current(m_devIdx);
        bool ok = false;
        if (!m_isFloat32)
            apolog::Write(L"[%s #%ld] processing off: format is not float32", SlotName(), m_instanceId);
        else if (IsEqualGUID(m_mode, kModeRaw))
            apolog::Write(L"[%s #%ld] processing off: RAW stream (unprocessed audio by contract)", SlotName(), m_instanceId);
        else
        {
            const UINT32 maxFrames = std::max(u32NumInputConnections ? ppInputConnections[0]->u32MaxFrameCount : 0u,
                                              u32NumOutputConnections ? ppOutputConnections[0]->u32MaxFrameCount : 0u);
            // Echo reference attached by AddAuxiliaryInput (its ring was allocated in Initialize).
            m_proc.SetReference(m_refActive.load() ? &m_ref : nullptr);
            // Automatic echo timing: on unless HKLM\SOFTWARE\WebrtcAudioEngine\Diagnostics\AutoMicDelay = 0 (A/B tests).
            const DWORD autoTiming = ReadDiagValue(L"AutoMicDelay", 1);
            double rememberedRate = 0.0;
            int rememberedShift = 0;
            const int remembered = autoTiming ? LoadRememberedDelay(m_deviceGuid, rememberedRate, rememberedShift) : 0;
            m_proc.SetEchoTimingSetup(autoTiming != 0, remembered, rememberedRate, rememberedShift);
            m_timingMeasured = m_timingValid = m_timingOutOfRange = m_timingChanges = 0;
            m_timingAgreedLogged = false;
            m_timingCapped = false;
            m_timingLast = {};
            // Per-device meters for the tray app (non-RT). Claim again if another device took our slot meanwhile.
            m_telemetryKey = telemetry::KeyOf(m_endpointKey);
            if (!telemetry::Owns(m_telemetry, m_telemetryKey))
                m_telemetry = telemetry::ClaimSlot(m_endpointKey, m_endpointName, m_deviceGuid);
            try
            {
                ok = m_proc.Prepare(static_cast<int>(m_channels), static_cast<int>(std::lround(fmt.fFramesPerSecond)), s,
                                    static_cast<int>(maxFrames ? maxFrames : 4096));
            }
            catch (...)  // out of memory: pass the audio through untouched instead of ending audiodg
            {
                ok = false;
                m_proc.Deactivate();
                apolog::Write(L"[%s #%ld] processing off: out of memory while preparing", SlotName(), m_instanceId);
            }
            if (ok && m_proc.EchoTimingActive())
            {
                StartTimingTimer();
                apolog::Write(L"[%s #%ld] echo timing: automatic, starting with %s mic delay %.1f ms, reference moved %.1f ms "
                              L"earlier, reference rate %+.1f ppm", SlotName(), m_instanceId,
                              remembered || rememberedShift ? L"the remembered" : L"no", m_proc.MicDelaySamples() / 48.0,
                              m_proc.Sync().RequestedShiftSamples() / 48.0, m_proc.Sync().RatePpm());
            }
            else if (ok && m_proc.HasAec() && !autoTiming)
                apolog::Write(L"[%s #%ld] echo timing: automatic mic delay OFF (AutoMicDelay = 0)", SlotName(), m_instanceId);
            if (ok && m_proc.HasAec())
            {
                // Diagnostics, off unless an administrator asks for it: value DiagDumpSeconds = 1..120 in
                // HKLM\SOFTWARE\WebrtcAudioEngine\Diagnostics records the last N seconds of what AEC3 receives (mic + speakers!),
                // written next to the log when the stream stops; only the newest 3 recordings are kept.
                const DWORD secs = ReadDiagValue(L"DiagDumpSeconds", 0);
                if (secs > 0)
                {
                    try
                    {
                        m_proc.ArmDump(static_cast<int>(std::min<DWORD>(secs, 120)));
                        m_dumpArmed = true;
                        apolog::Write(L"[%s #%ld] diagnostics: recording the last %lu s of AEC3 input (written at stream stop)",
                                      SlotName(), m_instanceId, std::min<DWORD>(secs, 120));
                    }
                    catch (...) { apolog::Write(L"[%s #%ld] diagnostics: out of memory, no recording", SlotName(), m_instanceId); }
                }
            }
            apolog::Write(L"[%s #%ld] NS %s: bypass=%u rumble=%u/%u hiss=%u/%u ns=%u level=%u channels=%u rate=%.0f%s maxFrames=%u latency=%d frames",
                          SlotName(), m_instanceId, ok ? L"ACTIVE" : L"off (unsupported format)", s.bypass, s.hpfEnabled,
                          s.hpfLevel, s.lpfEnabled, s.lpfLevel, s.nsEnabled, s.nsLevel, m_channels, fmt.fFramesPerSecond,
                          ok && m_proc.SampleRate() != VoiceProcessor::kSampleRate ? L" (resampled via 48 kHz)" : L"",
                          maxFrames, ok ? m_proc.LatencyFrames() : 0);
        }
    }

    apolog::Write(L"[%s #%ld] LockForProcess mode=%s fmt=%s %u ch %u-bit(container %u) %.0f Hz mask=0x%X maxFrames in=%u out=%u inBufType=%d",
                  SlotName(), m_instanceId, ModeName(m_mode), m_isFloat32 ? L"float32" : L"non-float",
                  fmt.dwSamplesPerFrame, fmt.dwValidBitsPerSample, fmt.dwBytesPerSampleContainer * 8,
                  fmt.fFramesPerSecond, fmt.dwChannelMask,
                  u32NumInputConnections ? ppInputConnections[0]->u32MaxFrameCount : 0,
                  u32NumOutputConnections ? ppOutputConnections[0]->u32MaxFrameCount : 0,
                  u32NumInputConnections ? static_cast<int>(ppInputConnections[0]->Type) : -1);
    return S_OK;
}

STDMETHODIMP CEngineApo::UnlockForProcess()
{
    const double seconds = (GetTickCount64() - m_lockTick) / 1000.0;
    const bool timing = m_timingTimer != nullptr && m_proc.EchoTimingActive();
    StopTimingTimer();
    if (timing)
    {
        const int delay = m_proc.MicDelaySamples();
        // Not a delay pinned at the 25 ms cap: the echo was out of reach, so the value is wrong (2026-11-28:
        // a broken stream's 25 ms carried into the next streams). Also a 0: never keep a stale delay.
        const bool keep = m_timingAgreedLogged && !m_timingCapped;
        if (keep)
            RememberDelay(m_deviceGuid, delay, m_proc.Sync().RatePpm(), m_proc.Sync().RequestedShiftSamples());
        apolog::Write(L"[%s #%ld] echo timing: %d measurements (%d clear, %d out of range), %d change(s), mic delay %.1f ms%s, "
                      L"last: echo %+.2f ms vs reference as AEC3 sees it, quality %.0f", SlotName(), m_instanceId, m_timingMeasured,
                      m_timingValid, m_timingOutOfRange, m_timingChanges, delay / 48.0,
                      keep ? L" (remembered for the next stream)"
                           : m_timingAgreedLogged ? L" (CAPPED: not remembered)" : L" (no agreed timing this stream)",
                      m_timingLast.offsetMs, m_timingLast.quality);
    }
    if (m_refActive.load())
    {
        // "dropped": reference that arrived while capture was paused (the queue keeps at most 1 s).
        // "blocks without reference": capture blocks processed with silence as reference, i.e. the
        // speakers were idle or paused (nothing to cancel), not a fault.
        if (m_refCalls.load() == 0)
            apolog::Write(L"[%s #%ld] echo reference: none arrived (speakers idle), AEC3 %s", SlotName(), m_instanceId,
                          m_proc.HasAec() ? L"running" : L"not running");
        else
            apolog::Write(L"[%s #%ld] echo reference: %.1f s received in %llu calls (%llu silent), %.1f s dropped while capture was paused, "
                          L"%llu capture blocks without reference (speakers idle/paused), AEC3 %s", SlotName(), m_instanceId,
                          m_refFrames.load() / 48000.0, m_refCalls.load(), m_refSilentCalls.load(), m_refDropped.load() / 48000.0,
                          m_proc.ReferenceUnderruns(), m_proc.HasAec() ? L"running" : L"not running");
    }
    if (m_proc.HasAec() && m_echoTimeline.HasData())
        m_echoTimeline.Report([this](const char* line) { apolog::Write(L"[%s #%ld] %hs", SlotName(), m_instanceId, line); });
    if (m_delayCount > 0)
    {
        // AEC3's echo-delay estimate per 10 s, 30 values (5 minutes) per line.
        for (int start = 0; start < m_delayCount; start += 30)
        {
            wchar_t line[400] = L"";
            size_t used = 0;
            for (int i = start; i < std::min(m_delayCount, start + 30); ++i)
            {
                const int n = _snwprintf_s(line + used, std::size(line) - used, _TRUNCATE, L" %d", static_cast<int>(m_delaySteps[i]));
                if (n > 0) used += static_cast<size_t>(n);
            }
            apolog::Write(L"[%s #%ld] AEC3 delay estimate ms per 10 s from %02d:%02d:%s", SlotName(), m_instanceId, start / 6,
                          (start % 6) * 10, line);
        }
    }
    if (m_queueCount > 0)
    {
        // Diagnostics: does the reference queue drift (render vs capture pace), and where is the echo?
        auto timeline = [&](const wchar_t* title, auto value) {
            for (int start = 0; start < m_queueCount; start += 30)
            {
                wchar_t line[600] = L"";
                size_t used = 0;
                for (int i = start; i < std::min(m_queueCount, start + 30); ++i)
                {
                    wchar_t v[24];
                    value(i, v);
                    const int n = _snwprintf_s(line + used, std::size(line) - used, _TRUNCATE, L" %s", v);
                    if (n > 0) used += static_cast<size_t>(n);
                }
                apolog::Write(L"[%s #%ld] %s per 10 s from %02d:%02d:%s", SlotName(), m_instanceId, title, start / 6, (start % 6) * 10, line);
            }
        };
        apolog::Write(L"[%s #%ld] reference sync: backlog target %.1f ms, rate correction now %+.1f ppm, %llu resync(s), "
                      L"%llu block(s) without reference, %llu capture pause(s), largest reference write %.1f ms, "
                      L"reference moved %.1f ms earlier in %llu step(s)", SlotName(),
                      m_instanceId, m_lastTarget / 48.0, m_proc.Sync().RatioPpm(), m_proc.Sync().Resyncs(), m_proc.ReferenceUnderruns(),
                      m_proc.Sync().CaptureGaps(), m_proc.Sync().PeakChunkSamples() / 48.0, m_proc.Sync().ShiftSamples() / 48.0,
                      m_proc.Sync().Shifts());
        timeline(L"reference backlog ms", [&](int i, wchar_t (&v)[24]) { _snwprintf_s(v, _TRUNCATE, L"%.1f", m_queueAvgSteps[i] / 48.0); });
        timeline(L"reference sync ppm", [&](int i, wchar_t (&v)[24]) { _snwprintf_s(v, _TRUNCATE, L"%+.1f", m_queueMinSteps[i] / 10.0); });
        timeline(L"reference resyncs/blocks without reference", [&](int i, wchar_t (&v)[24]) {
            _snwprintf_s(v, _TRUNCATE, L"%d/%d", m_queueTrimSteps[i], m_queueUnderSteps[i]); });
        timeline(L"echo vs reference ms (raw, auto timing)", [&](int i, wchar_t (&v)[24]) {
            if (m_offsetSteps[i] == INT16_MIN) wcscpy_s(v, L"."); else _snwprintf_s(v, _TRUNCATE, L"%+.1f", m_offsetSteps[i] / 10.0); });
    }
    if (m_dumpArmed)
    {
        m_dumpArmed = false;
        try
        {
            const std::vector<float> pairs = m_proc.TakeDump();
            if (!pairs.empty())
            {
                // WAV, float32, 48 kHz, 2 channels: left = microphone (what AEC3 gets), right = echo reference.
                // Header and samples are written separately (no second copy of up to 46 MB inside audiodg).
                const uint32_t dataBytes = static_cast<uint32_t>(pairs.size() * sizeof(float));
                uint8_t wav[44];
                auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
                auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
                std::memcpy(&wav[0], "RIFF", 4); put32(4, 36 + dataBytes); std::memcpy(&wav[8], "WAVEfmt ", 8);
                put32(16, 16); put16(20, 3 /*IEEE float*/); put16(22, 2); put32(24, 48000); put32(28, 48000 * 8);
                put16(32, 8); put16(34, 32); std::memcpy(&wav[36], "data", 4); put32(40, dataBytes);
                SYSTEMTIME t;
                GetLocalTime(&t);
                wchar_t name[96], path[MAX_PATH];
                _snwprintf_s(name, _TRUNCATE, L"aecdump-%04u%02u%02u_%02u%02u%02u-%ld.wav", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                           t.wSecond, m_instanceId);
                if (apolog::WriteDiagFile(name, wav, sizeof(wav), pairs.data(), dataBytes, path))
                    apolog::Write(L"[%s #%ld] diagnostics: wrote %.1f s of AEC3 input (L = mic, R = reference) to %s", SlotName(),
                                  m_instanceId, pairs.size() / 2 / 48000.0, path);
                else
                    apolog::Write(L"[%s #%ld] diagnostics: could not write the recording", SlotName(), m_instanceId);
            }
        }
        catch (...) { apolog::Write(L"[%s #%ld] diagnostics: out of memory while writing the recording", SlotName(), m_instanceId); }
    }
    const double peakDb = m_peak > 0.0f ? 20.0 * std::log10(static_cast<double>(m_peak)) : -999.0;
    apolog::Write(L"[%s #%ld] UnlockForProcess mode=%s after %.1f s: calls=%llu validFrames=%llu silentCalls=%llu period=%u..%u frames peak=%.1f dBFS underruns=%llu",
                  SlotName(), m_instanceId, ModeName(m_mode), seconds, m_processCalls, m_validFrames, m_silentCalls,
                  m_maxPeriod ? m_minPeriod : 0, m_maxPeriod, peakDb, m_proc.Underruns());
    if (const uint64_t bad = m_proc.NonFiniteBlocks())
        apolog::Write(L"[%s #%ld] WARNING: %llu block(s) had NaN/Inf after processing and were silenced", SlotName(), m_instanceId, bad);
    return CBaseAudioProcessingObject::UnlockForProcess();
}

// ---------------------------------------------------------------- Automatic echo timing (timer, non-RT)

void CALLBACK CEngineApo::TimingTimerCallback(PTP_CALLBACK_INSTANCE, PVOID self, PTP_TIMER) noexcept
{
    static_cast<CEngineApo*>(self)->OnTimingTick();
}

void CEngineApo::StartTimingTimer() noexcept
{
    if (!m_timingTimer)
        m_timingTimer = CreateThreadpoolTimer(&CEngineApo::TimingTimerCallback, this, nullptr);
    if (!m_timingTimer)
    {
        apolog::Write(L"[%s #%ld] echo timing: no timer (error %lu), automatic timing off", SlotName(), m_instanceId, GetLastError());
        return;
    }
    // Every 1000 ms (the controller's settle time counts in these ticks), first after 1 s, 100 ms slack.
    ULARGE_INTEGER due;
    due.QuadPart = static_cast<ULONGLONG>(-10000000LL);   // relative 1 s
    FILETIME ft{due.LowPart, due.HighPart};
    SetThreadpoolTimer(m_timingTimer, &ft, 1000, 100);
}

void CEngineApo::StopTimingTimer() noexcept
{
    if (!m_timingTimer)
        return;
    SetThreadpoolTimer(m_timingTimer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(m_timingTimer, TRUE);
}

void CEngineApo::OnTimingTick() noexcept
{
    const VoiceProcessor::EchoTimingEvent ev = m_proc.EchoTimingTick();
    if (!ev.measured)
        return;
    ++m_timingMeasured;
    if (ev.result.valid)
    {
        ++m_timingValid;
        m_timingLast = ev.result;
        // Raw echo position for the per-10 s diagnostics timeline (the delay that was in effect when measured).
        const size_t step = static_cast<size_t>((GetTickCount64() - m_lockTick) / 10000);
        const float raw = ev.result.offsetMs - static_cast<float>(m_proc.MicDelaySamples()) / 48.0f;
        if (step < std::size(m_offsetSteps))
            m_offsetSteps[step] = static_cast<int16_t>(std::clamp(std::lround(raw * 10.0f), -32767L, 32767L));
    }
    if (ev.result.outOfRange)
    {
        if (++m_timingOutOfRange == 1)
            apolog::Write(L"[%s #%ld] echo timing: clear echo at %+.1f ms, outside the range we act on (quality %.0f)", SlotName(),
                          m_instanceId, ev.result.offsetMs, ev.result.quality);
    }
    if (ev.decision.agreedNow)
        m_timingCapped = ev.decision.capped;
    if (ev.decision.change)
    {
        ++m_timingChanges;
        apolog::Write(L"[%s #%ld] echo timing: echo %+.2f ms vs reference (quality %.0f) -> reference moved %.1f ms earlier "
                      L"(total %.1f), mic delayed %.1f ms%s", SlotName(), m_instanceId, ev.decision.rawOffsetMs, ev.result.quality,
                      ev.decision.refShiftSamples / 48.0, ev.refShiftSamples / 48.0, ev.decision.delaySamples / 48.0,
                      ev.decision.capped ? L" (CAPPED: beyond the 25 ms mic delay and the reference slack)" : L"");
        if (!ev.decision.capped)
            RememberDelay(m_deviceGuid, ev.decision.delaySamples, ev.ratePpm, ev.refShiftSamples);
        m_timingAgreedLogged = true;
    }
    if (ev.rateChanged)
    {
        apolog::Write(L"[%s #%ld] echo timing: echo slides %+.2f ms per minute (%+.1f ppm) -> reference rate %+.1f ppm", SlotName(),
                      m_instanceId, ev.slidePpm * 0.06, ev.slidePpm, ev.ratePpm);
        if (!m_timingCapped)
            RememberDelay(m_deviceGuid, ev.delaySamples, ev.ratePpm, ev.refShiftSamples);
    }
    if (!ev.decision.change && ev.agreed && !m_timingAgreedLogged)
    {
        m_timingAgreedLogged = true;
        apolog::Write(L"[%s #%ld] echo timing: echo %+.2f ms vs reference (quality %.0f) -> mic delay %.1f ms is right, no change",
                      SlotName(), m_instanceId, ev.decision.rawOffsetMs, ev.result.quality, ev.delaySamples / 48.0);
    }
}

// ---------------------------------------------------------------- Real-time processing
// Rules: no allocation, no locks, no logging, no waiting.

STDMETHODIMP_(void) CEngineApo::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                          UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0)
        return;
    const DenormalGuard denormals;

    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
    const UINT32 frames = in->u32ValidFrameCount;

    ++m_processCalls;
    if (frames < m_minPeriod) m_minPeriod = frames;
    if (frames > m_maxPeriod) m_maxPeriod = frames;

    if (m_proc.IsActive())
    {
        // NS keeps a continuous timeline: silent buffers are fed as zeros so the FIFO stays in step.
        float* o = reinterpret_cast<float*>(out->pBuffer);
        const Settings cur = settings::Current(m_devIdx);
        const float gainDb = SliderGainDb(cur);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_proc.SetClock(now.QuadPart, g_qpcFreq);
        if (in->u32BufferFlags == BUFFER_VALID)
            m_proc.Process(reinterpret_cast<const float*>(in->pBuffer), o, static_cast<int>(frames), cur, gainDb);
        else
        {
            std::memset(o, 0, static_cast<size_t>(frames) * m_bytesPerFrame);
            m_proc.Process(o, o, static_cast<int>(frames), cur, gainDb);
            ++m_silentCalls;
        }
        VoiceProcessor::Meters mt;
        if (m_proc.TakeMeters(mt))  // every 100 ms
        {
            m_echoTimeline.Add(mt.echoRemovedDb, mt.referenceDbfs, mt.aecRunning);
            if (mt.aecDelayMs >= 0 && ++m_delayMeters >= 100)  // one value per 10 s
            {
                m_delayMeters = 0;
                if (m_delayCount < kDelaySteps)
                    m_delaySteps[m_delayCount++] = static_cast<int16_t>(std::min(mt.aecDelayMs, 32767));
            }
            if (mt.aecDelayMs >= 0)
            {
                m_queueAvgAcc += mt.refBacklog;
                m_ppmAcc += mt.refPpm;
                m_queueMinAcc = mt.refTarget;
                if (++m_queueMeters >= 100)   // one value per 10 s
                {
                    if (m_queueCount < kDelaySteps)
                    {
                        const uint64_t under = m_proc.ReferenceUnderruns();
                        m_queueAvgSteps[m_queueCount] = static_cast<int16_t>(std::lround(m_queueAvgAcc / m_queueMeters));
                        m_queueMinSteps[m_queueCount] = static_cast<int16_t>(std::clamp(std::lround(m_ppmAcc / m_queueMeters * 10.0), -32767L, 32767L));
                        m_queueTrimSteps[m_queueCount] = static_cast<int32_t>(mt.refResyncs - m_lastTrimmed);
                        m_queueUnderSteps[m_queueCount] = static_cast<int32_t>(under - m_lastUnderruns);
                        m_lastTrimmed = mt.refResyncs;
                        m_lastTarget = mt.refTarget;
                        m_lastUnderruns = under;
                        ++m_queueCount;
                    }
                    m_queueMeters = 0;
                    m_queueAvgAcc = 0;
                    m_ppmAcc = 0;
                }
            }
            if (m_telemetry)
            {
                telemetry::Values d{};
                d.inputDbfs = mt.inputDbfs; d.outputDbfs = mt.outputDbfs; d.referenceDbfs = mt.referenceDbfs;
                d.echoRemovedDb = mt.echoRemovedDb; d.agcGainDb = mt.agcGainDb; d.limiterDb = mt.limiterDb;
                d.aecRunning = mt.aecRunning ? 1u : 0u;
                d.speech = mt.speech;
                d.sampleRate = static_cast<uint32_t>(m_proc.SampleRate());
                d.aecDelayMs = mt.aecDelayMs;
                d.micDelayMs = static_cast<float>(m_proc.MicDelaySamples()) / 48.0f;
                d.refShiftMs = static_cast<float>(m_proc.Sync().ShiftSamples()) / 48.0f;
                d.refBacklogMs = mt.refBacklog / 48.0f;
                d.refTargetMs = static_cast<float>(mt.refTarget) / 48.0f;
                d.timingCapped = m_timingCapped.load(std::memory_order_relaxed) ? 1u : 0u;
                telemetry::Publish(m_telemetry, m_telemetryKey, d);
            }
        }
        float peak = m_peak;
        const size_t n = static_cast<size_t>(frames) * m_channels;
        for (size_t i = 0; i < n; ++i)
        {
            const float a = std::fabs(o[i]);
            if (a > peak) peak = a;
        }
        m_peak = peak;
        m_validFrames += frames;
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_VALID;
        return;
    }

    if (in->u32BufferFlags == BUFFER_VALID)
    {
        if (in->pBuffer != out->pBuffer)
            std::memcpy(reinterpret_cast<void*>(out->pBuffer), reinterpret_cast<const void*>(in->pBuffer),
                        static_cast<size_t>(frames) * m_bytesPerFrame);

        if (m_isFloat32)
        {
            const float* s = reinterpret_cast<const float*>(out->pBuffer);
            float peak = m_peak;
            const size_t n = static_cast<size_t>(frames) * m_channels;
            for (size_t i = 0; i < n; ++i)
            {
                const float a = std::fabs(s[i]);
                if (a > peak) peak = a;
            }
            m_peak = peak;
        }
        m_validFrames += frames;
    }
    else if (in->u32BufferFlags == BUFFER_SILENT)
    {
        ++m_silentCalls;
    }

    out->u32ValidFrameCount = frames;
    out->u32BufferFlags = in->u32BufferFlags;
}

// ---------------------------------------------------------------- Echo reference (auxiliary input)

namespace
{
    constexpr GUID kModeCommunications = {0x98951333, 0xB9CD, 0x48B1, {0xA0, 0xA3, 0xFF, 0x40, 0x68, 0x2D, 0x73, 0xF7}};
    constexpr GUID kModeDefault = {0xC18E2F7E, 0x933D, 0x4965, {0xB7, 0xD1, 0x1E, 0xEF, 0x22, 0x8D, 0x2A, 0xF3}};
    constexpr GUID kModeSpeech = {0xFC1CFC9B, 0xB9D6, 0x4CFA, {0xB5, 0xE0, 0x4B, 0xB2, 0x16, 0x68, 0x78, 0xB2}};
}

bool CEngineApo::IsAecCandidate() const noexcept
{
    // An AEC APO must be a capture mode effect. Microsoft's AecApo sample only accepts COMMUNICATIONS;
    // this effect also runs AEC for DEFAULT (most apps) and SPEECH. RAW streams skip mode effects anyway.
    return (IsEqualGUID(m_mode, kModeCommunications) || IsEqualGUID(m_mode, kModeDefault) || IsEqualGUID(m_mode, kModeSpeech));
}

STDMETHODIMP CEngineApo::GetDesiredReferenceStreamProperties(APO_REFERENCE_STREAM_PROPERTIES* pProperties)
{
    if (!pProperties)
        return E_POINTER;
    // Post-volume loopback: the reference has the speaker volume applied, like the real echo.
    *pProperties = APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK;
    apolog::Write(L"[%s #%ld] GetDesiredReferenceStreamProperties -> POST_VOLUME_LOOPBACK", SlotName(), m_instanceId);
    return S_OK;
}

STDMETHODIMP CEngineApo::IsInputFormatSupported(IAudioMediaType* pRequested, IAudioMediaType** ppSupported)
{
    // Aux (reference) input: we want 48 kHz float32, any channel count (we downmix to mono).
    if (!pRequested || !ppSupported)
        return E_POINTER;
    *ppSupported = nullptr;
    UNCOMPRESSEDAUDIOFORMAT f{};
    HRESULT hr = pRequested->GetUncompressedAudioFormat(&f);
    if (FAILED(hr))
        return hr;
    const bool ok = IsEqualGUID(f.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && f.dwBytesPerSampleContainer == 4 &&
                    std::lround(f.fFramesPerSecond) == VoiceProcessor::kSampleRate && f.dwSamplesPerFrame >= 1;
    if (ok)
    {
        pRequested->AddRef();
        *ppSupported = pRequested;
        hr = S_OK;
    }
    else
    {
        UNCOMPRESSEDAUDIOFORMAT want = f;
        want.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        want.dwBytesPerSampleContainer = 4;
        want.dwValidBitsPerSample = 32;
        want.fFramesPerSecond = static_cast<float>(VoiceProcessor::kSampleRate);
        if (want.dwSamplesPerFrame < 1) want.dwSamplesPerFrame = 2;
        hr = CreateAudioMediaTypeFromUncompressedAudioFormat(&want, ppSupported);
        if (SUCCEEDED(hr))
            hr = S_FALSE;  // "not that one, but this one"
    }
    apolog::Write(L"[%s #%ld] aux IsInputFormatSupported req=%s %uch %.0fHz -> 0x%08X", SlotName(), m_instanceId,
                  IsEqualGUID(f.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? L"float" : L"pcm", f.dwSamplesPerFrame,
                  f.fFramesPerSecond, hr);
    return hr;
}

STDMETHODIMP CEngineApo::AddAuxiliaryInput(DWORD dwInputId, UINT32 cbDataSize, BYTE* pbyData, APO_CONNECTION_DESCRIPTOR* pInputConnection)
{
    if (!pInputConnection || !pInputConnection->pFormat)
        return E_INVALIDARG;
    if (m_refActive.load())
        return E_FAIL;  // one reference only
    UNCOMPRESSEDAUDIOFORMAT f{};
    pInputConnection->pFormat->GetUncompressedAudioFormat(&f);
    m_refChannels = f.dwSamplesPerFrame;
    m_refRate = f.fFramesPerSecond;
    m_refFloat = IsEqualGUID(f.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && f.dwBytesPerSampleContainer == 4;
    APO_REFERENCE_STREAM_PROPERTIES props = APO_REFERENCE_STREAM_PROPERTIES_NONE;
    if (pbyData && cbDataSize == sizeof(AcousticEchoCanceller_Reference_Input))
        props = reinterpret_cast<const AcousticEchoCanceller_Reference_Input*>(pbyData)->streamProperties;
    if (!m_refFloat || std::lround(m_refRate) != VoiceProcessor::kSampleRate || m_refChannels < 1)
    {
        apolog::Write(L"[%s #%ld] AddAuxiliaryInput id=%lu REJECTED format %s %uch %.0fHz", SlotName(), m_instanceId, dwInputId,
                      m_refFloat ? L"float" : L"non-float", m_refChannels, m_refRate);
        return E_INVALIDARG;
    }
    // The ring was allocated in Initialize and never moves. Empty it for a new reference when no
    // stream runs; under a running stream leave it alone (APOProcess is reading it).
    if (m_ref.Capacity() == 0)
        return E_OUTOFMEMORY;
    if (!m_bIsLocked)
        m_ref.Reset();
    else
        apolog::Write(L"[%s #%ld] AddAuxiliaryInput while locked: echo cancellation starts with the next stream", SlotName(), m_instanceId);
    m_refInputId = dwInputId;
    m_refFrames = m_refCalls = m_refDropped = m_refSilentCalls = 0;
    m_refActive.store(true);
    apolog::Write(L"[%s #%ld] AddAuxiliaryInput id=%lu cb=%u (%s) props=0x%X format float %uch %.0fHz maxFrames=%u",
                  SlotName(), m_instanceId, dwInputId, cbDataSize,
                  cbDataSize == sizeof(AcousticEchoCanceller_Reference_Input) ? L"AEC reference input" : L"other",
                  props, m_refChannels, m_refRate, pInputConnection->u32MaxFrameCount);
    return S_OK;
}

STDMETHODIMP CEngineApo::RemoveAuxiliaryInput(DWORD dwInputId)
{
    if (!m_refActive.load() || dwInputId != m_refInputId)
        return E_INVALIDARG;
    m_refActive.store(false);
    apolog::Write(L"[%s #%ld] RemoveAuxiliaryInput id=%lu after calls=%llu frames=%llu silent=%llu dropped=%llu", SlotName(),
                  m_instanceId, dwInputId, m_refCalls.load(), m_refFrames.load(), m_refSilentCalls.load(), m_refDropped.load());
    return S_OK;
}

STDMETHODIMP_(void) CEngineApo::AcceptInput(DWORD dwInputId, const APO_CONNECTION_PROPERTY* p)
{
    // Real-time: no allocation, no locks, no logging.
    if (!p || !m_refActive.load(std::memory_order_acquire) || dwInputId != m_refInputId)
        return;
    m_refCalls.fetch_add(1, std::memory_order_relaxed);
    const UINT32 frames = p->u32ValidFrameCount;
    float mono[1024];
    const float* src = reinterpret_cast<const float*>(p->pBuffer);
    const bool silent = p->u32BufferFlags != BUFFER_VALID || !src;
    if (silent)
        m_refSilentCalls.fetch_add(1, std::memory_order_relaxed);
    const float inv = 1.0f / static_cast<float>(m_refChannels);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);   // stamps the reference writes (ReferenceSync follows their real rate)
    const int64_t ticks = now.QuadPart;
    for (UINT32 done = 0; done < frames;)
    {
        const UINT32 n = std::min<UINT32>(frames - done, 1024);
        for (UINT32 i = 0; i < n; ++i)
        {
            float acc = 0.0f;
            if (!silent)
                for (UINT32 c = 0; c < m_refChannels; ++c)
                    acc += src[(static_cast<size_t>(done) + i) * m_refChannels + c];
            // Any app can play NaN/Inf or absurd values; one of them would poison AEC3's state for good.
            acc *= inv;
            mono[i] = std::isfinite(acc) ? std::clamp(acc, -4.0f, 4.0f) : 0.0f;
        }
        const size_t written = m_ref.Write(mono, n, ticks);
        m_refDropped.fetch_add(n - written, std::memory_order_relaxed);
        done += n;
    }
    m_refFrames.fetch_add(frames, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- IAudioSystemEffects3

// Windows Settings > Sound > (mic) > Audio enhancements lists an effect pack's controllable effects
// (like Voice Clarity's "Voice Focus"). We expose AEC, NS and AGC as the standard effect types; their
// on/off state lives in our settings key, so Windows Settings, our app and the scripts stay in sync.
namespace
{
    constexpr GUID kEffectAec = {0x6f64adbe, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};  // ACOUSTIC_ECHO_CANCELLATION
    // Note: the Settings page (AudioHandlers.dll, build 26300) only draws effect rows for Microsoft's own
    // Voice Clarity pack (its pack id is hard-coded); other packs only get name + author. We still report
    // honest effect types for any other consumer of this interface.
    constexpr GUID kEffectNs = {0x6f64adbf, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};   // NOISE_SUPPRESSION
    constexpr GUID kEffectAgc = {0x6f64adc0, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};  // AUTOMATIC_GAIN_CONTROL

    // Events handed to us by GetControllableSystemEffectsList: signalled when our effect states change.
    // We keep our OWN duplicate of each event (closed when its effect instance goes away), so we never
    // signal a handle value the engine has closed and maybe reused for something else.
    struct FxEvent { const void* owner; HANDLE event; };
    SRWLOCK g_fxEventsLock = SRWLOCK_INIT;
    std::vector<FxEvent>* g_fxEvents = nullptr;   // leaked on purpose (process lifetime)

    void ForgetFxEvents(const void* owner) noexcept
    {
        AcquireSRWLockExclusive(&g_fxEventsLock);
        if (g_fxEvents)
            std::erase_if(*g_fxEvents, [owner](const FxEvent& e) {
                if (e.owner != owner) return false;
                CloseHandle(e.event);
                return true;
            });
        ReleaseSRWLockExclusive(&g_fxEventsLock);
    }
}

void SignalSystemEffectsChanged() noexcept
{
    // Called on the settings watcher thread (after VoiceProcessor rebuilds stages) and after
    // SetAudioSystemEffectState.
    AcquireSRWLockShared(&g_fxEventsLock);
    if (g_fxEvents)
        for (const FxEvent& e : *g_fxEvents) SetEvent(e.event);
    ReleaseSRWLockShared(&g_fxEventsLock);
}

STDMETHODIMP CEngineApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects, UINT* numEffects, HANDLE event)
{
    if (!effects || !numEffects)
        return E_POINTER;
    *effects = nullptr;
    *numEffects = 0;
    const Settings s = Settings::ReadFromRegistry(m_deviceGuid);
    auto* list = static_cast<AUDIO_SYSTEMEFFECT*>(CoTaskMemAlloc(3 * sizeof(AUDIO_SYSTEMEFFECT)));
    if (!list)
        return E_OUTOFMEMORY;
    auto state = [](uint32_t on) { return on ? AUDIO_SYSTEMEFFECT_STATE_ON : AUDIO_SYSTEMEFFECT_STATE_OFF; };
    list[0] = {kEffectAec, TRUE, state(s.aecEnabled)};
    list[1] = {kEffectNs, TRUE, state(s.nsEnabled)};
    list[2] = {kEffectAgc, TRUE, state(s.agcEnabled)};
    *effects = list;
    *numEffects = 3;
    if (event)
    {
        // One event per instance: a newer call replaces the older event.
        ForgetFxEvents(this);
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &dup, EVENT_MODIFY_STATE, FALSE, 0))
        {
            AcquireSRWLockExclusive(&g_fxEventsLock);
            if (!g_fxEvents) g_fxEvents = new (std::nothrow) std::vector<FxEvent>();
            bool kept = false;
            if (g_fxEvents)
            {
                try { g_fxEvents->push_back({this, dup}); kept = true; } catch (...) {}
            }
            ReleaseSRWLockExclusive(&g_fxEventsLock);
            if (!kept) CloseHandle(dup);
        }
    }
    apolog::Write(L"[%s #%ld] GetControllableSystemEffectsList -> AEC=%u NS=%u AGC=%u (event=%p)", SlotName(), m_instanceId,
                  s.aecEnabled, s.nsEnabled, s.agcEnabled, event);
    return S_OK;
}

STDMETHODIMP CEngineApo::SetAudioSystemEffectState(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state)
{
    // Changes this microphone's own settings if it has custom ones, otherwise the defaults.
    const wchar_t* scope = Settings::IsDeviceCustom(m_deviceGuid) ? m_deviceGuid : nullptr;
    Settings s = Settings::ReadFromRegistry(scope);
    const uint32_t on = state == AUDIO_SYSTEMEFFECT_STATE_ON ? 1u : 0u;
    const wchar_t* name = L"?";
    if (IsEqualGUID(effectId, kEffectAec)) { s.aecEnabled = on; name = L"AEC"; }
    else if (IsEqualGUID(effectId, kEffectNs)) { s.nsEnabled = on; name = L"NS"; }
    else if (IsEqualGUID(effectId, kEffectAgc)) { s.agcEnabled = on; name = L"AGC"; }
    else return E_NOTFOUND;
    const LONG err = Settings::WriteToRegistry(s, scope);  // persists; the watcher applies it live
    apolog::Write(L"[%s #%ld] SetAudioSystemEffectState %s -> %s (registry write %ld)", SlotName(), m_instanceId, name,
                  on ? L"ON" : L"OFF", err);
    if (err != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(err);  // nothing stored: Windows Settings must not show the new state
    SignalSystemEffectsChanged();
    return S_OK;
}

// ---------------------------------------------------------------- Mic volume slider (notifications)

float CEngineApo::SliderGainDb(const Settings& s) const noexcept
{
    if (m_muted.load(std::memory_order_relaxed))
        return VoiceProcessor::kMutedDb;  // silence, also with AGC2 on
    if (s.agcEnabled)              // AGC2 owns the level: the slider is ignored by design
        return 0.0f;
    // auto: apply the slider unless the microphone has its own volume control, guessed from the bus
    // (USB/Bluetooth devices have one). Windows' QueryHardwareSupport is only logged: the Realtek built-in
    // mic reports "hardware volume" too, yet its slider does nothing once our pack replaces Realtek's chain.
    const bool apply = s.gainMode == 1 || (s.gainMode == 0 && !m_hardwareVolumeBus);
    return apply ? m_sliderDb.load(std::memory_order_relaxed) : 0.0f;
}

STDMETHODIMP CEngineApo::GetApoNotificationRegistrationInfo(APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count)
{
    if (!apoNotifications || !count)
        return E_POINTER;
    *apoNotifications = nullptr;
    *count = 0;
    // Discovery-only instances never process audio, so they need no slider updates. Each registration also
    // costs audiodg registry handles that it never gives back (measured 2026-11-13), so skip it there.
    if (!m_device || m_discoveryOnly)
        return S_OK;  // nothing to register for
    auto* d = static_cast<APO_NOTIFICATION_DESCRIPTOR*>(CoTaskMemAlloc(sizeof(APO_NOTIFICATION_DESCRIPTOR)));
    if (!d)
        return E_OUTOFMEMORY;
    ZeroMemory(d, sizeof(*d));
    d->type = APO_NOTIFICATION_TYPE_ENDPOINT_VOLUME2;   // carries the volume in dB
    d->audioEndpointVolume.device = m_device;
    m_device->AddRef();                                  // the caller releases it
    *apoNotifications = d;
    *count = 1;
    apolog::Write(L"[%s #%ld] registered for endpoint volume notifications", SlotName(), m_instanceId);
    return S_OK;
}

STDMETHODIMP_(void) CEngineApo::HandleNotification(APO_NOTIFICATION* n)
{
    if (!n)
        return;
    if (n->type == APO_NOTIFICATION_TYPE_ENDPOINT_VOLUME2 && n->audioEndpointVolumeChange2.volume)
    {
        const auto* v = n->audioEndpointVolumeChange2.volume;
        const bool muted = v->notificationData && v->notificationData->bMuted;
        if (std::isfinite(v->masterVolumeInDb))
            m_sliderDb.store(v->masterVolumeInDb, std::memory_order_relaxed);
        if (v->notificationData)
            m_muted.store(muted, std::memory_order_relaxed);
        apolog::Write(L"[%s #%ld] slider %.2f dB (range %.1f..%.1f) muted=%d -> applying %.2f dB",
                      SlotName(), m_instanceId, v->masterVolumeInDb, v->volumeMinInDb, v->volumeMaxInDb, muted ? 1 : 0,
                      SliderGainDb(settings::Current(m_devIdx)));
    }
    else if (n->type == APO_NOTIFICATION_TYPE_ENDPOINT_VOLUME && n->audioEndpointVolumeChange.volume)
    {
        apolog::Write(L"[%s #%ld] slider (v1) scalar %.3f", SlotName(), m_instanceId, n->audioEndpointVolumeChange.volume->fMasterVolume);
    }
}

// ---------------------------------------------------------------- IAudioSystemEffects2

STDMETHODIMP CEngineApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE /*Event*/)
{
    if (!ppEffectsIds || !pcEffects)
        return E_POINTER;
    *ppEffectsIds = nullptr;  // controllable effects are reported through IAudioSystemEffects3
    *pcEffects = 0;
    return S_OK;
}
