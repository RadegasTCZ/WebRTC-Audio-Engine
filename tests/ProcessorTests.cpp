// Offline tests for VoiceProcessor (HPF + WebRTC NS chain), no audio engine involved.
#include "../src/VoiceProcessor.h"
#include "../src/EchoTimeline.h"
#include "rtc_base/cpu_info.h"
#include <string>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

static int g_failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++g_failures; } } while (0)

constexpr int kCh = 2;
constexpr int kLat = VoiceProcessor::kTotalLatency;

static std::vector<float> Noise(int frames, float db, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, std::pow(10.0f, db / 20.0f));
    std::vector<float> s(static_cast<size_t>(kCh) * frames);
    for (auto& v : s) v = n(rng);
    return s;
}

static std::vector<float> Sine(int frames, float hz, float amp)
{
    std::vector<float> s(static_cast<size_t>(kCh) * frames);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < kCh; ++c)
            s[static_cast<size_t>(f) * kCh + c] = amp * std::sin(2.0f * 3.14159265f * hz * f / 48000.0f);
    return s;
}

static void AddBursts(std::vector<float>& s)  // 300 Hz tone gated every 250 ms (speech-like)
{
    const int frames = static_cast<int>(s.size() / kCh);
    for (int f = 0; f < frames; ++f)
        if ((f / 12000) % 2 == 0)
            for (int c = 0; c < kCh; ++c)
                s[static_cast<size_t>(f) * kCh + c] += 0.1f * std::sin(2.0f * 3.14159265f * 300.0f * f / 48000.0f);
}

// Runs the processor; `perBlock` may change the settings before each engine period.
static std::vector<float> Run(const std::vector<float>& in, const std::vector<int>& periods, Settings s,
                              bool inPlace = false, std::function<void(int, Settings&)> perPeriod = nullptr)
{
    VoiceProcessor p;
    p.Prepare(kCh, 48000, s);
    std::vector<float> out = inPlace ? in : std::vector<float>(in.size());
    const int frames = static_cast<int>(in.size() / kCh);
    int pos = 0, k = 0;
    while (pos < frames)
    {
        if (perPeriod) perPeriod(k, s);
        const int n = std::min(periods[static_cast<size_t>(k++) % periods.size()], frames - pos);
        const size_t off = static_cast<size_t>(pos) * kCh;
        p.Process(inPlace ? out.data() + off : in.data() + off, out.data() + off, n, s);
        pos += n;
    }
    return out;
}

static double RmsDb(const std::vector<float>& s, size_t from, size_t to)
{
    double sum = 0;
    for (size_t i = from; i < to; ++i) sum += static_cast<double>(s[i]) * s[i];
    return 10.0 * std::log10(sum / static_cast<double>(to - from) + 1e-30);
}

static bool AllFinite(const std::vector<float>& s)
{
    for (float v : s) if (!std::isfinite(v)) return false;
    return true;
}

