#include "ProbeApo.h"
#include "Guids.h"
#include "Log.h"
#include <functiondiscoverykeys_devpkey.h>
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

    const APOInitSystemEffects* v1 = nullptr;
    const APOInitSystemEffects2* v2 = nullptr;
    if (cbDataSize == sizeof(APOInitSystemEffects2))
    {
        v2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        m_mode = v2->AudioProcessingMode;
    }
    else if (cbDataSize == sizeof(APOInitSystemEffects))
    {
        v1 = reinterpret_cast<const APOInitSystemEffects*>(pbyData);
    }

    // Endpoint name + id, for confirming which mic we sit on.
    wchar_t endpointName[256] = L"?";
    wchar_t endpointId[256] = L"?";
    IPropertyStore* endpointProps = v2 ? v2->pAPOEndpointProperties : (v1 ? v1->pAPOEndpointProperties : nullptr);
    if (endpointProps)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(endpointProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            wcsncpy_s(endpointName, pv.pwszVal, _TRUNCATE);
        PropVariantClear(&pv);
    }
    if (v2 && v2->pDeviceCollection)
    {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(v2->pDeviceCollection->Item(v2->nSoftwareIoDeviceInCollection, &dev)) && dev)
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id)
            {
                wcsncpy_s(endpointId, id, _TRUNCATE);
                CoTaskMemFree(id);
            }
            dev->Release();
        }
    }

    wchar_t modeGuid[40];
    apolog::GuidToString(m_mode, modeGuid);
    apolog::Write(L"[%s #%ld] Initialize cb=%u (%s) mode=%s %s discoveryOnly=%d endpoint=\"%s\" id=%s",
                  SlotName(), m_instanceId, cbDataSize,
                  v2 ? L"APOInitSystemEffects2" : (v1 ? L"APOInitSystemEffects" : L"other"),
                  ModeName(m_mode), modeGuid, v2 ? v2->InitializeForDiscoveryOnly : -1, endpointName, endpointId);

    m_bIsInitialized = true;
    return S_OK;
}

STDMETHODIMP CProbeApo::GetLatency(HNSTIME* pTime)
{
    if (!pTime)
        return E_POINTER;
    // HNSTIME is in 100 ns units.
    *pTime = m_proc.IsActive() ? static_cast<HNSTIME>(VoiceProcessor::kTotalLatency) * 10000000 / VoiceProcessor::kSampleRate : 0;
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
    if (m_slot != ApoSlot::MFX)
    {
        settings::EnsureWatcher();  // live settings (non-RT here)
        const Settings s = settings::Current();
        bool ok = false;
        if (!m_isFloat32)
            apolog::Write(L"[%s #%ld] processing off: format is not float32", SlotName(), m_instanceId);
        else
        {
            ok = m_proc.Prepare(static_cast<int>(m_channels), static_cast<int>(fmt.fFramesPerSecond), s);
            apolog::Write(L"[%s #%ld] NS %s: bypass=%u hpf=%u@%uHz ns=%u level=%u channels=%u rate=%.0f latency=%d samples",
                          SlotName(), m_instanceId, ok ? L"ACTIVE" : L"off (unsupported format)", s.bypass, s.hpfEnabled,
                          s.hpfHz, s.nsEnabled, s.nsLevel, m_channels, fmt.fFramesPerSecond, ok ? VoiceProcessor::kTotalLatency : 0);
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
    const double peakDb = m_peak > 0.0f ? 20.0 * std::log10(static_cast<double>(m_peak)) : -999.0;
    apolog::Write(L"[%s #%ld] UnlockForProcess mode=%s after %.1f s: calls=%llu validFrames=%llu silentCalls=%llu period=%u..%u frames peak=%.1f dBFS",
                  SlotName(), m_instanceId, ModeName(m_mode), seconds, m_processCalls, m_validFrames, m_silentCalls,
                  m_maxPeriod ? m_minPeriod : 0, m_maxPeriod, peakDb);
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
        if (in->u32BufferFlags == BUFFER_VALID)
            m_proc.Process(reinterpret_cast<const float*>(in->pBuffer), o, static_cast<int>(frames), settings::Current());
        else
        {
            std::memset(o, 0, static_cast<size_t>(frames) * m_bytesPerFrame);
            m_proc.Process(o, o, static_cast<int>(frames), settings::Current());
            ++m_silentCalls;
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

// ---------------------------------------------------------------- IAudioSystemEffects2

STDMETHODIMP CProbeApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE /*Event*/)
{
    if (!ppEffectsIds || !pcEffects)
        return E_POINTER;
    *ppEffectsIds = nullptr;  // the probe advertises no user-visible effects
    *pcEffects = 0;
    return S_OK;
}
