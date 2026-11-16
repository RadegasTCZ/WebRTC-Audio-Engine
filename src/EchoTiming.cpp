// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#include "EchoTiming.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "modules/audio_processing/utility/pffft_wrapper.h"

// ------------------------------------------------------------------ probe (real-time side)

void EchoTimingProbe::Push(const float* mic48, const float* ref48, int n) noexcept
{
    uint64_t w = m_written.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        m_accMic += mic48[i];
        m_accRef += ref48[i];
        if (++m_phase == kDecimation)
        {
            // Sum of 6 = a crude low-pass before taking every 6th sample. Both signals get the very same
            // treatment, so the relative timing is unaffected.
            m_mic[w & (kRingSize - 1)] = m_accMic;
            m_ref[w & (kRingSize - 1)] = m_accRef;
            ++w;
            m_accMic = m_accRef = 0.0f;
            m_phase = 0;
        }
    }
    m_written.store(w, std::memory_order_release);
}

void EchoTimingProbe::Clear() noexcept
{
    std::fill(std::begin(m_mic), std::end(m_mic), 0.0f);
    std::fill(std::begin(m_ref), std::end(m_ref), 0.0f);
    m_accMic = m_accRef = 0.0f;
    m_phase = 0;
    m_written.store(0, std::memory_order_release);
}

bool EchoTimingProbe::Latest(float* mic8, float* ref8, size_t n) const noexcept
{
    // The writer adds 80 samples per 10 ms block; copying n <= half the ring takes microseconds, so the
    // samples read are never overwritten meanwhile (and a torn sample would only spoil one measurement).
    const uint64_t w = m_written.load(std::memory_order_acquire);
    if (n > kRingSize / 2 || w < n)
        return false;
    const uint64_t start = w - n;
    for (size_t i = 0; i < n; ++i)
    {
        const size_t at = static_cast<size_t>((start + i) & (kRingSize - 1));
        mic8[i] = m_mic[at];
        ref8[i] = m_ref[at];
    }
    return true;
}

// ------------------------------------------------------------------ estimator (non-RT)

struct EchoTimingEstimator::Buffers
{
    std::unique_ptr<webrtc::Pffft::FloatBuffer> time, specRef, specMic, cross, gcc;
    std::vector<float> window, sRe, sIm, prr, pmm;
};

EchoTimingEstimator::EchoTimingEstimator()
    : m_fft(std::make_unique<webrtc::Pffft>(static_cast<size_t>(kFrame), webrtc::Pffft::FftType::kReal)),
      m_b(std::make_unique<Buffers>())
{
    m_b->time = m_fft->CreateBuffer();
    m_b->specRef = m_fft->CreateBuffer();
    m_b->specMic = m_fft->CreateBuffer();
    m_b->cross = m_fft->CreateBuffer();
    m_b->gcc = m_fft->CreateBuffer();
    m_b->window.resize(kFrame);
    for (int i = 0; i < kFrame; ++i)
        m_b->window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFrame));
    const size_t bins = kFrame / 2 + 1;
    m_b->sRe.resize(bins);
    m_b->sIm.resize(bins);
    m_b->prr.resize(bins);
    m_b->pmm.resize(bins);
}

EchoTimingEstimator::~EchoTimingEstimator() = default;

