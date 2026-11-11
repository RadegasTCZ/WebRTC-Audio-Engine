// Offline tests for VoiceProcessor (WebRTC NS core), no audio engine involved.
#include "../src/VoiceProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static int g_failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); ++g_failures; } } while (0)

static std::vector<float> MakeSignal(int channels, int frames, float noiseDb, bool addTone, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, std::pow(10.0f, noiseDb / 20.0f));
    std::vector<float> s(static_cast<size_t>(channels) * frames);
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c)
        {
            float v = n(rng);
            // Speech-like: 300 Hz tone gated on/off every 250 ms.
            if (addTone && (f / 12000) % 2 == 0)
                v += 0.1f * std::sin(2.0f * 3.14159265f * 300.0f * f / 48000.0f);
            s[static_cast<size_t>(f) * channels + c] = v;
        }
    return s;
}

static std::vector<float> Run(const std::vector<float>& in, int channels, const std::vector<int>& periods, bool inPlace)
{
    VoiceProcessor p;
    p.Prepare(channels, 48000, VoiceProcessor::Level::k12dB);
    std::vector<float> out(in.size());
    if (inPlace) out = in;
    const int frames = static_cast<int>(in.size() / channels);
    int pos = 0, k = 0;
    while (pos < frames)
    {
        const int n = std::min(periods[static_cast<size_t>(k++) % periods.size()], frames - pos);
        const size_t off = static_cast<size_t>(pos) * channels;
        p.Process(inPlace ? out.data() + off : in.data() + off, out.data() + off, n);
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

int main()
{
    const int ch = 2, frames = 48000 * 4;

    std::printf("[1] output independent of engine period size\n");
    auto sig = MakeSignal(ch, frames, -40.0f, true, 1);
    auto ref = Run(sig, ch, {480}, false);
    for (auto periods : std::vector<std::vector<int>>{{128}, {441}, {1000}, {7, 480, 333, 1024, 1}})
    {
        auto o = Run(sig, ch, periods, false);
        EXPECT(std::memcmp(o.data(), ref.data(), o.size() * sizeof(float)) == 0, "period pattern starting %d differs", periods[0]);
    }

    std::printf("[2] in-place == separate buffers\n");
    auto inplace = Run(sig, ch, {441}, true);
    EXPECT(std::memcmp(inplace.data(), ref.data(), ref.size() * sizeof(float)) == 0, "in-place result differs");

    std::printf("[3] constant latency: first %d samples per channel are exactly zero (FIFO prefill)\n", VoiceProcessor::kBlock);
    bool zeros = true;
    for (size_t i = 0; i < static_cast<size_t>(VoiceProcessor::kBlock) * ch; ++i) zeros &= (ref[i] == 0.0f);
    EXPECT(zeros, "FIFO prefill not silent");

    std::printf("[4] steady noise is reduced\n");
    auto noise = MakeSignal(ch, frames, -40.0f, false, 2);
    auto noiseOut = Run(noise, ch, {480}, false);
    const size_t half = noise.size() / 2;
    const double inDb = RmsDb(noise, half, noise.size()), outDb = RmsDb(noiseOut, half, noise.size());
    std::printf("    noise in %.1f dBFS -> out %.1f dBFS (%.1f dB reduction)\n", inDb, outDb, inDb - outDb);
    EXPECT(inDb - outDb > 6.0, "expected > 6 dB reduction");

    std::printf("[5] speech-like bursts survive (level within 6 dB of input during bursts)\n");
    // Burst window: 2.0 .. 2.25 s (tone on), shifted by the processing latency.
    const size_t b0 = static_cast<size_t>(2.0 * 48000 + VoiceProcessor::kTotalLatency + 2400) * ch;
    const size_t b1 = static_cast<size_t>(2.25 * 48000 + VoiceProcessor::kTotalLatency - 2400) * ch;
    const double sigIn = RmsDb(sig, b0 - VoiceProcessor::kTotalLatency * ch, b1 - VoiceProcessor::kTotalLatency * ch);
    const double sigOut = RmsDb(ref, b0, b1);
    std::printf("    burst in %.1f dBFS -> out %.1f dBFS\n", sigIn, sigOut);
    EXPECT(std::fabs(sigIn - sigOut) < 6.0, "speech-like burst changed by more than 6 dB");

    std::printf("[6] NaN / Inf input never produces non-finite output\n");
    auto bad = sig;
    for (size_t i = 1000; i < 1100; ++i) bad[i] = (i % 2) ? NAN : INFINITY;
    auto badOut = Run(bad, ch, {480}, false);
    bool finite = true;
    for (float v : badOut) finite &= std::isfinite(v);
    EXPECT(finite, "non-finite output");

    std::printf("[7] unsupported formats stay inactive (pass-through)\n");
    VoiceProcessor p;
    EXPECT(!p.Prepare(2, 44100, VoiceProcessor::Level::k12dB) && !p.IsActive(), "44.1 kHz should be refused");
    EXPECT(!p.Prepare(9, 48000, VoiceProcessor::Level::k12dB), "9 channels should be refused");

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
