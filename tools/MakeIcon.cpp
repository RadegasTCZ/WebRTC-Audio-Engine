// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// Build-time tool: writes the app icon (.ico, 16..256 px) from the same glyph the tray draws, so no
// binary image lives in the repo. Usage: MakeIcon <out.ico>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "../app/Glyph.h"

namespace
{
    void Put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
    void Put32(std::vector<uint8_t>& v, uint32_t x) { Put16(v, x & 0xFFFF); Put16(v, x >> 16); }

    // One icon image: BITMAPINFOHEADER (height doubled for the AND mask), bottom-up 32-bit BGRA
    // (straight alpha), then an all-zero 1-bit AND mask (alpha decides).
    std::vector<uint8_t> Image(int size)
    {
        std::vector<uint32_t> px(static_cast<size_t>(size) * size);
        glyph::Render(size, px.data(), 0xFFFFFF, glyph::kTileColor);
        std::vector<uint8_t> v;
        Put32(v, 40); Put32(v, size); Put32(v, size * 2); Put16(v, 1); Put16(v, 32);
        Put32(v, 0); Put32(v, 0); Put32(v, 0); Put32(v, 0); Put32(v, 0); Put32(v, 0);
        for (int y = size - 1; y >= 0; --y)
            for (int x = 0; x < size; ++x)
            {
                const uint32_t p = px[static_cast<size_t>(y) * size + x];
                const uint32_t a = p >> 24;
                auto un = [&](int shift) { return a ? std::min<uint32_t>(255, ((p >> shift) & 0xFF) * 255 / a) : 0; };
                v.push_back(static_cast<uint8_t>(un(0)));
                v.push_back(static_cast<uint8_t>(un(8)));
                v.push_back(static_cast<uint8_t>(un(16)));
                v.push_back(static_cast<uint8_t>(a));
            }
        const size_t maskRow = ((size + 31) / 32) * 4;
        v.insert(v.end(), maskRow * size, 0);
        return v;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage: MakeIcon <out.ico>\n"); return 2; }
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<std::vector<uint8_t>> images;
    for (int s : sizes) images.push_back(Image(s));
    std::vector<uint8_t> f;
    Put16(f, 0); Put16(f, 1); Put16(f, static_cast<uint32_t>(images.size()));  // ICONDIR
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
    for (size_t i = 0; i < images.size(); ++i)  // ICONDIRENTRY
    {
        const int s = sizes[i];
        f.push_back(static_cast<uint8_t>(s == 256 ? 0 : s));
        f.push_back(static_cast<uint8_t>(s == 256 ? 0 : s));
        f.push_back(0); f.push_back(0);
        Put16(f, 1); Put16(f, 32);
        Put32(f, static_cast<uint32_t>(images[i].size()));
        Put32(f, offset);
        offset += static_cast<uint32_t>(images[i].size());
    }
    for (auto& img : images) f.insert(f.end(), img.begin(), img.end());
    FILE* out = nullptr;
    if (fopen_s(&out, argv[1], "wb") || !out) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    const bool ok = std::fwrite(f.data(), 1, f.size(), out) == f.size();
    std::fclose(out);
    return ok ? 0 : 1;
}
