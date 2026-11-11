#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <atomic>

enum class ApoSlot { MFX, EFX };

// Milestone 1, step 1: pass-through probe.
// Audio is copied through untouched. Non-real-time callbacks log what the audio engine
// tells us (slot, processing mode, endpoint, format); APOProcess only updates counters.
class CProbeApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects2
{
public:
    explicit CProbeApo(ApoSlot slot) noexcept;

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IAudioProcessingObject
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;

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
    ~CProbeApo() override;
    const wchar_t* SlotName() const noexcept { return m_slot == ApoSlot::MFX ? L"MFX" : L"EFX"; }
    void ResetStats() noexcept;

    std::atomic<ULONG> m_refs{1};
    const ApoSlot m_slot;
    const long m_instanceId;
    GUID m_mode{};

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
