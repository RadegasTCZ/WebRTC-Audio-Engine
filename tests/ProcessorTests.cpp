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

    std::printf("[7] live switching (every stage, every level, bypass) stays finite and click-free\n");
    auto tone = Sine(frames, 1000.0f, 0.25f);
    auto sw = Run(tone, {441}, def, false, [](int k, Settings& s) {
        s.hpfEnabled = (k / 7) % 2;
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

    std::printf("[9] unsupported formats stay inactive\n");
    VoiceProcessor p;
    EXPECT(!p.Prepare(2, 44100, def) && !p.IsActive(), "44.1 kHz should be refused");
    EXPECT(!p.Prepare(9, 48000, def), "9 channels should be refused");

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
