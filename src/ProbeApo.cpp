#include "ProbeApo.h"
#include "Guids.h"
#include "Log.h"
#include "Telemetry.h"
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

std::atomic<long> CProbeApo::s_liveObjects{0};
std::atomic<long> CProbeApo::s_nextInstance{0};

namespace
{
#pragma warning(disable : 4815) // CRegAPOProperties<1> has a zero-length IID array by design
    CRegAPOProperties<1> g_regMfx(CLSID_WebrtcVoiceApoMFX, L"WebRTC Voice Probe (MFX)", L"Copyright (c) RadegasTCZ",
                                  1, 0, __uuidof(IAudioProcessingObject), APO_FLAG_DEFAULT);
    CRegAPOProperties<1> g_regEfx(CLSID_WebrtcVoiceApoEFX, L"WebRTC Voice Probe (EFX)", L"Copyright (c) RadegasTCZ",
                                  1, 0, __uuidof(IAudioProcessingObject), APO_FLAG_DEFAULT);
    CRegAPOProperties<1> g_regPack(CLSID_WebrtcAudioEnginePack, L"WebRTC Audio Engine", L"Copyright (c) RadegasTCZ",
                                   1, 0, __uuidof(IAudioProcessingObject), APO_FLAG_DEFAULT);

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

CProbeApo::CProbeApo(ApoSlot slot, IUnknown* outer) noexcept
    : CBaseAudioProcessingObject(slot == ApoSlot::MFX   ? &g_regMfx.m_Properties
                                 : slot == ApoSlot::EFX ? &g_regEfx.m_Properties
                                                        : &g_regPack.m_Properties),
      m_slot(slot),
      m_instanceId(++s_nextInstance)
{
    m_inner.self = this;
    m_outer = outer ? outer : &m_inner;
    ++s_liveObjects;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    apolog::Write(L"[%s #%ld] created in host %s (aggregated=%d)", SlotName(), m_instanceId, exe, outer ? 1 : 0);
}

CProbeApo::~CProbeApo()
{
    if (m_device)
        m_device->Release();
    apolog::Write(L"[%s #%ld] destroyed", SlotName(), m_instanceId);
    --s_liveObjects;
}

// ---------------------------------------------------------------- IUnknown

STDMETHODIMP CProbeApo::QueryInterface(REFIID riid, void** ppv) { return m_outer->QueryInterface(riid, ppv); }
STDMETHODIMP_(ULONG) CProbeApo::AddRef() { return m_outer->AddRef(); }
STDMETHODIMP_(ULONG) CProbeApo::Release() { return m_outer->Release(); }

HRESULT CProbeApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) noexcept
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
    else if (riid == __uuidof(IAudioSystemEffects3) && m_slot == ApoSlot::Pack)
        *ppv = static_cast<IAudioSystemEffects3*>(this);
    else if (riid == __uuidof(IAudioProcessingObjectNotifications) && m_slot == ApoSlot::Pack)
        *ppv = static_cast<IAudioProcessingObjectNotifications*>(this);
    else
    {
        // Useful for discovery: shows which optional interfaces (CAPX, AEC, ...) the engine asks for.
        wchar_t g[40];
        apolog::GuidToString(riid, g);
        apolog::Write(L"[%s #%ld] QueryInterface for unsupported %s", SlotName(), m_instanceId, g);
        return E_NOINTERFACE;
    }
    AddRef();  // goes to the outer object when aggregated (COM aggregation rule)
    return S_OK;
}

// ---------------------------------------------------------------- Initialize

STDMETHODIMP CProbeApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
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
                std::wstring lower(id);
                for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
                m_hardwareVolumeBus = lower.find(L"usb#") != std::wstring::npos || lower.find(L"usb\\") != std::wstring::npos ||
                                      lower.find(L"bth") != std::wstring::npos || lower.find(L"bluetooth") != std::wstring::npos;
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
    apolog::Write(L"[%s #%ld] Initialize cb=%u (%s) mode=%s %s discoveryOnly=%d endpoint=\"%s\" mmdevice=%s io=%s hwVolumeBus=%d",
                  SlotName(), m_instanceId, cbDataSize, initKind,
                  ModeName(m_mode), modeGuid, discoveryOnly, endpointName, mmEndpointId, endpointId,
                  m_hardwareVolumeBus ? 1 : 0);

    m_bIsInitialized = true;
    return S_OK;
}

STDMETHODIMP CProbeApo::GetLatency(HNSTIME* pTime)
{
    if (!pTime)
        return E_POINTER;
    // HNSTIME is in 100 ns units.
    *pTime = m_proc.IsActive() ? static_cast<HNSTIME>(m_proc.LatencyFrames()) * 10000000 / m_proc.SampleRate() : 0;
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
            swprintf_s(out, L"%s %uch %ub/%ub %.0fHz",
                       IsEqualGUID(f.guidFormatType, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? L"float" : L"pcm",
                       f.dwSamplesPerFrame, f.dwValidBitsPerSample, f.dwBytesPerSampleContainer * 8, f.fFramesPerSecond);
    }
}

