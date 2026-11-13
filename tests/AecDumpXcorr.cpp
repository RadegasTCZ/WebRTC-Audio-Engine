// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-correlates L (mic) and R (reference) of an aecdump WAV (float32, 48 kHz, stereo).
// Positive lag = the echo in the mic comes AFTER the reference (normal, AEC3 can model it).
// Negative lag = the echo arrives BEFORE the reference (AEC3 cannot model it).
// Recordings come from the effect: set HKLM\SOFTWARE\WebrtcVoiceApo\DiagDumpSeconds = N (1..120), start a
// stream, play speech on the speakers, stop the stream -> aecdump-*.wav next to the engine log.
// Usage: AecDumpXcorr.exe <aecdump.wav>
// Coarse search at 8 kHz over +-600 ms in 10 s windows, then refined at 48 kHz.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: AecDumpXcorr <aecdump.wav>\n"); return 1; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("cannot open %s\n", argv[1]); return 1; }
    std::fseek(f, 0, SEEK_END); long size = std::ftell(f); std::fseek(f, 44, SEEK_SET);
    const size_t n = (size - 44) / 8;
    std::vector<float> lr(n * 2);
    std::fread(lr.data(), 4, n * 2, f); std::fclose(f);
    std::vector<float> mic(n), ref(n);
    for (size_t i = 0; i < n; ++i) { mic[i] = lr[2 * i]; ref[i] = lr[2 * i + 1]; }

    // 8 kHz versions (average of 6 = crude low-pass).
    const size_t m = n / 6;
    std::vector<float> mic8(m), ref8(m);
    for (size_t i = 0; i < m; ++i)
    {
        float a = 0, b = 0;
        for (int k = 0; k < 6; ++k) { a += mic[i * 6 + k]; b += ref[i * 6 + k]; }
        mic8[i] = a; ref8[i] = b;
    }
    const int maxLag8 = 4800;  // 600 ms at 8 kHz
    const size_t win = 80000;  // 10 s
    std::printf("%s\n window   best lag      corr   (lag > 0: echo after reference = OK; < 0: echo BEFORE reference)\n", argv[1]);
    for (size_t w0 = maxLag8; w0 + win + maxLag8 < m; w0 += win)
    {
        double refE = 0, micE = 0;
        for (size_t i = w0; i < w0 + win; ++i) { refE += ref8[i] * ref8[i]; micE += mic8[i] * mic8[i]; }
        int best = 0; double bestC = -1;
        for (int lag = -maxLag8; lag <= maxLag8; ++lag)
        {
            double c = 0;
            for (size_t i = w0; i < w0 + win; ++i) c += static_cast<double>(ref8[i]) * mic8[i + lag];
            c = std::fabs(c);
            if (c > bestC) { bestC = c; best = lag; }
        }
        // Refine at 48 kHz around 6*best +-12 samples.
        int best48 = best * 6; double bc = -1;
        for (int lag = best * 6 - 12; lag <= best * 6 + 12; ++lag)
        {
            double c = 0;
            for (size_t i = w0 * 6; i < (w0 + win) * 6; ++i) c += static_cast<double>(ref[i]) * mic[i + lag];
            c = std::fabs(c);
            if (c > bc) { bc = c; best48 = lag; }
        }
        std::printf("  %3zu-%3zu s  %+7.2f ms   %.2f\n", w0 / 8000, (w0 + win) / 8000, best48 / 48.0,
                    bestC / std::sqrt(refE * micE + 1e-30));
    }
    return 0;
}
