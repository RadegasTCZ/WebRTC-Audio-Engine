// Offline tests for VoiceProcessor (HPF + WebRTC NS chain), no audio engine involved.
#include "../src/VoiceProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

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

    std::printf("[6b] hiss filter (12 kHz low-pass): 1 kHz passes, 18 kHz cut, gentle at 12 kHz (NS off)\n");
    {
        Settings lpOnly = def; lpOnly.nsEnabled = 0; lpOnly.hpfEnabled = 0; lpOnly.lpfEnabled = 1; lpOnly.limiterEnabled = 0;
        for (auto [hz, lo, hi] : {std::tuple{1000.0f, -0.2, 0.2}, std::tuple{12000.0f, 2.0, 4.5}, std::tuple{18000.0f, 6.0, 99.0}})
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
        Settings plain = def; plain.nsEnabled = 0; plain.hpfEnabled = 0; plain.limiterEnabled = 0;
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

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