STDMETHODIMP CProbeApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
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

STDMETHODIMP CProbeApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
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

void CProbeApo::ResetStats() noexcept
{
    m_processCalls = m_validFrames = m_silentCalls = 0;
    m_minPeriod = 0xFFFFFFFF;
    m_maxPeriod = 0;
    m_peak = 0.0f;
}

STDMETHODIMP CProbeApo::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
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
    m_lockTick = GetTickCount64();

    // Voice processing runs in the EFX and in the effect pack; the plain MFX stays a pure probe.
    m_proc.Deactivate();
    if (m_slot == ApoSlot::Pack && m_device)
    {
        // Starting slider position (notifications only arrive on changes).
        IAudioEndpointVolume* ev = nullptr;
        HRESULT hrv = m_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ev));
        float db = 0.0f;
        if (SUCCEEDED(hrv) && ev)
        {
            hrv = ev->GetMasterVolumeLevel(&db);
            ev->Release();
        }
        if (SUCCEEDED(hrv))
            m_sliderDb.store(db, std::memory_order_relaxed);
        apolog::Write(L"[%s #%ld] initial slider: hr=0x%08X %.2f dB, hwVolumeBus=%d", SlotName(), m_instanceId, hrv, db,
                      m_hardwareVolumeBus ? 1 : 0);
    }
    if (m_slot != ApoSlot::MFX)
    {
        settings::EnsureWatcher();  // live settings (non-RT here)
        const Settings s = settings::Current();
        bool ok = false;
        if (!m_isFloat32)
            apolog::Write(L"[%s #%ld] processing off: format is not float32", SlotName(), m_instanceId);
        else
        {
            const UINT32 maxFrames = std::max(u32NumInputConnections ? ppInputConnections[0]->u32MaxFrameCount : 0u,
                                              u32NumOutputConnections ? ppOutputConnections[0]->u32MaxFrameCount : 0u);
            m_proc.SetReference(m_refActive.load() ? &m_ref : nullptr);  // echo reference attached by AddAuxiliaryInput
            if (m_slot == ApoSlot::Pack && !m_telemetry)
                m_telemetry = telemetry::ClaimSlot(m_endpointKey, m_endpointName);  // per-device meters for the tray app (non-RT)
            ok = m_proc.Prepare(static_cast<int>(m_channels), static_cast<int>(std::lround(fmt.fFramesPerSecond)), s,
                                static_cast<int>(maxFrames ? maxFrames : 4096));
            apolog::Write(L"[%s #%ld] NS %s: bypass=%u hpf=%u@%uHz ns=%u level=%u channels=%u rate=%.0f%s maxFrames=%u latency=%d frames",
                          SlotName(), m_instanceId, ok ? L"ACTIVE" : L"off (unsupported format)", s.bypass, s.hpfEnabled,
                          s.hpfHz, s.nsEnabled, s.nsLevel, m_channels, fmt.fFramesPerSecond,
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

STDMETHODIMP CProbeApo::UnlockForProcess()
{
    const double seconds = (GetTickCount64() - m_lockTick) / 1000.0;
    if (m_refActive.load())
        apolog::Write(L"[%s #%ld] echo reference: calls=%llu frames=%llu (%.1f s) silentCalls=%llu dropped=%llu, AEC3 %s, reference underruns=%llu", SlotName(),
                      m_instanceId, m_refCalls.load(), m_refFrames.load(), m_refFrames.load() / 48000.0, m_refSilentCalls.load(), m_refDropped.load(),
                      m_proc.HasAec() ? L"running" : L"not running", m_proc.ReferenceUnderruns());
    const double peakDb = m_peak > 0.0f ? 20.0 * std::log10(static_cast<double>(m_peak)) : -999.0;
    apolog::Write(L"[%s #%ld] UnlockForProcess mode=%s after %.1f s: calls=%llu validFrames=%llu silentCalls=%llu period=%u..%u frames peak=%.1f dBFS underruns=%llu",
                  SlotName(), m_instanceId, ModeName(m_mode), seconds, m_processCalls, m_validFrames, m_silentCalls,
                  m_maxPeriod ? m_minPeriod : 0, m_maxPeriod, peakDb, m_proc.Underruns());
    return CBaseAudioProcessingObject::UnlockForProcess();
}

// ---------------------------------------------------------------- Real-time processing
// Rules: no allocation, no locks, no logging, no waiting.

STDMETHODIMP_(void) CProbeApo::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                          UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0)
        return;

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
        const Settings cur = settings::Current();
        const float gainDb = SliderGainDb(cur);
        if (in->u32BufferFlags == BUFFER_VALID)
            m_proc.Process(reinterpret_cast<const float*>(in->pBuffer), o, static_cast<int>(frames), cur, gainDb);
        else
        {
            std::memset(o, 0, static_cast<size_t>(frames) * m_bytesPerFrame);
            m_proc.Process(o, o, static_cast<int>(frames), cur, gainDb);
            ++m_silentCalls;
        }
        VoiceProcessor::Meters mt;
        if (m_telemetry && m_proc.TakeMeters(mt))
        {
            telemetry::Values d{};
            d.inputDbfs = mt.inputDbfs; d.outputDbfs = mt.outputDbfs; d.referenceDbfs = mt.referenceDbfs;
            d.echoRemovedDb = mt.echoRemovedDb; d.agcGainDb = mt.agcGainDb; d.limiterDb = mt.limiterDb;
            d.aecRunning = mt.aecRunning ? 1u : 0u;
            d.sampleRate = static_cast<uint32_t>(m_proc.SampleRate());
            telemetry::Publish(m_telemetry, d);
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

bool CProbeApo::IsAecCandidate() const noexcept
{
    // An AEC APO must be a capture mode effect. Microsoft's AecApo sample only accepts COMMUNICATIONS;
    // the owner wants AEC for DEFAULT (most apps) and SPEECH too. RAW streams skip mode effects anyway.
    return m_slot == ApoSlot::Pack &&
           (IsEqualGUID(m_mode, kModeCommunications) || IsEqualGUID(m_mode, kModeDefault) || IsEqualGUID(m_mode, kModeSpeech));
}

STDMETHODIMP CProbeApo::GetDesiredReferenceStreamProperties(APO_REFERENCE_STREAM_PROPERTIES* pProperties)
{
    if (!pProperties)
        return E_POINTER;
    // Post-volume loopback: the reference has the speaker volume applied, like the real echo.
    *pProperties = APO_REFERENCE_STREAM_PROPERTIES_POST_VOLUME_LOOPBACK;
    apolog::Write(L"[%s #%ld] GetDesiredReferenceStreamProperties -> POST_VOLUME_LOOPBACK", SlotName(), m_instanceId);
    return S_OK;
}

STDMETHODIMP CProbeApo::IsInputFormatSupported(IAudioMediaType* pRequested, IAudioMediaType** ppSupported)
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

STDMETHODIMP CProbeApo::AddAuxiliaryInput(DWORD dwInputId, UINT32 cbDataSize, BYTE* pbyData, APO_CONNECTION_DESCRIPTOR* pInputConnection)
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
    m_ref.Init(VoiceProcessor::kSampleRate);  // 1 s of mono reference
    m_refInputId = dwInputId;
    m_refFrames = m_refCalls = m_refDropped = m_refSilentCalls = 0;
    m_refActive.store(true);
    apolog::Write(L"[%s #%ld] AddAuxiliaryInput id=%lu cb=%u (%s) props=0x%X format float %uch %.0fHz maxFrames=%u",
                  SlotName(), m_instanceId, dwInputId, cbDataSize,
                  cbDataSize == sizeof(AcousticEchoCanceller_Reference_Input) ? L"AEC reference input" : L"other",
                  props, m_refChannels, m_refRate, pInputConnection->u32MaxFrameCount);
    return S_OK;
}

STDMETHODIMP CProbeApo::RemoveAuxiliaryInput(DWORD dwInputId)
{
    if (!m_refActive.load() || dwInputId != m_refInputId)
        return E_INVALIDARG;
    m_refActive.store(false);
    apolog::Write(L"[%s #%ld] RemoveAuxiliaryInput id=%lu after calls=%llu frames=%llu silent=%llu dropped=%llu", SlotName(),
                  m_instanceId, dwInputId, m_refCalls.load(), m_refFrames.load(), m_refSilentCalls.load(), m_refDropped.load());
    return S_OK;
}

STDMETHODIMP_(void) CProbeApo::AcceptInput(DWORD dwInputId, const APO_CONNECTION_PROPERTY* p)
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
    for (UINT32 done = 0; done < frames;)
    {
        const UINT32 n = std::min<UINT32>(frames - done, 1024);
        for (UINT32 i = 0; i < n; ++i)
        {
            float acc = 0.0f;
            if (!silent)
                for (UINT32 c = 0; c < m_refChannels; ++c)
                    acc += src[(static_cast<size_t>(done) + i) * m_refChannels + c];
            mono[i] = acc * inv;
        }
        const size_t written = m_ref.Write(mono, n);
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
    SRWLOCK g_fxEventsLock = SRWLOCK_INIT;
    std::vector<HANDLE>* g_fxEvents = nullptr;
}

void SignalSystemEffectsChanged() noexcept
{
    // Called on the settings watcher thread (after VoiceProcessor rebuilds stages) and after
    // SetAudioSystemEffectState.
    AcquireSRWLockShared(&g_fxEventsLock);
    if (g_fxEvents)
        for (HANDLE e : *g_fxEvents) SetEvent(e);
    ReleaseSRWLockShared(&g_fxEventsLock);
}

STDMETHODIMP CProbeApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects, UINT* numEffects, HANDLE event)
{
    if (!effects || !numEffects)
        return E_POINTER;
    *effects = nullptr;
    *numEffects = 0;
    const Settings s = Settings::ReadFromRegistry();
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
        AcquireSRWLockExclusive(&g_fxEventsLock);
        if (!g_fxEvents) g_fxEvents = new std::vector<HANDLE>();
        if (std::find(g_fxEvents->begin(), g_fxEvents->end(), event) == g_fxEvents->end())
            g_fxEvents->push_back(event);
        ReleaseSRWLockExclusive(&g_fxEventsLock);
        if (!m_fxEvent) m_fxEvent = event;
    }
    apolog::Write(L"[%s #%ld] GetControllableSystemEffectsList -> AEC=%u NS=%u AGC=%u (event=%p)", SlotName(), m_instanceId,
                  s.aecEnabled, s.nsEnabled, s.agcEnabled, event);
    return S_OK;
}