int main()
{
    const int frames = 48000 * 4;
    const Settings def{};
    auto sig = Noise(frames, -40.0f, 1);
    AddBursts(sig);

    std::printf("[1] output independent of engine period size\n");
    auto ref = Run(sig, {480}, def);
    for (auto periods : std::vector<std::vector<int>>{{128}, {441}, {1000}, {7, 480, 333, 1024, 1}})
    {
        auto o = Run(sig, periods, def);
        EXPECT(std::memcmp(o.data(), ref.data(), o.size() * sizeof(float)) == 0, "period pattern starting %d differs", periods[0]);
    }

    std::printf("[2] in-place == separate buffers\n");
    auto inplace = Run(sig, {441}, def, true);
    EXPECT(std::memcmp(inplace.data(), ref.data(), ref.size() * sizeof(float)) == 0, "in-place result differs");

    std::printf("[3] bypass = input delayed by exactly %d samples (constant latency)\n", kLat);
    Settings byp = def; byp.bypass = 1;
    auto bo = Run(sig, {441}, byp);
    bool exact = true;
    for (size_t i = 0; i < bo.size(); ++i)
    {
        const float expected = i < static_cast<size_t>(kLat) * kCh ? 0.0f : sig[i - static_cast<size_t>(kLat) * kCh];
        exact &= (bo[i] == expected);
    }
    EXPECT(exact, "bypass is not a pure delay");

    std::printf("[4] steady noise is reduced\n");
    auto noise = Noise(frames, -40.0f, 2);
    auto no = Run(noise, {480}, def);
    const size_t half = noise.size() / 2;
    const double inDb = RmsDb(noise, half, noise.size()), outDb = RmsDb(no, half, noise.size());
    std::printf("    noise in %.1f dBFS -> out %.1f dBFS (%.1f dB reduction)\n", inDb, outDb, inDb - outDb);
    EXPECT(inDb - outDb > 6.0, "expected > 6 dB reduction");

    std::printf("[5] speech-like bursts survive (within 6 dB)\n");
    const size_t b0 = static_cast<size_t>(2.0 * 48000 + kLat + 2400) * kCh, b1 = static_cast<size_t>(2.25 * 48000 + kLat - 2400) * kCh;
    const double sigIn = RmsDb(sig, b0 - static_cast<size_t>(kLat) * kCh, b1 - static_cast<size_t>(kLat) * kCh), sigOut = RmsDb(ref, b0, b1);
    std::printf("    burst in %.1f dBFS -> out %.1f dBFS\n", sigIn, sigOut);
    EXPECT(std::fabs(sigIn - sigOut) < 6.0, "speech-like burst changed by more than 6 dB");

    std::printf("[6] high-pass: 30 Hz cut, 1 kHz passes (NS off)\n");
    Settings hpfOnly = def; hpfOnly.nsEnabled = 0;
    for (auto [hz, minCut, maxCut] : {std::tuple{30.0f, 10.0, 99.0}, std::tuple{1000.0f, -0.5, 0.5}})
    {
        auto s = Sine(frames, hz, 0.25f);
        auto o = Run(s, {480}, hpfOnly);
        const double cut = RmsDb(s, half, s.size()) - RmsDb(o, half, s.size());
        std::printf("    %.0f Hz: %.1f dB attenuation\n", hz, cut);
        EXPECT(cut > minCut && cut < maxCut, "%.0f Hz attenuation %.1f dB out of range", hz, cut);
    }

    std::printf("[6b] hiss filter (10 kHz, 6 dB/oct): 1 kHz passes, -3 dB at 10 kHz, 18 kHz reduced (NS off)\n");
    {
        Settings lpOnly = def; lpOnly.nsEnabled = 0; lpOnly.hpfEnabled = 0; lpOnly.lpfEnabled = 1; lpOnly.limiterEnabled = 0;
        for (auto [hz, lo, hi] : {std::tuple{1000.0f, -0.2, 0.3}, std::tuple{10000.0f, 2.0, 4.0}, std::tuple{18000.0f, 5.0, 99.0}})
        {
            auto s = Sine(frames, hz, 0.25f);
            auto o = Run(s, {480}, lpOnly);
            const double cut = RmsDb(s, half, s.size()) - RmsDb(o, half, s.size());
            std::printf("    %5.0f Hz: %.1f dB attenuation\n", hz, cut);
            EXPECT(cut > lo && cut < hi, "%.0f Hz attenuation %.1f dB out of range", hz, cut);
        }
    }

    std::printf("[7] live switching (every stage, every level, bypass) stays finite and click-free\n");
    auto tone = Sine(frames, 1000.0f, 0.25f);
    auto sw = Run(tone, {441}, def, false, [](int k, Settings& s) {
        s.hpfEnabled = (k / 7) % 2;
        s.lpfEnabled = (k / 5) % 2;
        s.nsEnabled = (k / 11) % 2;
        s.nsLevel = (k / 13) % 4;
        s.bypass = (k / 17) % 2;
        s.hpfHz = 60 + 20 * ((k / 19) % 3);
    });
    EXPECT(AllFinite(sw), "non-finite output while switching");
    float maxStep = 0;
    for (size_t i = static_cast<size_t>(kLat + 4800) * kCh + kCh; i < sw.size(); i += kCh)
        maxStep = std::max(maxStep, std::fabs(sw[i] - sw[i - kCh]));
    const float sineStep = 0.25f * 2.0f * 3.14159265f * 1000.0f / 48000.0f;  // max slope of the clean tone
    std::printf("    largest sample-to-sample step %.4f (clean tone max %.4f)\n", maxStep, sineStep);
    EXPECT(maxStep < 3.0f * sineStep, "switching produced a click");

    std::printf("[8] NaN / Inf input never produces non-finite output\n");
    auto bad = sig;
    for (size_t i = 1000; i < 1100; ++i) bad[i] = (i % 2) ? NAN : INFINITY;
    EXPECT(AllFinite(Run(bad, {480}, def)), "non-finite output");

    std::printf("[8b] input gain (slider): +12 dB and -6 dB applied, changes ramp without clicks (NS/HPF off)\n");
    {
        Settings plain = def; plain.nsEnabled = 0; plain.hpfEnabled = 0;
        auto t = Sine(frames, 1000.0f, 0.05f);
        for (float g : {12.0f, -6.0f})
        {
            VoiceProcessor vp; vp.Prepare(kCh, 48000, plain);
            std::vector<float> y(t.size());
            vp.Process(t.data(), y.data(), frames, plain, g);
            const double got = RmsDb(y, half, y.size()) - RmsDb(t, half, t.size());
            std::printf("    requested %+.0f dB -> measured %+.2f dB\n", g, got);
            EXPECT(std::fabs(got - g) < 0.1, "gain %.0f dB measured %.2f", g, got);
        }
        // Slider moves every 10 periods between -17 and +30 dB: must stay click-free.
        VoiceProcessor vp; vp.Prepare(kCh, 48000, plain);
        std::vector<float> y(t.size());
        int pos = 0, k = 0;
        while (pos < frames)
        {
            const int n = std::min(441, frames - pos);
            const float g = ((k++ / 10) % 2) ? 30.0f : -17.0f;
            vp.Process(t.data() + static_cast<size_t>(pos) * kCh, y.data() + static_cast<size_t>(pos) * kCh, n, plain, g);
            pos += n;
        }
        float step = 0;
        for (size_t i = static_cast<size_t>(kLat + 4800) * kCh + kCh; i < y.size(); i += kCh)
            step = std::max(step, std::fabs(y[i] - y[i - kCh]));
        const float maxSlope = 0.05f * 31.6f * 2.0f * 3.14159265f * 1000.0f / 48000.0f;  // tone at +30 dB
        std::printf("    slider jumps -17 <-> +30 dB: largest step %.4f (tone at +30 dB max %.4f)\n", step, maxSlope);
        EXPECT(step < 1.5f * maxSlope && AllFinite(y), "slider change clicks");
    }

    std::printf("[8c] limiter: a +6 dBFS tone is held at or below -1 dBFS; off = untouched\n");
    {
        Settings lim = def; lim.nsEnabled = 0; lim.hpfEnabled = 0;
        auto loud = Sine(frames, 1000.0f, 2.0f);
        VoiceProcessor vp; vp.Prepare(kCh, 48000, lim);
        std::vector<float> y(loud.size());
        vp.Process(loud.data(), y.data(), frames, lim);
        float peak = 0;
        for (size_t i = half; i < y.size(); ++i) peak = std::max(peak, std::fabs(y[i]));
        std::printf("    peak %.3f (ceiling 0.891), reduction %.1f dB\n", peak, vp.LimiterReductionDb());
        EXPECT(peak <= 0.8913f && AllFinite(y), "limiter let %.3f through", peak);
        Settings off = lim; off.limiterEnabled = 0;
        VoiceProcessor v2; v2.Prepare(kCh, 48000, off);
        v2.Process(loud.data(), y.data(), frames, off);
        peak = 0;
        for (size_t i = half; i < y.size(); ++i) peak = std::max(peak, std::fabs(y[i]));
        EXPECT(peak > 1.9f, "limiter off still limits (peak %.3f)", peak);
    }

    std::printf("[8d] AGC2: runs finite; parameter changes swap stages live; slider ignored while AGC is on\n");
    {
        Settings agc = def; agc.agcEnabled = 1;
        auto quiet = sig;  // noise + speech-like bursts
        for (auto& v : quiet) v *= 0.1f;
        VoiceProcessor vp; vp.Prepare(kCh, 48000, agc);
        std::vector<float> y(quiet.size());
        int pos = 0, k = 0;
        while (pos < frames)
        {
            const int n = std::min(480, frames - pos);
            if (k == 100) { Settings s2 = agc; s2.agcMaxGainDb = 50; s2.agcHeadroomDb = 3; vp.RequestStageParams(s2); }
            if (k == 200) { Settings s3 = agc; s3.agcSpeedDbPerSec = 10; vp.RequestStageParams(s3); }
            vp.Process(quiet.data() + static_cast<size_t>(pos) * kCh, y.data() + static_cast<size_t>(pos) * kCh, n, agc, 30.0f);
            pos += n; ++k;
        }
        std::printf("    AGC gain at end %.1f dB, output rms %.1f dBFS (input %.1f dBFS)\n", vp.AgcGainDb(),
                    RmsDb(y, half, y.size()), RmsDb(quiet, half, quiet.size()));
        EXPECT(AllFinite(y), "AGC produced non-finite output");
        // With +30 dB slider ignored, the output must not be ~30 dB louder than AGC alone would make it.
        EXPECT(RmsDb(y, half, y.size()) < RmsDb(quiet, half, quiet.size()) + 52.0, "slider gain leaked through with AGC on");
    }

    std::printf("[9] unsupported formats stay inactive\n");
    VoiceProcessor p;
    EXPECT(!p.Prepare(2, 4000, def) && !p.IsActive(), "4 kHz should be refused");
    EXPECT(!p.Prepare(9, 48000, def), "9 channels should be refused");

    std::printf("[10] other sample rates (resampled via 48 kHz): no underruns, steady latency, tone intact, noise reduced\n");
    for (int rate : {8000, 16000, 22050, 32000, 44100, 88200, 96000, 192000})
    {
        const int n = rate * 4;
        const float toneHz = std::min(1000.0f, rate / 8.0f);
        auto run = [&](const std::vector<float>& x, Settings st, const std::vector<int>& periods, uint64_t& underruns, int& latency) {
            VoiceProcessor vp;
            vp.Prepare(kCh, rate, st, 2048);
            latency = vp.LatencyFrames();
            std::vector<float> y(x.size());
            int pos = 0, k = 0;
            while (pos < n)
            {
                const int len = std::min(periods[static_cast<size_t>(k++) % periods.size()], n - pos);
                vp.Process(x.data() + static_cast<size_t>(pos) * kCh, y.data() + static_cast<size_t>(pos) * kCh, len, st);
                pos += len;
            }
            underruns = vp.Underruns();
            return y;
        };
        // Tone through bypass: must come out as a clean delayed copy (only resampler filtering).
        std::vector<float> tone(static_cast<size_t>(kCh) * n);
        for (int f = 0; f < n; ++f)
            for (int c = 0; c < kCh; ++c)
                tone[static_cast<size_t>(f) * kCh + c] = 0.25f * std::sin(2.0f * 3.14159265f * toneHz * f / rate);
        uint64_t ur = 0; int lat = 0;
        Settings b = def; b.bypass = 1;
        auto y = run(tone, b, {rate / 100, 7, rate / 100 + 13, 1024, 333}, ur, lat);
        // Distortion: least-squares fit of a sine at the tone frequency (any phase = any fractional
        // delay); everything left over is distortion, glitches or dropouts.
        const double w = 2.0 * 3.14159265358979 * toneHz / rate;
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        for (int f = n / 2; f < n; ++f)
        {
            const double s = std::sin(w * f), c = std::cos(w * f), v = y[static_cast<size_t>(f) * kCh];
            ss += s * s; sc += s * c; cc += c * c; ys += v * s; yc += v * c;
        }
        const double det = ss * cc - sc * sc, A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det;
        double fitPow = 0, resPow = 0;
        for (int f = n / 2; f < n; ++f)
        {
            const double fit = A * std::sin(w * f) + B * std::cos(w * f), v = y[static_cast<size_t>(f) * kCh];
            fitPow += fit * fit; resPow += (v - fit) * (v - fit);
        }
        const double best = -10.0 * std::log10(fitPow / (resPow + 1e-30));
        // Delay from the fitted phase, unwrapped to the period nearest the reported latency.
        const double period = rate / toneHz;
        double d = std::fmod(-std::atan2(B, A) / w, period);
        if (d < 0) d += period;
        d += std::round((lat - d) / period) * period;
        const int bestD = static_cast<int>(std::lround(d));
        // Steady noise through the full chain must be reduced.
        auto nz = Noise(n, -40.0f, 7);
        uint64_t ur2 = 0; int lat2 = 0;
        auto ny = run(nz, def, {rate / 100}, ur2, lat2);
        const double red = RmsDb(nz, nz.size() / 2, nz.size()) - RmsDb(ny, ny.size() / 2, ny.size());
        std::printf("    %6d Hz: latency %4d frames (best match %4d), tone SNR %5.1f dB, noise -%4.1f dB, underruns %llu/%llu\n",
                    rate, lat, bestD, -best, red, ur, ur2);
        EXPECT(ur == 0 && ur2 == 0, "%d Hz: underruns", rate);
        EXPECT(-best > 40.0, "%d Hz: tone SNR %.1f dB too low (resampler path broken?)", rate, -best);
        EXPECT(std::abs(bestD - lat) <= 3, "%d Hz: reported latency %d vs measured %d", rate, lat, bestD);
        EXPECT(red > 3.0, "%d Hz: noise not reduced (%.1f dB)", rate, red);
    }

    // ----------------------------------------------------------------- AEC3
    // Feeds the processor block by block, pushing the matching farEnd-end block into the reference ring
    // first (as AcceptInput does before APOProcess in the engine).
    auto runAec = [&](const std::vector<float>& mic, const std::vector<float>& farEnd, Settings st, bool withRef,
                      std::function<void(int, Settings&)> perBlock = nullptr) {
        ReferenceRing ring;
        ring.Init(48000);
        VoiceProcessor vp;
        vp.SetReference(withRef ? &ring : nullptr);
        vp.Prepare(kCh, 48000, st);
        std::vector<float> y(mic.size());
        const int n = static_cast<int>(mic.size() / kCh);
        for (int pos = 0, k = 0; pos < n; pos += 480, ++k)
        {
            if (perBlock) perBlock(k, st);
            ring.Write(farEnd.data() + pos, 480);
            vp.Process(mic.data() + static_cast<size_t>(pos) * kCh, y.data() + static_cast<size_t>(pos) * kCh, 480, st);
        }
        return y;
    };

    std::printf("[11] latency with and without AEC3 matches the reported %d samples (NS/HPF/AGC/limiter off)\n", kLat);
    {
        Settings plain = def; plain.nsEnabled = 0; plain.hpfEnabled = 0; plain.lpfEnabled = 0; plain.limiterEnabled = 0;
        const int n = 48000;
        std::vector<float> mic(static_cast<size_t>(kCh) * n, 0.0f), farEnd(n, 0.0f);
        for (int f = 9600; f < n; f += 9600) for (int c = 0; c < kCh; ++c) mic[static_cast<size_t>(f) * kCh + c] = 0.5f;
        for (bool withRef : {false, true})
        {
            auto y = runAec(mic, farEnd, plain, withRef);
            int peakAt = -1; float peak = 0;
            for (int f = 19200; f < 28800; ++f)
                if (std::fabs(y[static_cast<size_t>(f) * kCh]) > peak) { peak = std::fabs(y[static_cast<size_t>(f) * kCh]); peakAt = f; }
            std::printf("    %s: impulse at 19200 -> output peak at %d (delay %d)\n", withRef ? "AEC3 path " : "stand-in  ", peakAt, peakAt - 19200);
            EXPECT(peakAt - 19200 == kLat, "%s delay %d != reported %d", withRef ? "AEC3" : "stand-in", peakAt - 19200, kLat);
        }
    }

    std::printf("[12] echo cancellation: farEnd-end echo removed, near-end talker kept\n");
    {
        Settings st = def; st.nsEnabled = 0; st.hpfEnabled = 0; st.limiterEnabled = 0;
        const int n = 48000 * 12;
        // Far end: speech-like noise bursts (on 300 ms / off 200 ms), 10 s.
        std::mt19937 rng(11);
        std::normal_distribution<float> g(0.0f, 0.1f);
        std::vector<float> farEnd(n, 0.0f);
        for (int f = 0; f < n; ++f) if ((f / 24000) % 5 < 3 && f < 48000 * 10) farEnd[f] = g(rng);
        // Echo path: 30 ms delay, -6 dB, simple low-pass (a speaker+room stand-in).
        std::vector<float> echo(n, 0.0f);
        float lp = 0.0f;
        for (int f = 1440; f < n; ++f) { lp += 0.3f * (farEnd[f - 1440] - lp); echo[f] = 0.5f * lp; }
        // Near end: a quieter 440 Hz talker during the last 2 s only (no farEnd-end then).
        std::vector<float> mic(static_cast<size_t>(kCh) * n);
        for (int f = 0; f < n; ++f)
        {
            const float nearSig = f >= 48000 * 10 ? 0.05f * std::sin(2.0f * 3.14159265f * 440.0f * f / 48000.0f) : 0.0f;
            for (int c = 0; c < kCh; ++c) mic[static_cast<size_t>(f) * kCh + c] = echo[f] + nearSig;
        }
        auto off = st; off.aecEnabled = 0;
        auto yOn = runAec(mic, farEnd, st, true);
        auto yOff = runAec(mic, farEnd, off, true);
        // Echo-only window after convergence: 6..10 s (shifted by the latency).
        const size_t e0 = static_cast<size_t>(48000 * 6 + kLat) * kCh, e1 = static_cast<size_t>(48000 * 10 + kLat) * kCh;
        const double echoOff = RmsDb(yOff, e0, e1), echoOn = RmsDb(yOn, e0, e1);
        // Near-end window: 10.5..12 s.
        const size_t n0 = static_cast<size_t>(48000 * 10.5 + kLat) * kCh, n1 = static_cast<size_t>(n) * kCh;
        const double nearOff = RmsDb(yOff, n0, n1), nearOn = RmsDb(yOn, n0, n1);
        std::printf("    echo  : AEC off %.1f dBFS -> AEC on %.1f dBFS (%.1f dB removed)\n", echoOff, echoOn, echoOff - echoOn);
        std::printf("    talker: AEC off %.1f dBFS -> AEC on %.1f dBFS\n", nearOff, nearOn);
        EXPECT(echoOff - echoOn > 20.0, "only %.1f dB echo removal", echoOff - echoOn);
        EXPECT(std::fabs(nearOff - nearOn) < 3.0, "near-end talker changed by %.1f dB", nearOn - nearOff);
        EXPECT(AllFinite(yOn), "non-finite AEC output");
    }

    std::printf("[13] stuck / bursty / drifting devices: the echo reference stays bounded and AEC recovers\n");
    {
        // A timeline in "real" 10 ms ticks. The playback device plays `played` (silence while it is stuck)
        // and hands it to the echo reference; the capture device records the echo of what was played.
        // Each scenario decides per tick how many reference samples arrive and whether capture runs.
        constexpr int kTicks = 1600;          // 16 s
        constexpr int kEchoDelay = 2400;      // 50 ms speaker -> room -> mic
        struct Scenario
        {
            const char* name;
            double renderRate;                // reference samples per capture sample (clock drift)
            std::function<bool(int)> renderStuck, captureStuck;
            std::function<int(int)> burst;    // extra ticks of reference delivered at once at this tick
        };
        auto never = [](int) { return false; };
        auto between = [](int a, int b) { return [a, b](int t) { return t >= a && t < b; }; };
        const Scenario scenarios[] = {
            {"playback stuck 2 s", 1.0, between(600, 800), never, nullptr},
            {"capture stuck 2 s", 1.0, never, between(600, 800), nullptr},
            {"both stuck 2 s", 1.0, between(600, 800), between(600, 800), nullptr},
            {"reference late then 1 s burst", 1.0, between(600, 700), never, [](int t) { return t == 700 ? 100 : 0; }},
            {"playback clock +0.1 %", 1.001, never, never, nullptr},
            {"playback clock +100 ppm", 1.0001, never, never, nullptr},
            {"playback clock +20 ppm", 1.00002, never, never, nullptr},
            {"playback clock -100 ppm", 0.9999, never, never, nullptr},
            {"playback clock -0.1 %", 0.999, never, never, nullptr},
        };
        Settings st = def; st.nsEnabled = 0; st.hpfEnabled = 0; st.lpfEnabled = 0; st.limiterEnabled = 0;
        for (const Scenario& sc : scenarios)
        {
            // Far-end source (speech-like bursts), indexed in playback samples.
            const int srcLen = static_cast<int>(kTicks * 480 * 1.01) + 48000 * 2;
            std::mt19937 rng(5);
            std::normal_distribution<float> g(0.0f, 0.1f);
            std::vector<float> src(static_cast<size_t>(srcLen));
            for (int i = 0; i < srcLen; ++i) src[i] = ((i / 24000) % 5 < 3) ? g(rng) : 0.0f;

            double results[2] = {};
            size_t maxAvail = 0;
            uint64_t underruns = 0;
            double worstMs = 0;
            bool finite = true;
            for (int aecOn = 0; aecOn < 2; ++aecOn)
            {
                Settings s = st; s.aecEnabled = static_cast<uint32_t>(aecOn);
                ReferenceRing ring;
                ring.Init(48000);
                VoiceProcessor vp;
                vp.SetReference(&ring);
                vp.Prepare(kCh, 48000, s);
                // played[t] for every capture-clock sample (what the speaker emits at that instant).
                std::vector<float> played(static_cast<size_t>(kTicks) * 480, 0.0f);
                std::vector<float> pending;      // reference held back by a stuck-then-bursting device
                double renderPos = 0;            // playback samples consumed so far
                std::vector<float> y(static_cast<size_t>(kTicks) * 480 * kCh, 0.0f), mic(480 * kCh);
                float lp = 0;
                LARGE_INTEGER freq, t0, t1;
                QueryPerformanceFrequency(&freq);
                for (int t = 0; t < kTicks; ++t)
                {
                    // Playback side for this tick.
                    const bool rStuck = sc.renderStuck(t);
                    const double next = renderPos + 480 * sc.renderRate;
                    const int a = static_cast<int>(renderPos), b = static_cast<int>(next);
                    if (!rStuck)
                    {
                        for (int i = 0; i < 480; ++i)
                            played[static_cast<size_t>(t) * 480 + i] = src[static_cast<size_t>(a + static_cast<int>(i * sc.renderRate))];
                        ring.Write(src.data() + a, static_cast<size_t>(b - a));
                        renderPos = next;
                    }
                    else if (sc.burst)  // the device kept playing, but its reference is delivered late
                    {
                        for (int i = 0; i < 480; ++i)
                            played[static_cast<size_t>(t) * 480 + i] = src[static_cast<size_t>(a + i)];
                        pending.insert(pending.end(), src.begin() + a, src.begin() + b);
                        renderPos = next;
                    }
                    if (sc.burst && sc.burst(t) > 0)
                    {
                        ring.Write(pending.data(), pending.size());
                        pending.clear();
                    }
                    maxAvail = std::max(maxAvail, ring.Available());

                    // Capture side: the mic hears the echo of what was played 50 ms earlier.
                    for (int i = 0; i < 480; ++i)
                    {
                        const long long idx = static_cast<long long>(t) * 480 + i - kEchoDelay;
                        lp += 0.3f * ((idx >= 0 ? played[static_cast<size_t>(idx)] : 0.0f) - lp);
                        for (int c = 0; c < kCh; ++c) mic[static_cast<size_t>(i) * kCh + c] = 0.5f * lp;
                    }
                    if (sc.captureStuck(t))
                        continue;  // those mic samples are lost; the playback side kept running
                    QueryPerformanceCounter(&t0);
                    vp.Process(mic.data(), y.data() + static_cast<size_t>(t) * 480 * kCh, 480, s);
                    QueryPerformanceCounter(&t1);
                    worstMs = std::max(worstMs, 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
                }
                finite = finite && AllFinite(y);
                // Echo left 3..7 s after the event (11..15 s); the drift cases measure the same window.
                results[aecOn] = RmsDb(y, static_cast<size_t>(1100) * 480 * kCh, static_cast<size_t>(1500) * 480 * kCh);
                if (aecOn) underruns = vp.ReferenceUnderruns();
            }
            const double removed = results[0] - results[1];
            std::printf("    %-30s echo removed %5.1f dB after recovery, reference queue max %5zu (cap 48000), ref underruns %4llu, worst call %.2f ms\n",
                        sc.name, removed, maxAvail, static_cast<unsigned long long>(underruns), worstMs);
            EXPECT(finite, "%s: non-finite output", sc.name);
            EXPECT(maxAvail <= 48000, "%s: reference queue exceeded its capacity", sc.name);
            EXPECT(std::fabs(sc.renderRate - 1.0) > 0.0005 || removed > 15.0, "%s: only %.1f dB echo removal after recovery", sc.name, removed);
        }
    }

    std::printf("[14] settings hammered from another thread while processing (stage swaps, seqlock reads)\n");
    {
        Settings base = def; base.agcEnabled = 1;
        settings::Publish(base);
        ReferenceRing ring;
        ring.Init(48000);
        VoiceProcessor vp;
        vp.SetReference(&ring);
        vp.Prepare(kCh, 48000, base);
        std::atomic<bool> stop{false};
        std::atomic<int> changes{0};
        std::thread watcher([&] {
            Settings s = base;
            for (int i = 0; !stop.load(); ++i)
            {
                s.agcMaxGainDb = 20 + (i % 30);              // AGC2 rebuild
                s.aecStrength = static_cast<uint32_t>(i % 3); // AEC3 rebuild
                s.nsLevel = static_cast<uint32_t>(i % 4);
                settings::Publish(s);
                VoiceProcessor::OnSettingsChanged(s);
                ++changes;
                std::this_thread::sleep_for(std::chrono::microseconds(500 + (i % 7) * 300));
            }
        });
        auto noise = Noise(48000 * 4, -30.0f, 3);
        std::vector<float> y(noise.size()), refBlock(480, 0.01f);
        double worstMs = 0;
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        // Keep processing (looping the noise) until the watcher has made plenty of changes.
        for (int pos = 0, blocks = 0; changes.load() < 300 && blocks < 200000; pos = (pos + 480) % (48000 * 4), ++blocks)
        {
            ring.Write(refBlock.data(), 480);
            const Settings cur = settings::Current();
            QueryPerformanceCounter(&t0);
            vp.Process(noise.data() + static_cast<size_t>(pos) * kCh, y.data() + static_cast<size_t>(pos) * kCh, 480, cur);
            QueryPerformanceCounter(&t1);
            worstMs = std::max(worstMs, 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
        }
        stop = true;
        watcher.join();
        std::printf("    %d settings changes while processing, output finite: %s, worst call %.2f ms\n", changes.load(),
                    AllFinite(y) ? "yes" : "NO", worstMs);
        EXPECT(changes.load() >= 300, "watcher only made %d changes", changes.load());
        EXPECT(AllFinite(y), "non-finite output under concurrent settings changes");
        settings::Publish(Settings{});
    }

    std::printf("[15] echo timeline: 10 s steps, quiet steps marked, summary over playing steps only\n");
    {
        EchoTimeline tl;
        tl.Reset();
        // 6 minutes of meters (100 ms each): playing at 30 dB, then 1 min nothing playing, then 40 dB,
        // with a NaN thrown in; AEC off for the last 10 s (ignored).
        for (int m = 0; m < 3600; ++m)
        {
            const int sec = m / 10;
            const bool quiet = sec >= 120 && sec < 180;
            const float erle = m == 50 ? NAN : (sec < 120 ? 30.0f : 40.0f);
            tl.Add(erle, quiet ? -90.0f : -30.0f, sec < 350);
        }
        std::vector<std::string> lines;
        tl.Report([&](const char* l) { lines.emplace_back(l); });
        for (const auto& l : lines) std::printf("    %s\n", l.c_str());
        EXPECT(lines.size() == 3, "expected a summary + 2 timeline lines, got %zu", lines.size());
        EXPECT(!lines.empty() && lines[0].find("avg 35.9 dB, min 30.0, max 40.0 over 29 of 36 steps") != std::string::npos,
               "summary wrong: %s", lines.empty() ? "" : lines[0].c_str());
        EXPECT(lines.size() == 3 && lines[1].rfind("  00:00  30", 0) == 0 && lines[1].find("  .") != std::string::npos,
               "timeline line wrong: %s", lines.size() > 1 ? lines[1].c_str() : "");
    }

    std::printf("[16] speech meter (for the tray's activity dots): a 0..1 probability, low on steady noise, also with AGC off\n");
    {
        for (const bool agc : {true, false})
        {
            Settings s{};
            s.agcEnabled = agc ? 1u : 0u;
            VoiceProcessor p;
            p.Prepare(kCh, 48000, s);
            const auto in = Noise(48000 * 5, -50.0f, 7);
            std::vector<float> out(in.size());
            float lo = 1.0f, hi = 0.0f, last = 0.0f;
            int meters = 0;
            for (int pos = 0; pos < 48000 * 5; pos += 480)
            {
                const size_t off = static_cast<size_t>(pos) * kCh;
                p.Process(in.data() + off, out.data() + off, 480, s);
                VoiceProcessor::Meters m;
                if (p.TakeMeters(m))
                {
                    ++meters;
                    lo = std::min(lo, m.speech);
                    hi = std::max(hi, m.speech);
                    last = m.speech;
                }
            }
            std::printf("    AGC %-3s: %d meters, speech %.2f..%.2f, last %.2f\n", agc ? "on" : "off", meters, lo, hi, last);
            EXPECT(meters >= 45, "too few meters (%d)", meters);
            EXPECT(std::isfinite(lo) && std::isfinite(hi) && lo >= 0.0f && hi <= 1.0f, "speech out of range");
            EXPECT(last < 0.5f, "steady noise reads as speech (%.2f)", last);
        }
    }

    std::printf("[17] mic mute and a broken slider value: mute gives silence (also with AGC on); NaN never reaches the output\n");
    {
        for (const bool agc : {true, false})
        {
            Settings s{};
            s.agcEnabled = agc ? 1u : 0u;
            VoiceProcessor p;
            p.Prepare(kCh, 48000, s);
            const auto in = Noise(48000 * 2, -20.0f, 11);
            std::vector<float> out(in.size());
            for (int pos = 0; pos < 48000 * 2; pos += 480)
            {
                const size_t off = static_cast<size_t>(pos) * kCh;
                p.Process(in.data() + off, out.data() + off, 480, s, VoiceProcessor::kMutedDb);
            }
            const double muted = RmsDb(out, out.size() / 2, out.size());
            std::printf("    AGC %-3s muted: %.1f dBFS\n", agc ? "on" : "off", muted);
            EXPECT(muted < -100.0, "muted output not silent (%.1f dBFS)", muted);
        }
        Settings s{};
        VoiceProcessor p;
        p.Prepare(kCh, 48000, s);
        const auto in = Noise(48000 * 2, -20.0f, 12);
        std::vector<float> out(in.size());
        bool finite = true;
        for (int pos = 0; pos < 48000 * 2; pos += 480)
        {
            const size_t off = static_cast<size_t>(pos) * kCh;
            p.Process(in.data() + off, out.data() + off, 480, s, pos < 48000 ? std::numeric_limits<float>::quiet_NaN() : 0.0f);
            for (size_t i = off; i < off + 480 * kCh; ++i) finite &= std::isfinite(out[i]);
        }
        const double level = RmsDb(out, out.size() / 2, out.size());
        std::printf("    NaN slider then 0 dB: all samples finite=%d, output %.1f dBFS\n", finite ? 1 : 0, level);
        EXPECT(finite && level > -60.0, "NaN slider corrupted the output");
    }

    std::printf("[18] cost per 10 ms block on this machine (informational; the audio thread has 10 ms)\n");
    {
        std::printf("    CPU: AVX2 %s (WebRTC picks its AVX2 kernels at runtime; SSE2 otherwise)\n",
                    webrtc::cpu_info::Supports(webrtc::cpu_info::ISA::kAVX2) ? "detected" : "not detected");
        const int n = 48000 * 20;
        std::mt19937 rng(5);
        std::normal_distribution<float> g(0.0f, 0.05f);
        std::vector<float> farEnd(n), mic(static_cast<size_t>(kCh) * n);
        for (int f = 0; f < n; ++f)
        {
            farEnd[f] = g(rng);
            for (int c = 0; c < kCh; ++c) mic[static_cast<size_t>(f) * kCh + c] = 0.3f * farEnd[f] + 0.01f * g(rng);
        }
        auto bench = [&](const char* name, Settings st, bool withRef, int rate = 48000) {
            ReferenceRing ring;
            ring.Init(48000);
            VoiceProcessor vp;
            vp.SetReference(withRef ? &ring : nullptr);
            vp.Prepare(kCh, rate, st);
            const int block = rate / 100;
            std::vector<float> y(static_cast<size_t>(kCh) * block);
            std::vector<double> us;
            for (int pos = 0; pos + block <= n; pos += block)
            {
                ring.Write(farEnd.data() + pos, 480);
                const auto t0 = std::chrono::steady_clock::now();
                vp.Process(mic.data() + static_cast<size_t>(pos) * kCh, y.data(), block, st);
                us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            }
            us.erase(us.begin(), us.begin() + 100);  // warm-up
            std::sort(us.begin(), us.end());
            double sum = 0;
            for (double v : us) sum += v;
            std::printf("    %-34s mean %5.0f us (%4.1f %% of one core), p99 %5.0f us, max %5.0f us\n", name, sum / us.size(),
                        sum / us.size() / 100.0, us[us.size() * 99 / 100], us.back());
        };
        Settings all = def; all.aecEnabled = 1; all.nsEnabled = 1; all.agcEnabled = 1; all.limiterEnabled = 1;
        bench("everything on (AEC with speakers)", all, true);
        Settings noAec = all; noAec.aecEnabled = 0;
        bench("AEC switched off (reference kept)", noAec, true);
        bench("no speaker reference (no AEC3)", all, false);
        Settings noNs = all; noNs.nsEnabled = 0;
        bench("NS switched off", noNs, true);
        Settings none = all; none.aecEnabled = 0; none.nsEnabled = 0; none.agcEnabled = 0; none.hpfEnabled = 0; none.lpfEnabled = 0;
        none.limiterEnabled = 0;
        bench("all switched off (reference kept)", none, true);
        bench("everything on, 32 kHz mic (resampled)", all, true, 32000);
    }

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
