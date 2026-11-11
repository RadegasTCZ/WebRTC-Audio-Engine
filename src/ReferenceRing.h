// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// Lock-free single-producer / single-consumer ring of mono float samples (48 kHz echo reference).
// Producer: IApoAuxiliaryInputRT::AcceptInput. Consumer: the capture processing thread.
// Never allocates after Init().
class ReferenceRing
{
public:
    void Init(size_t capacity) { m_buf.assign(capacity, 0.0f); m_w.store(0); m_r.store(0); }
    size_t Capacity() const noexcept { return m_buf.size(); }
    size_t Available() const noexcept { return m_w.load(std::memory_order_acquire) - m_r.load(std::memory_order_acquire); }

    // Returns the number of samples written (drops the rest if full).
    size_t Write(const float* src, size_t n) noexcept
    {
        const size_t w = m_w.load(std::memory_order_relaxed), r = m_r.load(std::memory_order_acquire);
        const size_t space = m_buf.size() - (w - r);
        if (n > space) n = space;
        for (size_t i = 0; i < n; ++i) m_buf[(w + i) % m_buf.size()] = src ? src[i] : 0.0f;
        m_w.store(w + n, std::memory_order_release);
        return n;
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
    // Consumer side: drop samples so that at most `keep` remain (bounds the reference delay).
    void TrimTo(size_t keep) noexcept
    {
        const size_t r = m_r.load(std::memory_order_relaxed), w = m_w.load(std::memory_order_acquire);
        if (w - r > keep) m_r.store(w - keep, std::memory_order_release);
    }

private:
    std::vector<float> m_buf;
    std::atomic<size_t> m_w{0}, m_r{0};
};
