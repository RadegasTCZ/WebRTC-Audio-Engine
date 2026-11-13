#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <audioengineextensionapo.h>
#include <atomic>
#include "VoiceProcessor.h"
#include "ReferenceRing.h"
#include "Telemetry.h"
#include "EchoTimeline.h"

// Signals Windows (Settings > Audio enhancements) that our controllable effect states changed.
void SignalSystemEffectsChanged() noexcept;


// The "WebRTC Audio Engine" effect pack APO: a capture mode effect (DEFAULT / COMMUNICATIONS / SPEECH)
// running VoiceProcessor (cleanup filters, AEC3 with the render loopback as auxiliary input, NS, AGC2,
// limiter). It also applies the mic volume slider in software, reports AEC/NS/AGC as controllable system
// effects, and publishes live meters for the settings app. Non-real-time callbacks log what the audio
// engine tells us (mode, endpoint, format) to C:\ProgramData\WebrtcVoiceApo\engine.log.
class CEngineApo final : public CBaseAudioProcessingObject, public IAudioSystemEffects3, public IAudioProcessingObjectNotifications,
                        public IApoAcousticEchoCancellation2, public IApoAuxiliaryInputConfiguration, public IApoAuxiliaryInputRT
{
public:
    // The audio engine creates APOs AGGREGATED (pUnkOuter != null), so we implement the
    // standard COM aggregation pattern: an inner non-delegating IUnknown that owns the
    // lifetime, while the IUnknown methods on our interfaces delegate to the outer object.
    explicit CEngineApo(IUnknown* outer) noexcept;
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

    // IAudioSystemEffects3 (Windows 11 "CAPX"; the engine then uses APOInitSystemEffects3 and
    // delivers APO notifications). We expose no Windows-controllable sub-effects.
    STDMETHOD(GetControllableSystemEffectsList)(AUDIO_SYSTEMEFFECT** effects, UINT* numEffects, HANDLE event) override;
    STDMETHOD(SetAudioSystemEffectState)(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state) override;

    // Echo cancellation reference (capture MFX in COMMUNICATIONS mode only):
    // IApoAcousticEchoCancellation(2) marks us as an AEC; the engine then attaches the render loopback
    // as an auxiliary input and feeds it through AcceptInput.
    STDMETHOD(GetDesiredReferenceStreamProperties)(APO_REFERENCE_STREAM_PROPERTIES* pProperties) override;
    STDMETHOD(AddAuxiliaryInput)(DWORD dwInputId, UINT32 cbDataSize, BYTE* pbyData, APO_CONNECTION_DESCRIPTOR* pInputConnection) override;
    STDMETHOD(RemoveAuxiliaryInput)(DWORD dwInputId) override;
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHOD_(void, AcceptInput)(DWORD dwInputId, const APO_CONNECTION_PROPERTY* pInputConnection) override;

    // IAudioProcessingObjectNotifications (Windows 11): endpoint volume -> software slider gain
    STDMETHOD(GetApoNotificationRegistrationInfo)(APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count) override;
    STDMETHOD_(void, HandleNotification)(APO_NOTIFICATION* apoNotification) override;

    static long LiveObjects() noexcept { return s_liveObjects.load(); }

private:
    struct InnerUnknown final : IUnknown
    {
        CEngineApo* self;
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

    ~CEngineApo() override;
    HRESULT NonDelegatingQueryInterface(REFIID riid, void** ppv) noexcept;
    static constexpr const wchar_t* SlotName() noexcept { return L"PACK"; }  // log tag
    void ResetStats() noexcept;

    InnerUnknown m_inner;
    IUnknown* m_outer;  // the aggregating object, or &m_inner when not aggregated
    std::atomic<ULONG> m_refs{1};
    const long m_instanceId;
    GUID m_mode{};

    // Voice processing (EFX and Pack). Prepared in LockForProcess, used in APOProcess.
    VoiceProcessor m_proc;

    // Mic volume slider: the pack replaces the OEM chain, which on laptop codecs applied the slider
    // in software. We apply it ourselves unless the device's volume is in hardware (USB/Bluetooth).
    IMMDevice* m_device = nullptr;       // our endpoint (from Initialize), for notifications
    bool m_hardwareVolumeBus = false;    // USB / Bluetooth endpoint (fallback guess)
    bool m_discoveryOnly = false;        // created only to list effects (Settings); never processes audio
    int m_hardwareVolume = -1;           // QueryHardwareSupport says volume: 1/0, -1 unknown (logged only, unreliable)
    std::atomic<float> m_sliderDb{0.0f};
    std::atomic<bool> m_muted{false};    // endpoint mute (we replace the chain that applied it)
    float SliderGainDb(const Settings& s) const noexcept;

    // Echo reference (render loopback) from the auxiliary input, downmixed to mono.
    bool IsAecCandidate() const noexcept;   // Pack slot + COMMUNICATIONS mode
    ReferenceRing m_ref;
    EchoTimeline m_echoTimeline;        // echo removed per 10 s, written to the log when the stream stops
    std::atomic<bool> m_refActive{false};
    DWORD m_refInputId = 0;
    UINT32 m_refChannels = 0;
    float m_refRate = 0.0f;
    bool m_refFloat = false;
    std::atomic<uint64_t> m_refFrames{0}, m_refCalls{0}, m_refDropped{0}, m_refSilentCalls{0};

    ::telemetry::Slot* m_telemetry = nullptr;  // this device's shared-memory meters for the tray app
    LONG64 m_telemetryKey = 0;
    wchar_t m_endpointName[64] = L"Microphone";
    wchar_t m_endpointKey[128] = L"";
    wchar_t m_deviceGuid[64] = L"";   // endpoint GUID, the key for per-device settings
    int m_devIdx = -1;                // settings snapshot index (-1 = defaults)

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