STDMETHODIMP CProbeApo::SetAudioSystemEffectState(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state)
{
    Settings s = Settings::ReadFromRegistry();
    const uint32_t on = state == AUDIO_SYSTEMEFFECT_STATE_ON ? 1u : 0u;
    const wchar_t* name = L"?";
    if (IsEqualGUID(effectId, kEffectAec)) { s.aecEnabled = on; name = L"AEC"; }
    else if (IsEqualGUID(effectId, kEffectNs)) { s.nsEnabled = on; name = L"NS"; }
    else if (IsEqualGUID(effectId, kEffectAgc)) { s.agcEnabled = on; name = L"AGC"; }
    else return E_NOTFOUND;
    settings::Publish(s);                         // takes effect right away in this process
    const LONG err = Settings::WriteToRegistry(s); // persists + reaches the settings app
    apolog::Write(L"[%s #%ld] SetAudioSystemEffectState %s -> %s (registry write %ld)", SlotName(), m_instanceId, name,
                  on ? L"ON" : L"OFF", err);
    SignalSystemEffectsChanged();
    return S_OK;
}

// ---------------------------------------------------------------- Mic volume slider (notifications)

float CProbeApo::SliderGainDb(const Settings& s) const noexcept
{
    if (m_slot != ApoSlot::Pack)   // in the OEM chain the OEM applies the slider itself
        return 0.0f;
    if (s.agcEnabled)              // AGC2 owns the level: the slider is ignored (owner's request)
        return 0.0f;
    const bool apply = s.gainMode == 1 || (s.gainMode == 0 && !m_hardwareVolumeBus);
    return apply ? m_sliderDb.load(std::memory_order_relaxed) : 0.0f;
}

