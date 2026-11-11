#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <atomic>
#include "VoiceProcessor.h"

// MFX = bit-exact probe; EFX = NS appended to the OEM endpoint chain; Pack = NS as our own effect pack.
enum class ApoSlot { MFX, EFX, Pack };


// Milestone 1, step 1: pass-through probe.
// Audio is copied through untouched. Non-real-time callbacks log what the audio engine
// tells us (slot, processing mode, endpoint, format); APOProcess only updates counters.
class CProbeApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects2
{
public:
    // The audio engine creates APOs AGGREGATED (pUnkOuter != null), so we implement the
    // standard COM aggregation pattern: an inner non-delegating IUnknown that owns the
    // lifetime, while the IUnknown methods on our interfaces delegate to the outer object.
    CProbeApo(ApoSlot slot, IUnknown* outer) noexcept;
    IUnknown* NonDelegatingUnknown() noexcept { return &m_inner; }

    // IUnknown (delegating)
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IAudioProcessingObject
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHOD(GetLatency)(HNSTIME* pTime) override;

    STDMETHOD(IsInputFormatSupported)(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedInputFormat,
                                      IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType* pOppositeFormat, IAudioMediaType* pRequestedOutputFormat,
                                       IAudioMediaType** ppSupportedOutputFormat) override;

    // IAudioProcessingObjectRT
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
                                 UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // IAudioProcessingObjectConfiguration
    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                              UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    // IAudioSystemEffects2
    STDMETHOD(GetEffectsList)(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;

    static long LiveObjects() noexcept { return s_liveObjects.load(); }

private:
    struct InnerUnknown final : IUnknown
    {
        CProbeApo* self;
        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override { return self->NonDelegatingQueryInterface(riid, ppv); }
        STDMETHOD_(ULONG, AddRef)() override { return ++self->m_refs; }
        STDMETHOD_(ULONG, Release)() override
        {
            ULONG r = --self->m_refs;
            if (r == 0)
                delete self;
            return r;
        }
    };

    ~CProbeApo() override;
    HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppv) noexcept;
    const wchar_t* SlotName() const noexcept { return m_slot == ApoSlot::MFX ? L"MFX" : m_slot == ApoSlot::EFX ? L"EFX" : L"PACK"; }
    void ResetStats() noexcept;

    InnerUnknown m_inner;
    IUnknown* m_outer;  // the aggregating object, or &m_inner when not aggregated
    std::atomic<ULONG> m_refs{1};
    const ApoSlot m_slot;
    const long m_instanceId;
    GUID m_mode{};

    // Noise suppression (EFX only). Prepared in LockForProcess, used in APOProcess.
    VoiceProcessor m_proc;

    // Cached at LockForProcess (never changes while locked).
    UINT32 m_bytesPerFrame = 0;
    UINT32 m_channels = 0;
    bool m_isFloat32 = false;

    // Written only by the real-time thread; read after UnlockForProcess.
    UINT64 m_processCalls = 0;
    UINT64 m_validFrames = 0;
    UINT64 m_silentCalls = 0;
    UINT32 m_minPeriod = 0xFFFFFFFF;
    UINT32 m_maxPeriod = 0;
    float m_peak = 0.0f;
    ULONGLONG m_lockTick = 0;

    static std::atomic<long> s_liveObjects;
    static std::atomic<long> s_nextInstance;
};
