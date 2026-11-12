// Is the speaker path processed (e.g. by Dolby) before the echo reference is tapped?
//
// Plays a sequence of steady tones (one per octave band, fixed level) on the default playback device, and
// at the same time measures:
//   - the WASAPI loopback of that device (what "loopback" recording apps get), and
//   - the echo reference our effect receives (telemetry referenceDbfs; the default microphone is kept open
//     so the effect runs; it must use the WebRTC Audio Engine effect pack).
// If both come back at the played level in every band, the tap is BEFORE any speaker processing. If bands
// come back changed (and change again when the speaker effect is toggled), the tap is AFTER it.
//
// Usage: PathProbe.exe [levelDbfs=-23]
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>
#include "../src/Telemetry.h"

namespace
{
    constexpr double kTone = 1.5, kGap = 0.4, kLead = 1.0;   // seconds
    constexpr double kWinFrom = 0.5, kWinTo = 1.3;             // measuring window inside each tone
    const double kFreqs[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000};
    constexpr int kTones = static_cast<int>(std::size(kFreqs));

    double ToneStart(int k) { return kLead + k * (kTone + kGap); }
    double Db(double meanSq) { return meanSq > 1e-14 ? 10.0 * std::log10(meanSq) : -140.0; }

    bool Fail(const char* what, HRESULT hr)
    {
        std::printf("FAILED: %s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }

    void PrintName(IMMDevice* dev, const char* role)
    {
        IPropertyStore* ps = nullptr;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            std::wprintf(L"%hs: %ls\n", role, pv.pwszVal);
        PropVariantClear(&pv);
        if (ps) ps->Release();
    }
}

int wmain(int argc, wchar_t** argv)
{
    const double levelDb = argc > 1 ? _wtof(argv[1]) : -23.0;
    const double amp = std::sqrt(2.0) * std::pow(10.0, levelDb / 20.0);   // sine RMS = levelDb
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (FAILED(hr)) return Fail("MMDeviceEnumerator", hr), 1;
    IMMDevice *spk = nullptr, *mic = nullptr;
    if (FAILED(hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &spk))) return Fail("default playback device", hr), 1;
    if (FAILED(hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &mic))) return Fail("default microphone", hr), 1;
    PrintName(spk, "Playback  ");
    PrintName(mic, "Microphone");
    IAudioEndpointVolume* vol = nullptr;
    if (SUCCEEDED(spk->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol))))
    {
        float scalar = 0, db = 0;
        BOOL mute = FALSE;
        vol->GetMasterVolumeLevelScalar(&scalar);
        vol->GetMasterVolumeLevel(&db);
        vol->GetMute(&mute);
        std::printf("Playback volume: %.0f%% (%.1f dB)%s\n", scalar * 100, db, mute ? "  MUTED" : "");
        vol->Release();
    }

    // Three shared-mode clients: playback, loopback of the same device, and the microphone.
    IAudioClient *render = nullptr, *loop = nullptr, *cap = nullptr;
    spk->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&render));
    spk->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&loop));
    mic->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap));
    if (!render || !loop || !cap) return Fail("Activate IAudioClient", E_FAIL), 1;
    WAVEFORMATEX *rf = nullptr, *mf = nullptr;
    render->GetMixFormat(&rf);
    cap->GetMixFormat(&mf);
    std::printf("Playback mix format: %lu Hz %u ch (float)\n\n", rf->nSamplesPerSec, rf->nChannels);
    const double rate = rf->nSamplesPerSec;
    const UINT32 ch = rf->nChannels;
    if (FAILED(hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, rf, nullptr))) return Fail("render Initialize", hr), 1;
    if (FAILED(hr = loop->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, rf, nullptr))) return Fail("loopback Initialize", hr), 1;
    if (FAILED(hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mf, nullptr))) return Fail("microphone Initialize", hr), 1;
    IAudioRenderClient* rc = nullptr;
    IAudioCaptureClient *lc = nullptr, *mc = nullptr;
    render->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc));
    loop->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&lc));
    cap->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&mc));
    UINT32 renderBuf = 0;
    render->GetBufferSize(&renderBuf);

    const double total = ToneStart(kTones) + 0.5;
    const UINT64 totalFrames = static_cast<UINT64>(total * rate);
    // Per tone: loopback sum of squares (by loopback frame index) and telemetry samples (by time).
    std::vector<double> loopSq(kTones, 0.0), loopN(kTones, 0.0);
    std::vector<std::vector<float>> refDb(kTones), inDb(kTones);

    cap->Start();
    loop->Start();
    UINT64 played = 0, looped = 0;
    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    // Prefill, then start playback.
    auto fill = [&] {
        UINT32 pad = 0;
        render->GetCurrentPadding(&pad);
        UINT32 n = renderBuf - pad;
        if (n == 0) return;
        BYTE* data = nullptr;
        if (FAILED(rc->GetBuffer(n, &data))) return;
        float* out = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < n; ++i, ++played)
        {
            const double t = played / rate;
            float v = 0.0f;
            for (int k = 0; k < kTones; ++k)
                if (t >= ToneStart(k) && t < ToneStart(k) + kTone)
                {
                    const double tt = t - ToneStart(k);
                    const double fade = std::min({1.0, tt / 0.02, (kTone - tt) / 0.02});   // 20 ms fades
                    v = static_cast<float>(amp * fade * std::sin(2.0 * 3.14159265358979 * kFreqs[k] * tt));
                }
            for (UINT32 c = 0; c < ch; ++c) out[i * ch + c] = v;
        }
        rc->ReleaseBuffer(n, 0);
    };
    fill();
    render->Start();
    QueryPerformanceCounter(&t0);

    std::printf("Playing %d tones at %.1f dBFS RMS (%.0f s)...\n", kTones, levelDb, total);
    for (;;)
    {
        Sleep(5);
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - t0.QuadPart) / f.QuadPart;
        if (t > total + 0.3) break;
        if (played < totalFrames) fill();

        // Loopback: frames counted from when playback started, so frame index ~ playback time (+ a few ms).
        UINT32 n = 0;
        while (SUCCEEDED(lc->GetNextPacketSize(&n)) && n > 0)
        {
            BYTE* data = nullptr;
            UINT32 got = 0;
            DWORD flags = 0;
            if (FAILED(lc->GetBuffer(&data, &got, &flags, nullptr, nullptr))) break;
            const float* s = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < got; ++i, ++looped)
            {
                const double lt = looped / rate;
                for (int k = 0; k < kTones; ++k)
                    if (lt >= ToneStart(k) + kWinFrom && lt < ToneStart(k) + kWinTo)
                    {
                        const double x = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : s[i * ch];
                        loopSq[k] += x * x;
                        loopN[k] += 1;
                    }
            }
            lc->ReleaseBuffer(got);
        }
        // Microphone: just keep it flowing (the effect only runs while someone records).
        while (SUCCEEDED(mc->GetNextPacketSize(&n)) && n > 0)
        {
            BYTE* data = nullptr;
            UINT32 got = 0;
            DWORD flags = 0;
            if (FAILED(mc->GetBuffer(&data, &got, &flags, nullptr, nullptr))) break;
            mc->ReleaseBuffer(got);
        }
        // Echo reference level as our effect sees it (100 ms meters).
        telemetry::Device devs[telemetry::kMaxSlots];
        if (telemetry::ReadAll(devs, 300) > 0)
            for (int k = 0; k < kTones; ++k)
                if (t >= ToneStart(k) + kWinFrom + 0.1 && t < ToneStart(k) + kWinTo)
                {
                    refDb[k].push_back(devs[0].values.referenceDbfs);
                    inDb[k].push_back(devs[0].values.inputDbfs);
                }
    }
    render->Stop();
    loop->Stop();
    cap->Stop();

    auto median = [](std::vector<float> v) {
        if (v.empty()) return -999.0;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return static_cast<double>(v[v.size() / 2]);
    };
    std::printf("\n  band      played   loopback  (diff)   our echo ref  (diff)   mic hears\n");
    for (int k = 0; k < kTones; ++k)
    {
        const double lb = loopN[k] > 0 ? Db(loopSq[k] / loopN[k]) : -999.0;
        const double rf2 = median(refDb[k]), mi = median(inDb[k]);
        std::printf("  %5.0f Hz  %6.1f   %7.1f  (%+5.1f)", kFreqs[k], levelDb, lb, lb - levelDb);
        if (rf2 > -998) std::printf("   %7.1f      (%+5.1f)   %7.1f\n", rf2, rf2 - levelDb, mi);
        else std::printf("   (no data: is the mic on WebRTC Audio Engine?)\n");
    }
    std::printf("\nAll diffs near 0 dB = untouched path (tap before any speaker processing).\n"
                "Diffs that vary by band, and change when the speaker effect is toggled = processed path.\n");

    CoTaskMemFree(rf);
    CoTaskMemFree(mf);
    rc->Release(); lc->Release(); mc->Release();
    render->Release(); loop->Release(); cap->Release();
    spk->Release(); mic->Release(); en->Release();
    CoUninitialize();
    return 0;
}
