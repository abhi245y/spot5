// ps5-spotify - Shared look: colours, the album palette, small helpers.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "gfx/draw_list.hpp"

namespace screens::style
{

using namespace hui;
using gfx::Color;

constexpr float kMargin = 96.0f;
constexpr float kRight = gfx::kVirtualWidth - kMargin;
constexpr float kCentreX = gfx::kVirtualWidth * 0.5f;
constexpr float kCentreY = gfx::kVirtualHeight * 0.5f;
constexpr float kTau = 6.2831853f;

const Color kInk = Color::rgb(0xf4fff8);    // all text: a white with a breath of green
const Color kNight = Color::rgb(0x020604);  // the black moves pass through
const Color kGreen = Color::rgb(0x1ed760);  // the app's accent
const Color kMint = Color::rgb(0x9ff0c0);   // pale end of the accent
const Color kDeep = Color::rgb(0x0b3d22);   // dark end of the accent
const Color kDanger = Color::rgb(0xff6b6b); // errors
const Color kClear = Color::rgb(0x000000, 0.0f);

// "3:07" (or "1:02:03" for long episodes).
inline void format_time(char *out, std::size_t size, float seconds)
{
    const int whole = std::max(0, static_cast<int>(seconds));
    if (whole >= 3600)
        std::snprintf(out, size, "%d:%02d:%02d", whole / 3600, whole / 60 % 60, whole % 60);
    else
        std::snprintf(out, size, "%d:%02d", whole / 60, whole % 60);
}

inline Color hsv(float h, float s, float v)
{
    h = h - std::floor(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h * 6.0f) % 6)
    {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return {r + m, g + m, b + m, 1.0f};
}

// Four colours for an album, like the kit's catalogue items (dark, mid,
// accent) so the designs' palette code carries over. Derived from the name
// until real cover art is decoded; the same album always gets the same tones.
struct AlbumPalette
{
    Color dark, mid, accent;
};

inline AlbumPalette album_palette(std::string_view name)
{
    std::uint32_t h = 2166136261u; // FNV-1a
    for (char c : name)
        h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    return {hsv(hue, 0.65f, 0.16f), hsv(hue + 0.04f, 0.6f, 0.42f), hsv(hue + 0.08f, 0.55f, 0.95f)};
}

// The cover's own colours, at the same brightness levels as album_palette()
// so text stays readable on any cover: dark and mid from the average hue,
// the accent from the most vivid sample. Grey covers stay grey.
inline AlbumPalette palette_from_pixels(const std::uint8_t *rgba, int width, int height)
{
    constexpr int kGrid = 24;
    float sum_r = 0, sum_g = 0, sum_b = 0;
    float best = -1.0f, best_h = 0.0f, best_s = 0.0f;
    for (int gy = 0; gy < kGrid; gy++)
    {
        for (int gx = 0; gx < kGrid; gx++)
        {
            const int x = (gx * 2 + 1) * width / (kGrid * 2);
            const int y = (gy * 2 + 1) * height / (kGrid * 2);
            const std::uint8_t *p = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
            const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            sum_r += r, sum_g += g, sum_b += b;
            const float hi = std::max({r, g, b}), lo = std::min({r, g, b});
            const float sat = hi > 0.0f ? (hi - lo) / hi : 0.0f;
            const float score = sat * (0.35f + hi); // vivid and not too dark
            if (score > best)
            {
                best = score;
                best_s = sat;
                float h = 0.0f;
                if (hi > lo)
                {
                    if (hi == r)
                        h = (g - b) / (hi - lo) / 6.0f;
                    else if (hi == g)
                        h = (2.0f + (b - r) / (hi - lo)) / 6.0f;
                    else
                        h = (4.0f + (r - g) / (hi - lo)) / 6.0f;
                }
                best_h = h < 0.0f ? h + 1.0f : h;
            }
        }
    }
    const float n = static_cast<float>(kGrid * kGrid);
    const float r = sum_r / n, g = sum_g / n, b = sum_b / n;
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b});
    const float avg_s = hi > 0.0f ? (hi - lo) / hi : 0.0f;
    float avg_h = 0.0f;
    if (hi > lo)
    {
        if (hi == r)
            avg_h = (g - b) / (hi - lo) / 6.0f;
        else if (hi == g)
            avg_h = (2.0f + (b - r) / (hi - lo)) / 6.0f;
        else
            avg_h = (4.0f + (r - g) / (hi - lo)) / 6.0f;
    }
    const float base_s = std::min(avg_s * 1.3f, 0.7f);
    return {hsv(avg_h, base_s, 0.16f), hsv(avg_h, base_s, 0.42f),
            hsv(best_h, std::clamp(best_s, 0.0f, 0.75f), 0.95f)};
}

} // namespace screens::style
