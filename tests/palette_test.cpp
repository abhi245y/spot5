// ps5-spotify - Host check for style::palette_from_pixels.
// SPDX-License-Identifier: GPL-3.0-or-later
// Build/run: clang++ -std=c++20 -Isrc tests/palette_test.cpp -o /tmp/pt && /tmp/pt
#include <cassert>
#include <cstdio>
#include <vector>

#include "screens/style.hpp"

using namespace screens::style;

std::vector<std::uint8_t> image(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4)
        px[i] = r, px[i + 1] = g, px[i + 2] = b, px[i + 3] = 255;
    return px;
}

int main()
{
    // A red cover: red accent, reddish dark tone, nothing green or blue.
    auto red = image(640, 640, 220, 30, 40);
    AlbumPalette p = palette_from_pixels(red.data(), 640, 640);
    assert(p.accent.r > p.accent.g && p.accent.r > p.accent.b);
    assert(p.dark.r >= p.dark.g && p.dark.r >= p.dark.b);
    assert(p.accent.r > 0.9f); // bright enough for accents on dark

    // A grey cover stays grey (no invented colour).
    auto grey = image(300, 300, 128, 128, 128);
    p = palette_from_pixels(grey.data(), 300, 300);
    assert(std::fabs(p.mid.r - p.mid.g) < 0.01f && std::fabs(p.mid.g - p.mid.b) < 0.01f);

    // Mostly blue with one small vivid yellow patch: base blue, accent yellow.
    auto mix = image(640, 640, 20, 40, 120);
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 80; x++)
        {
            std::uint8_t *q = &mix[(static_cast<std::size_t>(y + 300) * 640 + x + 300) * 4];
            q[0] = 250, q[1] = 220, q[2] = 20;
        }
    p = palette_from_pixels(mix.data(), 640, 640);
    assert(p.mid.b > p.mid.r);                         // base from the blue majority
    assert(p.accent.r > p.accent.b && p.accent.g > p.accent.b); // accent yellow
    std::puts("palette OK");
}
