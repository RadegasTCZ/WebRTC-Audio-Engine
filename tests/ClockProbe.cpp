// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// ClockProbe: measures each audio device's real sample clock against QueryPerformanceCounter.
// Opens every active capture endpoint (shared mode, data drained and discarded) and the default render
// endpoint (plays digital silence, mixed in shared mode, inaudible), then samples IAudioClock::GetPosition
// (device position + its QPC timestamp) for N seconds and fits a line. Output: ppm vs QPC per device and
// ppm of each microphone relative to the speakers (what the echo canceller sees).
// Also flags moments where a device lost audio (position fell behind and stayed behind).
// Note: a "LOST 10 ms" followed one second later by "jumped ahead 10 ms" is only a late position update.
// Usage: ClockProbe.exe [seconds=60]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#pragma comment(lib, "ole32.lib")

struct Dev
{
    std::wstring name;
    bool render = false;
    IAudioClient* client = nullptr;
    IAudioClock* clock = nullptr;
    IAudioCaptureClient* cap = nullptr;
    IAudioRenderClient* ren = nullptr;
    UINT32 bufFrames = 0;
    UINT64 freq = 0;
    std::vector<double> t, p;  // seconds (QPC), seconds (device position / freq)
};

static bool Open(IMMDevice* d, bool render, Dev& out)
{
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
    {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out.name = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    out.render = render;
    if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&out.client))) return false;
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(out.client->GetMixFormat(&wf))) return false;
    HRESULT hr = out.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /*200 ms*/, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr)) { std::printf("  %ls: Initialize failed 0x%08lx\n", out.name.c_str(), hr); return false; }
    out.client->GetBufferSize(&out.bufFrames);
    if (FAILED(out.client->GetService(__uuidof(IAudioClock), (void**)&out.clock))) return false;
    out.clock->GetFrequency(&out.freq);
    if (render)
    {
        if (FAILED(out.client->GetService(__uuidof(IAudioRenderClient), (void**)&out.ren))) return false;
        BYTE* b = nullptr;
        if (SUCCEEDED(out.ren->GetBuffer(out.bufFrames, &b))) out.ren->ReleaseBuffer(out.bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    else if (FAILED(out.client->GetService(__uuidof(IAudioCaptureClient), (void**)&out.cap))) return false;
    return SUCCEEDED(out.client->Start());
}

static void Service(Dev& d)
{
    if (d.render)
    {
        UINT32 pad = 0;
        if (SUCCEEDED(d.client->GetCurrentPadding(&pad)) && pad < d.bufFrames)
        {
            BYTE* b = nullptr; const UINT32 n = d.bufFrames - pad;
            if (SUCCEEDED(d.ren->GetBuffer(n, &b))) d.ren->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        return;
    }
    UINT32 n = 0;
    while (SUCCEEDED(d.cap->GetNextPacketSize(&n)) && n)
    {
        BYTE* b; DWORD fl; UINT32 got;
        if (FAILED(d.cap->GetBuffer(&b, &got, &fl, nullptr, nullptr))) break;
        d.cap->ReleaseBuffer(got);
    }
}

// Least-squares slope of p over t, and the RMS residual (jitter) in microseconds.
static void Fit(const Dev& d, double& slope, double& jitterUs)
{
    const size_t n = d.t.size();
    double mt = 0, mp = 0;
    for (size_t i = 0; i < n; ++i) { mt += d.t[i]; mp += d.p[i]; }
    mt /= n; mp /= n;
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < n; ++i) { sxy += (d.t[i] - mt) * (d.p[i] - mp); sxx += (d.t[i] - mt) * (d.t[i] - mt); }
    slope = sxy / sxx;
    double r = 0;
    for (size_t i = 0; i < n; ++i) { const double e = d.p[i] - (mp + slope * (d.t[i] - mt)); r += e * e; }
    jitterUs = std::sqrt(r / n) * 1e6;
}

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 60;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    std::vector<Dev> devs;
    {
        IMMDeviceCollection* rc = nullptr;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rc);
        UINT rn = 0; rc->GetCount(&rn);
        for (UINT i = 0; i < rn; ++i)
        {
            IMMDevice* r = nullptr;
            if (SUCCEEDED(rc->Item(i, &r))) { Dev d; if (Open(r, true, d)) devs.push_back(d); r->Release(); }
        }
    }
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
    UINT cnt = 0; col->GetCount(&cnt);
    for (UINT i = 0; i < cnt; ++i)
    {
        IMMDevice* c = nullptr;
        if (SUCCEEDED(col->Item(i, &c))) { Dev d; if (Open(c, false, d)) devs.push_back(d); c->Release(); }
    }
    if (devs.empty()) { std::printf("no devices\n"); return 1; }
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    std::printf("Measuring %d s (speakers play digital silence)...\n", seconds);
    for (auto& d : devs) std::printf("  %s %ls\n", d.render ? "playback:" : "mic:     ", d.name.c_str());
    const ULONGLONG start = GetTickCount64();
    ULONGLONG nextSample = start + 2000;  // skip start-up
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds + 2) * 1000)
    {
        for (auto& d : devs) Service(d);
        if (GetTickCount64() >= nextSample)
        {
            nextSample += 250;
            for (auto& d : devs)
            {
                UINT64 pos = 0, qpc = 0;  // qpc in 100 ns units
                if (SUCCEEDED(d.clock->GetPosition(&pos, &qpc)) && pos)
                {
                    const double t = qpc * 1e-7, p = static_cast<double>(pos) / d.freq;
                    d.t.push_back(t);
                    d.p.push_back(p);
                    // Lag = time passed minus audio delivered. It bounces by up to one packet (~10 ms) because
                    // positions update per packet, so compare the smallest lag of each 1 s window: a rise of more
                    // than 4 ms between windows = audio the device lost (a dropout); a fall = it jumped ahead.
                    const size_t n = d.t.size();
                    if (n >= 8 && n % 4 == 0)
                    {
                        auto minLag = [&](size_t from) {
                            double m = 1e9;
                            for (size_t i = from; i < from + 4; ++i) m = std::min(m, (d.t[i] - d.t[0]) - (d.p[i] - d.p[0]));
                            return m * 1000.0;
                        };
                        const double step = minLag(n - 4) - minLag(n - 8);
                        if (step > 4.0 || step < -4.0)
                        {
                            SYSTEMTIME st; GetLocalTime(&st);
                            std::printf("  %02d:%02d:%02d  %ls: %s %.1f ms\n", st.wHour, st.wMinute, st.wSecond, d.name.c_str(),
                                        step > 0 ? "LOST" : "jumped ahead", step > 0 ? step : -step);
                        }
                    }
                }
            }
        }
        Sleep(5);
    }
    std::vector<double> slope(devs.size(), 0.0);
    std::printf("\n%-42s %13s %10s\n", "device", "vs PC timer", "jitter");
    for (size_t i = 0; i < devs.size(); ++i)
    {
        double jit = 0;
        if (devs[i].t.size() < 20) { std::printf("%-42ls  (not enough samples)\n", devs[i].name.c_str()); continue; }
        Fit(devs[i], slope[i], jit);
        std::printf("%s %-36ls %+9.1f ppm %7.0f us\n", devs[i].render ? "[out]" : "[mic]", devs[i].name.c_str(), (slope[i] - 1) * 1e6, jit);
    }
    std::printf("\nmic vs playback device (the drift the echo canceller has to follow):\n");
    for (size_t m = 0; m < devs.size(); ++m)
    {
        if (devs[m].render || slope[m] == 0.0) continue;
        for (size_t r = 0; r < devs.size(); ++r)
            if (devs[r].render && slope[r] != 0.0)
                std::printf("  %-34ls vs %-34ls %+8.1f ppm\n", devs[m].name.c_str(), devs[r].name.c_str(), (slope[m] / slope[r] - 1) * 1e6);
    }
    std::printf("\nEcho canceller: fine up to ~100 ppm vs speakers in offline tests, fails around 1000 ppm (0.1 %%).\n");
    for (auto& d : devs) { d.client->Stop(); }
    return 0;
}
