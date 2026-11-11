// Opens the default capture endpoint through WASAPI (shared mode) like an app would,
// records a few seconds and reports every HRESULT + the peak level.
// Usage: CaptureTest.exe [default|comms|raw|rawcomms] [seconds] [slider 0..100]
//   default  : AudioCategory_Other, no RAW            (typical recorder)
//   comms    : AudioCategory_Communications, no RAW   (Teams/Discord style)
//   raw      : AUDCLNT_STREAMOPTIONS_RAW              (bypasses SFX/MFX)
//   rawcomms : Communications + RAW                   (Chrome/Edge WebRTC style)
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <initializer_list>
#include "../src/Telemetry.h"

#define CHECK(expr) do { HRESULT _hr = (expr); std::printf("%-48s hr=0x%08lX\n", #expr, static_cast<unsigned long>(_hr)); if (FAILED(_hr)) return 1; } while (0)

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* modeArg = argc > 1 ? argv[1] : L"default";
    if (wcscmp(modeArg, L"meters") == 0)
    {
        // Print the live meters the audio effect publishes (for the tray app), once per 200 ms.
        const int count = argc > 2 ? _wtoi(argv[2]) * 5 : 25;
        for (int i = 0; i < count; ++i)
        {
            telemetry::Data d{};
            if (telemetry::Read(d))
                std::printf("in %6.1f  out %6.1f  ref %6.1f dBFS | echo removed %5.1f dB  agc %+5.1f dB  limiter %4.1f dB | aec %u  %u Hz\n",
                            d.inputDbfs, d.outputDbfs, d.referenceDbfs, d.echoRemovedDb, d.agcGainDb, d.limiterDb, d.aecRunning, d.sampleRate);
            else
                std::printf("(no live data)\n");
            Sleep(200);
        }
        return 0;
    }
    const int seconds = argc > 2 ? _wtoi(argv[2]) : 3;
    const bool comms = wcsstr(modeArg, L"comms") != nullptr;
    const bool raw = wcsstr(modeArg, L"raw") != nullptr;
    std::printf("CaptureTest: category=%s raw=%d seconds=%d\n", comms ? "Communications" : "Other", raw, seconds);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)));
    IMMDevice* dev = nullptr;
    CHECK(en->GetDefaultAudioEndpoint(eCapture, comms ? eCommunications : eConsole, &dev));

    LPWSTR id = nullptr; dev->GetId(&id);
    IPropertyStore* ps = nullptr; PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)))
        std::wprintf(L"Endpoint: %s  %s\n", pv.pwszVal, id);
    PropVariantClear(&pv); if (ps) ps->Release(); CoTaskMemFree(id);

    // Optional 3rd arg: endpoint volume slider 0..100 to set before recording (restored afterwards).
    IAudioEndpointVolume* vol = nullptr;
    float oldVol = -1.0f;
    if (argc > 3 && SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol))))
    {
        vol->GetMasterVolumeLevelScalar(&oldVol);
        const float v = static_cast<float>(_wtoi(argv[3])) / 100.0f;
        vol->SetMasterVolumeLevelScalar(v, nullptr);
        float db = 0; vol->GetMasterVolumeLevel(&db);
        std::printf("Slider set to %.0f%% (%.1f dB), was %.0f%%\n", v * 100, db, oldVol * 100);
        DWORD hw = 0; vol->QueryHardwareSupport(&hw);
        float mn = 0, mx = 0, inc = 0; vol->GetVolumeRange(&mn, &mx, &inc);
        std::printf("QueryHardwareSupport=0x%lX (VOLUME=%d MUTE=%d METER=%d), range %.1f..%.1f dB step %.2f\n", hw,
                    (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0, (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0, (hw & ENDPOINT_HARDWARE_SUPPORT_METER) != 0, mn, mx, inc);
        Sleep(200);
    }

    IAudioClient2* client = nullptr;
    CHECK(dev->Activate(__uuidof(IAudioClient2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)));

    if (wcscmp(modeArg, L"formats") == 0)
    {
        // Which hardware formats does the endpoint accept? (exclusive mode = what the driver supports)
        std::printf("Exclusive-mode support (rate x bits x channels):\n");
        for (DWORD rate : {8000u, 11025u, 16000u, 22050u, 32000u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
        {
            std::printf("  %6lu Hz:", rate);
            for (WORD bits : {16, 24, 32})
                for (WORD chans : {1, 2, 4})
                {
                    WAVEFORMATEXTENSIBLE w{};
                    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    w.Format.nChannels = chans;
                    w.Format.nSamplesPerSec = rate;
                    w.Format.wBitsPerSample = bits == 24 ? 32 : bits;  // 24-bit usually in a 32-bit container
                    w.Format.nBlockAlign = static_cast<WORD>(chans * w.Format.wBitsPerSample / 8);
                    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
                    w.Format.cbSize = 22;
                    w.Samples.wValidBitsPerSample = bits;
                    w.dwChannelMask = chans == 1 ? SPEAKER_FRONT_CENTER : chans == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
                    w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                    HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr);
                    if (hr == S_OK) std::printf(" %ub/%uch", bits, chans);
                }
            std::printf("\n");
        }
        client->Release(); dev->Release(); en->Release();
        return 0;
    }

    AudioClientProperties props{};
    props.cbSize = sizeof(props);
    props.eCategory = comms ? AudioCategory_Communications : AudioCategory_Other;
    props.Options = raw ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
    CHECK(client->SetClientProperties(&props));

    WAVEFORMATEX* mix = nullptr;
    CHECK(client->GetMixFormat(&mix));
    std::printf("Mix format: %lu Hz, %u ch, %u bit, tag 0x%X\n", mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);

    CHECK(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr));
    IAudioCaptureClient* cap = nullptr;
    CHECK(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)));
    CHECK(client->Start());

    // Mix format in shared mode is float32 on all current Windows versions.
    float peak = 0.0f; double sumSq = 0.0; UINT64 samples = 0; UINT64 frames = 0; UINT32 silentPackets = 0;
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    while (GetTickCount64() < end)
    {
        Sleep(10);
        UINT32 n = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&n)) && n > 0)
        {
            BYTE* data = nullptr; UINT32 got = 0; DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &got, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) ++silentPackets;
            else
            {
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < got * mix->nChannels; ++i) { peak = (std::fabs(f[i]) > peak) ? std::fabs(f[i]) : peak; sumSq += static_cast<double>(f[i]) * f[i]; ++samples; }
            }
            frames += got;
            cap->ReleaseBuffer(got);
        }
    }
    client->Stop();
    std::printf("Captured %llu frames (%.2f s), silent packets %u, rms %.1f dBFS, peak %.1f dBFS\n", frames,
                frames / static_cast<double>(mix->nSamplesPerSec), silentPackets, (samples && sumSq > 0) ? 10.0 * std::log10(sumSq / samples) : -999.0,
                peak > 0 ? 20.0 * std::log10(peak) : -999.0);
    if (vol) { vol->SetMasterVolumeLevelScalar(oldVol, nullptr); vol->Release(); }
    CoTaskMemFree(mix); cap->Release(); client->Release(); dev->Release(); en->Release();
    CoUninitialize();
    return 0;
}
