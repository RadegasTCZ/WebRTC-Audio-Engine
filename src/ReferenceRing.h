// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

// Lock-free single-producer / single-consumer ring of mono float samples (48 kHz echo reference).
// Producer: IApoAuxiliaryInputRT::AcceptInput. Consumer: the capture processing thread.
// Never allocates after Init().
//
// The producer may stamp each write with a clock reading (e.g. QueryPerformanceCounter): the consumer then
// knows how much reference "should" be there at any moment (ReferenceSync uses it to follow the reference
// rate precisely, independent of how the writes are chunked).
class ReferenceRing
{
public:
    // Allocate once, before any producer or consumer can run (it may throw std::bad_alloc).
    void Init(size_t capacity) { m_buf.assign(capacity, 0.0f); m_w.store(0); m_r.store(0); m_seq.store(0); m_stampW.store(0); m_stampT.store(0); }
    // Empties the ring without reallocating. Safe against a concurrent producer/consumer in the sense
    // that the buffer never moves: at worst a few stale samples are read once.
    void Reset() noexcept { m_r.store(m_w.load(std::memory_order_acquire), std::memory_order_release); }
    size_t Capacity() const noexcept { return m_buf.size(); }
    size_t Available() const noexcept { return m_w.load(std::memory_order_acquire) - m_r.load(std::memory_order_acquire); }
    size_t ReadCount() const noexcept { return m_r.load(std::memory_order_acquire); }

    // Returns the number of samples written (drops the rest if full). ticks != 0: clock reading at this write.
    size_t Write(const float* src, size_t n, int64_t ticks = 0) noexcept
    {
        const size_t w = m_w.load(std::memory_order_relaxed), r = m_r.load(std::memory_order_acquire);
        const size_t space = m_buf.size() - (w - r);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) m_buf[(w + i) % m_buf.size()] = src ? src[i] : 0.0f;
        m_w.store(w + n, std::memory_order_release);
        if (ticks != 0)
        {
            // Seqlock: odd while the stamp is being written.
            const uint32_t s = m_seq.load(std::memory_order_relaxed);
            m_seq.store(s + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            m_stampW.store(w + n, std::memory_order_relaxed);
            m_stampT.store(ticks, std::memory_order_relaxed);
            m_seq.store(s + 2, std::memory_order_release);
        }
        return n;
    }
    // Consumer: the last stamped write (total samples written then, clock reading). False if none / torn.
    bool LastStamp(size_t& written, int64_t& ticks) const noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t s1 = m_seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            const size_t w = m_stampW.load(std::memory_order_relaxed);
            const int64_t t = m_stampT.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_seq.load(std::memory_order_relaxed) != s1) continue;
            if (t == 0) return false;
            written = w;
            ticks = t;
            return true;
        }
        return false;
    }
    // Reads exactly n samples if available; otherwise reads nothing and returns false.
    bool Read(float* dst, size_t n) noexcept
    {
        const size_t r = m_r.load(std::memory_order_relaxed), w = m_w.load(std::memory_order_acquire);
        if (w - r < n) return false;
        for (size_t i = 0; i < n; ++i) dst[i] = m_buf[(r + i) % m_buf.size()];
        m_r.store(r + n, std::memory_order_release);
        return true;
    }
    // Consumer side: drop exactly n of the queued samples (fewer if not that many). Relative to the read
    // position, so a write landing meanwhile is kept (TrimTo(Available() - n) would drop it too).
    void Drop(size_t n) noexcept
    {
        const size_t r = m_r.load(std::memory_order_relaxed), w = m_w.load(std::memory_order_acquire);
        m_r.store(r + std::min(n, w - r), std::memory_order_release);
    }
    // Consumer side: drop samples so that at most `keep` remain (bounds the reference delay).
    void TrimTo(size_t keep) noexcept
    {
        const size_t r = m_r.load(std::memory_order_relaxed), w = m_w.load(std::memory_order_acquire);
        if (w - r > keep) m_r.store(w - keep, std::memory_order_release);
    }

private:
    std::vector<float> m_buf;
    std::atomic<size_t> m_w{0}, m_r{0};
    std::atomic<uint32_t> m_seq{0};
    std::atomic<size_t> m_stampW{0};    // guarded by m_seq
    std::atomic<int64_t> m_stampT{0};
};