STDMETHODIMP CProbeApo::GetApoNotificationRegistrationInfo(APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count)
{
    if (!apoNotifications || !count)
        return E_POINTER;
    *apoNotifications = nullptr;
    *count = 0;
    if (!m_device)
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

STDMETHODIMP_(void) CProbeApo::HandleNotification(APO_NOTIFICATION* n)
{
    if (!n)
        return;
    if (n->type == APO_NOTIFICATION_TYPE_ENDPOINT_VOLUME2 && n->audioEndpointVolumeChange2.volume)
    {
        const auto* v = n->audioEndpointVolumeChange2.volume;
        const bool muted = v->notificationData && v->notificationData->bMuted;
        m_sliderDb.store(v->masterVolumeInDb, std::memory_order_relaxed);
        apolog::Write(L"[%s #%ld] slider %.2f dB (range %.1f..%.1f) muted=%d -> applying %.2f dB",
                      SlotName(), m_instanceId, v->masterVolumeInDb, v->volumeMinInDb, v->volumeMaxInDb, muted ? 1 : 0,
                      SliderGainDb(settings::Current()));
    }
    else if (n->type == APO_NOTIFICATION_TYPE_ENDPOINT_VOLUME && n->audioEndpointVolumeChange.volume)
    {
        apolog::Write(L"[%s #%ld] slider (v1) scalar %.3f", SlotName(), m_instanceId, n->audioEndpointVolumeChange.volume->fMasterVolume);
    }
}

// ---------------------------------------------------------------- IAudioSystemEffects2

STDMETHODIMP CProbeApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE /*Event*/)
{
    if (!ppEffectsIds || !pcEffects)
        return E_POINTER;
    *ppEffectsIds = nullptr;  // the probe advertises no user-visible effects
    *pcEffects = 0;
    return S_OK;
}