EchoTimingResult EchoTimingEstimator::Estimate(const float* mic8, const float* ref8) noexcept
{
    EchoTimingResult r;
    Buffers& b = *m_b;
    std::fill(b.sRe.begin(), b.sRe.end(), 0.0f);
    std::fill(b.sIm.begin(), b.sIm.end(), 0.0f);
    std::fill(b.prr.begin(), b.prr.end(), 0.0f);
    std::fill(b.pmm.begin(), b.pmm.end(), 0.0f);

    // 1. Welch average of the cross-spectrum over the frames where the speakers play.
    for (int k = 0; k < kFrames; ++k)
    {
        const float* ref = ref8 + static_cast<size_t>(k) * kHop;
        const float* mic = mic8 + static_cast<size_t>(k) * kHop;
        double e = 0.0;
        for (int i = 0; i < kFrame; ++i) e += static_cast<double>(ref[i]) * ref[i];
        // Samples are sums of 6 at 48 kHz: divide by 36 for 48 kHz full-scale power.
        const double dbfs = 10.0 * std::log10(e / kFrame / 36.0 + 1e-30);
        if (dbfs < kRefActiveDbfs)
            continue;
        ++r.activeFrames;

        auto transform = [&](const float* src, webrtc::Pffft::FloatBuffer* out) {
            auto t = b.time->GetView();
            for (int i = 0; i < kFrame; ++i) t[i] = src[i] * b.window[i];
            m_fft->ForwardTransform(*b.time, out, /*ordered=*/true);
        };
        transform(ref, b.specRef.get());
        transform(mic, b.specMic.get());
        // Ordered real layout: [DC, Nyquist, re1, im1, re2, im2, ...].
        const auto R = b.specRef->GetConstView();
        const auto M = b.specMic->GetConstView();
        for (int bin = 1; bin < kFrame / 2; ++bin)
        {
            const float rr = R[2 * bin], ri = R[2 * bin + 1], mr = M[2 * bin], mi = M[2 * bin + 1];
            b.sRe[bin] += rr * mr + ri * mi;   // conj(R) * M
            b.sIm[bin] += rr * mi - ri * mr;
            b.prr[bin] += rr * rr + ri * ri;
            b.pmm[bin] += mr * mr + mi * mi;
        }
    }
    if (r.activeFrames < kMinActiveFrames)
        return r;

    // 2. Coherence weighting (SCOT): every bin counts by how well mic and reference agree there, not by
    //    its loudness. This whitens speech, so its pitch harmonics do not smear the peak. 150..3600 Hz.
    auto c = b.cross->GetView();
    std::fill(c.begin(), c.end(), 0.0f);
    const int lo = 150 * kFrame / EchoTimingProbe::kRate, hi = 3600 * kFrame / EchoTimingProbe::kRate;
    int used = 0;
    for (int bin = lo; bin <= hi; ++bin)
    {
        const float d = std::sqrt(b.prr[bin] * b.pmm[bin]);
        if (!(d > 1e-20f)) continue;
        c[2 * bin] = b.sRe[bin] / d;
        c[2 * bin + 1] = b.sIm[bin] / d;
        ++used;
    }
    if (used == 0)
        return r;
    m_fft->BackwardTransform(*b.cross, b.gcc.get(), /*ordered=*/true);
    const auto g = b.gcc->GetConstView();
    const float scale = 1.0f / (2.0f * static_cast<float>(used));   // all bins in phase -> 1.0

    // 3. Peak search over the lag range (index = lag, negative lags wrap to the end).
    const int minLag = static_cast<int>(kMinLagMs * EchoTimingProbe::kRate / 1000);
    const int maxLag = static_cast<int>(kMaxLagMs * EchoTimingProbe::kRate / 1000);
    auto at = [&](int lag) { return std::fabs(g[static_cast<size_t>((lag + kFrame) % kFrame)]) * scale; };
    int best = 0;
    float peak = -1.0f;
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (const float v = at(lag); v > peak) { peak = v; best = lag; }
    // The echo often arrives twice within a few ms (direct sound + a desk reflection). AEC3 must cover the
    // EARLIEST arrival, so prefer an earlier local peak of at least 60 % within 5 ms before the strongest.
    const int strongest = best;
    for (int lag = std::max(minLag + 1, strongest - 40); lag < strongest; ++lag)
    {
        const float v = at(lag);
        if (v >= 0.6f * peak && v >= at(lag - 1) && v >= at(lag + 1)) { best = lag; break; }
    }
    double sum = 0.0, sumSq = 0.0;
    int n = 0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        if (lag >= best - 16 && lag <= strongest + 16) continue;   // the peak(s) +-2 ms
        const double v = at(lag);
        sum += v; sumSq += v * v; ++n;
    }
    const double mean = sum / n, sd = std::sqrt(std::max(1e-30, sumSq / n - mean * mean));
    r.peak = peak;
    r.quality = static_cast<float>((peak - mean) / sd);

    // Sub-sample position (parabola through the peak and its neighbours).
    float frac = 0.0f;
    if (best > minLag && best < maxLag)
    {
        const float y0 = at(best - 1), y1 = at(best), y2 = at(best + 1);
        const float den = y0 - 2.0f * y1 + y2;
        if (den < 0.0f) frac = std::clamp(0.5f * (y0 - y2) / den, -0.5f, 0.5f);
    }
    r.offsetMs = (static_cast<float>(best) + frac) * 1000.0f / EchoTimingProbe::kRate;

    const bool clear = r.quality >= kMinQuality && r.peak >= kMinPeak;
    const bool inRange = r.offsetMs >= kActMinMs && r.offsetMs <= kActMaxMs;
    r.valid = clear && inRange;
    r.outOfRange = clear && !inRange;
    return r;
}

