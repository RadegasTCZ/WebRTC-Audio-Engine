// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#pragma once
// The app's glyph: rounded "level" bars like a voice waveform. Pure pixel math (no Windows calls), so
// the tray (drawn at runtime) and the build-time icon generator (tools/MakeIcon.cpp) share one drawing.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glyph
{
    // Premultiplied BGRA (0xAARRGGBB as a little-endian uint32).
    inline uint32_t Premul(uint32_t rgb, float alpha)
    {
        const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        const uint32_t r = ((rgb >> 16) & 0xFF) * a / 255, g = ((rgb >> 8) & 0xFF) * a / 255, b = (rgb & 0xFF) * a / 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    // "src over dst", both premultiplied.
    inline uint32_t Over(uint32_t src, uint32_t dst)
    {
        const uint32_t ia = 255 - (src >> 24);
        auto ch = [&](int shift) { return (((src >> shift) & 0xFF) + ((dst >> shift) & 0xFF) * ia / 255) << shift; };
        return ch(24) | ch(16) | ch(8) | ch(0);
    }

    // Draws the bars in `ink` (0xRRGGBB). tile != 0: on a rounded square of that color (0xRRGGBB), for the
    // app icon (taskbar, Explorer, Start menu), which must read on light and dark backgrounds alike.
    // Small tiled sizes get three wider bars so they stay crisp.
    inline void Render(int size, uint32_t* px, uint32_t ink, uint32_t tile = 0)
    {
        std::fill(px, px + size * size, 0u);
        const float s = static_cast<float>(size);
        const bool narrow = tile && size <= 24;  // inside a small tile five bars would be under 2 px wide
        const int bars = narrow ? 3 : 5;
        const float five[5] = {0.38f, 0.72f, 1.0f, 0.62f, 0.32f}, three[3] = {0.55f, 1.0f, 0.7f};
        const float* heights = narrow ? three : five;
        const float area = tile ? s * 0.64f : s;  // the bars sit inside the tile's padding
        const float barW = narrow ? area / 5.0f : area / 8.0f, gap = narrow ? area / 10.0f : area / 16.0f;
        const float total = bars * barW + (bars - 1) * gap, x0 = (s - total) / 2.0f;
        const float tileR = s * 0.22f;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float cx = x + 0.5f, cy = y + 0.5f;
                uint32_t out = 0;
                if (tile)
                {
                    // Rounded square: distance to the inner rectangle, minus the corner radius.
                    const float qx = std::max(std::abs(cx - s / 2) - (s / 2 - tileR), 0.0f);
                    const float qy = std::max(std::abs(cy - s / 2) - (s / 2 - tileR), 0.0f);
                    const float d = std::hypot(qx, qy) - tileR;
                    out = Premul(tile, 0.5f - d);
                }
                for (int b = 0; b < bars; ++b)
                {
                    const float h = heights[b] * area * 0.85f, top = (s - h) / 2.0f;
                    const float mid = x0 + b * (barW + gap) + barW / 2;
                    const float my = std::clamp(cy, top + barW / 2, top + h - barW / 2);
                    const float d = std::hypot(cx - mid, cy - my) - barW / 2;  // rounded bar
                    if (d < 0.5f)
                        out = Over(Premul(ink, 0.5f - d), out);
                }
                px[y * size + x] = out;
            }
    }

    inline constexpr uint32_t kTileColor = 0x2F6FED;  // app icon tile (blue); the bars are white
}
