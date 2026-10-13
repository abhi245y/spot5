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

} // namespace screens::style