// ------------------------------------------------------------------ controller (non-RT, pure logic)

void EchoTimingController::Reset(int currentDelaySamples) noexcept
{
    m_delay = std::clamp(currentDelaySamples, 0, kMaxDelaySamples);
    m_settle = 0;
    m_count = 0;
    m_agreed = false;
    m_lastRaw = 0.0f;
}

EchoTimingController::Decision EchoTimingController::OnResult(const EchoTimingResult& r) noexcept
{
    Decision d;
    d.delaySamples = m_delay;
    if (m_settle > 0)
    {
        --m_settle;   // the measured span still holds audio from before the last change
        return d;
    }
    if (!r.valid)
        return d;   // silence / near-end talk / no clear peak: neither counts nor breaks the streak

    // Raw offset = what the echo timing would be without our delay.
    const float raw = r.offsetMs - static_cast<float>(m_delay) / 48.0f;
    for (int i = kAgreeCount - 1; i > 0; --i) m_recent[i] = m_recent[i - 1];
    m_recent[0] = raw;
    if (m_count < kAgreeCount) ++m_count;
    if (m_count < kAgreeCount)
        return d;
    float s[kAgreeCount];
    std::copy(std::begin(m_recent), std::end(m_recent), s);
    std::sort(std::begin(s), std::end(s));
    if (s[kAgreeCount - 1] - s[0] > kAgreeMs)
        return d;

    const float agreed = s[kAgreeCount / 2];
    m_agreed = true;
    m_lastRaw = agreed;
    d.rawOffsetMs = agreed;
    float neededMs = std::max(0.0f, kMarginMs - agreed);
    if (neededMs > kMaxDelayMs)
    {
        neededMs = kMaxDelayMs;
        d.capped = true;
    }
    const int needed = static_cast<int>(std::lround(neededMs * 48.0f));
    if (std::abs(needed - m_delay) < static_cast<int>(kChangeMs * 48.0f))
        return d;

    m_delay = needed;
    m_settle = kSettleTicks;
    m_count = 0;
    d.change = true;
    d.delaySamples = needed;
    return d;
}

// ------------------------------------------------------------------ drift (non-RT)

void EchoDriftEstimator::Add(double seconds, float rawMs) noexcept
{
    if (m_n == kMax)
    {
        std::copy(m_t + 1, m_t + kMax, m_t);
        std::copy(m_v + 1, m_v + kMax, m_v);
        --m_n;
    }
    m_t[m_n] = seconds;
    m_v[m_n] = rawMs;
    ++m_n;
}

bool EchoDriftEstimator::Slope(double& ppm, int& points, double& span) const noexcept
{
    points = m_n;
    span = m_n > 1 ? m_t[m_n - 1] - m_t[0] : 0.0;
    if (m_n < 2) return false;
    // Segments: a new one starts at every jump.
    int seg[kMax];
    int id = 0;
    seg[0] = 0;
    for (int i = 1; i < m_n; ++i)
    {
        if (std::fabs(m_v[i] - m_v[i - 1]) > 1.5f) ++id;
        seg[i] = id;
    }
    static thread_local double slopes[kMax * (kMax - 1) / 2];
    int k = 0;
    for (int i = 0; i < m_n; ++i)
        for (int j = i + 1; j < m_n; ++j)
            if (seg[i] == seg[j] && m_t[j] - m_t[i] >= 5.0)   // pairs at least 5 s apart (noise ~0.1 ms)
                slopes[k++] = (m_v[j] - m_v[i]) / (m_t[j] - m_t[i]);
    if (k < 10) return false;
    std::nth_element(slopes, slopes + k / 2, slopes + k);
    ppm = slopes[k / 2] * 1000.0;   // ms per s -> ppm
    return true;
}
