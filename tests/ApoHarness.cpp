// Offline smoke test: drives the APO like audiodg does, inside a normal process,
// so crashes show up here instead of killing system audio.
// Usage: ApoHarness.exe <path to WebrtcVoiceApo.dll>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include "../src/Guids.h"

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

static int Fail(const char* what, HRESULT hr)
{
    std::printf("FAIL: %s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
    return 1;
}

// Minimal aggregating outer object, like the audio engine's wrapper around an APO.
struct FakeOuter final : IUnknown
{
    IUnknown* inner = nullptr;
    ULONG refs = 1;
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; }
        return inner->QueryInterface(riid, ppv);
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override { return --refs; }  // lives on the stack
};

static int RunOne(GetClassObjectFn getClassObject, REFCLSID clsid, const char* name, UINT32 period, bool aggregated)
{
    std::printf("--- %s, period %u frames, %s\n", name, period, aggregated ? "AGGREGATED (like audiodg)" : "standalone");
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(clsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return Fail("DllGetClassObject", hr);

    FakeOuter outer;
    IAudioProcessingObject* apo = nullptr;
    if (aggregated)
    {
        void* bad = nullptr;
        if (factory->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &bad) != CLASS_E_NOAGGREGATION)
            return Fail("aggregated create with non-IUnknown IID should be refused", E_FAIL);
        hr = factory->CreateInstance(&outer, __uuidof(IUnknown), reinterpret_cast<void**>(&outer.inner));
        if (SUCCEEDED(hr)) hr = outer.QueryInterface(__uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo));
    }
    else
    {
        hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo));
    }
    factory->Release();
    if (FAILED(hr)) return Fail("CreateInstance", hr);

    APOInitSystemEffects2 init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = clsid;
    init.AudioProcessingMode = {0x98951333, 0xB9CD, 0x48B1, {0xA0, 0xA3, 0xFF, 0x40, 0x68, 0x2D, 0x73, 0xF7}}; // COMMUNICATIONS
    hr = apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
    if (FAILED(hr)) return Fail("Initialize", hr);

    IAudioProcessingObjectConfiguration* cfg = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioSystemEffects2* fx2 = nullptr;
    apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), reinterpret_cast<void**>(&cfg));
    apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&rt));
    apo->QueryInterface(__uuidof(IAudioSystemEffects2), reinterpret_cast<void**>(&fx2));
    if (!cfg || !rt || !fx2) return Fail("QueryInterface (config/RT/fx2)", E_NOINTERFACE);

    // 48 kHz stereo float, the usual engine format for capture APOs.
    const UINT32 channels = 2;
    UNCOMPRESSEDAUDIOFORMAT fmt{};
    fmt.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fmt.dwSamplesPerFrame = channels;
    fmt.dwBytesPerSampleContainer = 4;
    fmt.dwValidBitsPerSample = 32;
    fmt.fFramesPerSecond = 48000.0f;
    fmt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    IAudioMediaType* mediaType = nullptr;
    hr = CreateAudioMediaTypeFromUncompressedAudioFormat(&fmt, &mediaType);
    if (FAILED(hr)) return Fail("CreateAudioMediaTypeFromUncompressedAudioFormat", hr);

    IAudioMediaType* supported = nullptr;
    hr = apo->IsInputFormatSupported(nullptr, mediaType, &supported);
    std::printf("IsInputFormatSupported -> 0x%08lX\n", static_cast<unsigned long>(hr));
    if (supported) supported->Release();

    std::vector<float> inBuf(static_cast<size_t>(period) * channels), outBuf(inBuf.size(), 0.0f);
    APO_CONNECTION_DESCRIPTOR inDesc{APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(inBuf.data()), period, mediaType, APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR outDesc{APO_CONNECTION_BUFFER_TYPE_EXTERNAL, reinterpret_cast<UINT_PTR>(outBuf.data()), period, mediaType, APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR* inDescs[] = {&inDesc};
    APO_CONNECTION_DESCRIPTOR* outDescs[] = {&outDesc};
    hr = cfg->LockForProcess(1, inDescs, 1, outDescs);
    if (FAILED(hr)) return Fail("LockForProcess", hr);

    APO_CONNECTION_PROPERTY inProp{reinterpret_cast<UINT_PTR>(inBuf.data()), period, BUFFER_VALID, APO_CONNECTION_PROPERTY_SIGNATURE};
    APO_CONNECTION_PROPERTY outProp{reinterpret_cast<UINT_PTR>(outBuf.data()), 0, BUFFER_INVALID, APO_CONNECTION_PROPERTY_SIGNATURE};
    APO_CONNECTION_PROPERTY* inProps[] = {&inProp};
    APO_CONNECTION_PROPERTY* outProps[] = {&outProp};

    // 1 second of a 440 Hz sine at -6 dBFS, then one silent buffer.
    double phase = 0.0;
    const UINT32 blocks = 48000 / period;
    for (UINT32 b = 0; b < blocks; ++b)
    {
        for (UINT32 i = 0; i < period; ++i)
        {
            const float s = static_cast<float>(0.5 * std::sin(phase));
            phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
            for (UINT32 c = 0; c < channels; ++c) inBuf[static_cast<size_t>(i) * channels + c] = s;
        }
        rt->APOProcess(1, inProps, 1, outProps);
        if (outProp.u32ValidFrameCount != period || outProp.u32BufferFlags != BUFFER_VALID)
            return Fail("frame count / flags wrong", E_FAIL);
        for (float v : outBuf)
            if (!std::isfinite(v)) return Fail("non-finite output", E_FAIL);
    }
    inProp.u32BufferFlags = BUFFER_SILENT;
    rt->APOProcess(1, inProps, 1, outProps);
    // With processing active the output stays VALID (the latency FIFO keeps running).
    if (outProp.u32BufferFlags != BUFFER_SILENT && outProp.u32BufferFlags != BUFFER_VALID)
        return Fail("silent buffer handling", E_FAIL);

    // In-place case (same buffer for input and output).
    inProp.u32BufferFlags = BUFFER_VALID;
    outProp.pBuffer = inProp.pBuffer;
    rt->APOProcess(1, inProps, 1, outProps);

    LPGUID ids = nullptr; UINT count = 99;
    hr = fx2->GetEffectsList(&ids, &count, nullptr);
    std::printf("GetEffectsList -> 0x%08lX count=%u\n", static_cast<unsigned long>(hr), count);

    hr = cfg->UnlockForProcess();
    if (FAILED(hr)) return Fail("UnlockForProcess", hr);

    fx2->Release(); rt->Release(); cfg->Release(); mediaType->Release();
    ULONG left = apo->Release();
    if (aggregated)
    {
        if (outer.refs != 1) return Fail("outer refcount not balanced", E_FAIL);
        left = outer.inner->Release();  // the engine releases the inner object last
    }
    std::printf("OK: %u blocks %s, final refcount %lu\n", blocks + 2, "processed, all finite", left);
    return left == 0 ? 0 : Fail("refcount leak", E_FAIL);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { std::printf("usage: ApoHarness <WebrtcVoiceApo.dll>\n"); return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return Fail("LoadLibrary", HRESULT_FROM_WIN32(GetLastError()));
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject) return Fail("GetProcAddress DllGetClassObject", E_FAIL);

    int rc = 0;
    for (bool aggregated : {true, false})
        for (UINT32 period : {480u, 441u, 128u})
        {
            rc |= RunOne(getClassObject, CLSID_WebrtcAudioEnginePack, "PACK", period, aggregated);
        }
    using CanUnloadFn = HRESULT(STDAPICALLTYPE*)();
    auto canUnload = reinterpret_cast<CanUnloadFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    std::printf("DllCanUnloadNow -> %s\n", canUnload && canUnload() == S_OK ? "S_OK" : "S_FALSE (objects alive!)");
    std::printf(rc == 0 ? "ALL PASSED\n" : "SOME TESTS FAILED\n");
    FreeLibrary(dll);
    CoUninitialize();
    return rc;
}
