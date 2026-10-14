// spot5 - Text in scripts the baked fonts lack, shaped with HarfBuzz and drawn
// from glyphs rasterised at runtime.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/shaper.hpp"

#include "gfx/gl_batch.hpp"

#include <hb.h>

#include "third_party/stb/stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hui::gfx
{

namespace
{

// Glyphs are rasterised as the baked fonts are: 56 px em, 8 px distance spread.
constexpr int kAtlas = 2048;
constexpr float kPixels = 56.0f;
constexpr int kRange = 8;

// Felzenszwalb and Huttenlocher's 1D squared distance transform.
void distance_1d(const float *f, float *d, int *v, float *z, int n)
{
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;
    for (int q = 1, k = 0; q < n; ++q)
    {
        float s = 0.0f;
        while ((s = ((f[q] + static_cast<float>(q * q)) - (f[v[k]] + static_cast<float>(v[k] * v[k]))) /
                    static_cast<float>(2 * q - 2 * v[k])) <= z[k])
            --k;
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }
    for (int q = 0, k = 0; q < n; ++q)
    {
        while (z[k + 1] < static_cast<float>(q))
            ++k;
        d[q] = static_cast<float>((q - v[k]) * (q - v[k])) + f[v[k]];
    }
}

void distance_2d(std::vector<float> &grid, int w, int h)
{
    const int n = std::max(w, h);
    std::vector<float> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)),
        z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < w; ++x)
    {
        for (int y = 0; y < h; ++y)
            f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y * w + x)];
        distance_1d(f.data(), d.data(), v.data(), z.data(), h);
        for (int y = 0; y < h; ++y)
            grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(y)];
    }
    for (int y = 0; y < h; ++y)
    {
        std::copy_n(&grid[static_cast<std::size_t>(y * w)], w, f.begin());
        distance_1d(f.data(), d.data(), v.data(), z.data(), w);
        std::copy_n(d.begin(), w, &grid[static_cast<std::size_t>(y * w)]);
    }
}

} // namespace

struct Shaper::Face
{
    std::string data;
    float scale = 1.0f;
    stbtt_fontinfo info{};
    float raster_scale = 0.0f; // font units to atlas pixels
    hb_font_t *font = nullptr;
    float upem = 1000.0f;

    ~Face()
    {
        hb_font_destroy(font);
    }
};

Shaper::Shaper() : atlas_(static_cast<std::size_t>(kAtlas * kAtlas), 0) {}

Shaper::~Shaper() = default;

bool Shaper::add_face(std::string data, float scale)
{
    auto face = std::make_unique<Face>();
    face->data = std::move(data);
    face->scale = scale;
    const auto *bytes = reinterpret_cast<const unsigned char *>(face->data.data());
    if (!stbtt_InitFont(&face->info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)))
        return false;
    face->raster_scale = stbtt_ScaleForMappingEmToPixels(&face->info, kPixels);
    hb_blob_t *blob = hb_blob_create(face->data.data(), static_cast<unsigned>(face->data.size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t *hb_face = hb_face_create(blob, 0);
    face->font = hb_font_create(hb_face);
    face->upem = static_cast<float>(hb_face_get_upem(hb_face));
    hb_face_destroy(hb_face);
    hb_blob_destroy(blob);
    faces_.push_back(std::move(face));
    return true;
}

void Shaper::attach(GlBatch &batch)
{
    texture_ = batch.create_font_texture(kAtlas, kAtlas, atlas_.data());
}

void Shaper::upload(GlBatch &batch)
{
    rasterised_ = 0;
    if (texture_ == 0 || dirty_bottom_ <= dirty_top_)
        return;
    batch.update_font_texture(texture_, dirty_top_, dirty_bottom_ - dirty_top_, kAtlas,
                              &atlas_[static_cast<std::size_t>(dirty_top_ * kAtlas)]);
    dirty_top_ = dirty_bottom_ = 0;
}

int Shaper::face_for(std::uint32_t codepoint) const
{
    for (std::size_t i = 0; i < faces_.size(); ++i)
    {
        if (covers(static_cast<int>(i), codepoint))
            return static_cast<int>(i);
    }
    return -1;
}

bool Shaper::covers(int face, std::uint32_t codepoint) const
{
    return stbtt_FindGlyphIndex(&faces_[static_cast<std::size_t>(face)]->info,
                                static_cast<int>(codepoint)) != 0;
}

bool Shaper::joins(std::uint32_t previous, std::uint32_t next)
{
    if (next == 0x200c || next == 0x200d || previous == 0x200d)
        return true;
    hb_unicode_funcs_t *unicode = hb_unicode_funcs_get_default();
    switch (hb_unicode_general_category(unicode, next))
    {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
        return true;
    default:
        return hb_unicode_combining_class(unicode, previous) == HB_UNICODE_COMBINING_CLASS_VIRAMA;
    }
}

const Shaper::Shaped &Shaper::shaped(int face, std::string_view text)
{
    std::string key(1, static_cast<char>(face));
    key.append(text);
    if (const auto it = shaped_.find(key); it != shaped_.end())
        return it->second;
    // ponytail: dropped wholesale when large; an LRU if titles churn it.
    if (shaped_.size() > 4096)
        shaped_.clear();
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_set_flags(buffer, HB_BUFFER_FLAG_DO_NOT_INSERT_DOTTED_CIRCLE);
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0,
                       static_cast<int>(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(faces_[static_cast<std::size_t>(face)]->font, buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t *position = hb_buffer_get_glyph_positions(buffer, &count);
    Shaped result;
    result.glyphs.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
        result.glyphs.push_back({info[i].codepoint, static_cast<float>(position[i].x_advance),
                                 static_cast<float>(position[i].x_offset),
                                 static_cast<float>(position[i].y_offset)});
        result.advance += static_cast<float>(position[i].x_advance);
    }
    hb_buffer_destroy(buffer);
    return shaped_.emplace(std::move(key), std::move(result)).first->second;
}

const Shaper::Slot *Shaper::slot(int face, std::uint32_t glyph)
{
    const std::uint64_t key = (static_cast<std::uint64_t>(face) << 32) | glyph;
    if (const auto it = slots_.find(key); it != slots_.end())
        return &it->second;
    if (rasterised_ >= per_frame)
        return nullptr;
    ++rasterised_;

    // TinySDF's method: an anti-aliased coverage bitmap, then exact distance
    // transforms inside and out. ~30x faster than stb_truetype's own SDF.
    const Face &f = *faces_[static_cast<std::size_t>(face)];
    const int index = static_cast<int>(glyph);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&f.info, index, f.raster_scale, f.raster_scale, &x0, &y0, &x1, &y1);
    Slot result;
    if (x1 <= x0 || y1 <= y0)
        return &slots_.emplace(key, result).first->second; // a space: nothing to draw
    const int w = x1 - x0 + 2 * kRange;
    const int h = y1 - y0 + 2 * kRange;
    if (pen_x_ + w + 1 > kAtlas)
    {
        pen_x_ = 1;
        pen_y_ += shelf_ + 1;
        shelf_ = 0;
    }
    if (pen_y_ + h + 1 > kAtlas)
    {
        // Full: start over. Glyphs still on screen come back over the next frames.
        slots_.clear();
        std::fill(atlas_.begin(), atlas_.end(), 0);
        pen_x_ = pen_y_ = 1;
        shelf_ = 0;
        dirty_top_ = 0;
        dirty_bottom_ = kAtlas;
    }
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(w * h), 0);
    stbtt_MakeGlyphBitmap(&f.info, &coverage[static_cast<std::size_t>(kRange * w + kRange)], x1 - x0,
                          y1 - y0, w, f.raster_scale, f.raster_scale, index);
    std::vector<float> outside(coverage.size()), inside(coverage.size());
    for (std::size_t i = 0; i < coverage.size(); ++i)
    {
        const float a = static_cast<float>(coverage[i]) / 255.0f;
        outside[i] = a >= 1.0f ? 0.0f : a <= 0.0f ? 1e20f : std::max(0.0f, 0.5f - a) * std::max(0.0f, 0.5f - a);
        inside[i] = a >= 1.0f ? 1e20f : a <= 0.0f ? 0.0f : std::max(0.0f, a - 0.5f) * std::max(0.0f, a - 0.5f);
    }
    distance_2d(outside, w, h);
    distance_2d(inside, w, h);
    // The baked fonts' encoding: 128 on the edge, 128 / range less per pixel out.
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(y * w + x);
            const float value =
                128.0f - (std::sqrt(outside[i]) - std::sqrt(inside[i])) * (128.0f / kRange);
            atlas_[static_cast<std::size_t>((pen_y_ + y) * kAtlas + pen_x_ + x)] =
                static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
        }
    }
    result.x = static_cast<std::uint16_t>(pen_x_);
    result.y = static_cast<std::uint16_t>(pen_y_);
    result.w = static_cast<std::uint16_t>(w);
    result.h = static_cast<std::uint16_t>(h);
    result.offset_x = static_cast<float>(x0 - kRange);
    result.offset_y = static_cast<float>(y0 - kRange);
    if (dirty_bottom_ <= dirty_top_)
        dirty_top_ = dirty_bottom_ = pen_y_;
    dirty_top_ = std::min(dirty_top_, pen_y_);
    dirty_bottom_ = std::max(dirty_bottom_, pen_y_ + h);
    pen_x_ += w + 1;
    shelf_ = std::max(shelf_, h);
    return &slots_.emplace(key, result).first->second;
}

float Shaper::shape(int face, std::string_view text, float x, float y, float size,
                    std::vector<GlyphQuad> *quads)
{
    const Shaped &run = shaped(face, text);
    const Face &f = *faces_[static_cast<std::size_t>(face)];
    const float units = size * f.scale / f.upem;  // font units to output pixels
    const float pixels = size * f.scale / kPixels; // atlas pixels to output pixels
    if (quads == nullptr)
        return run.advance * units;
    float pen = x;
    for (const Placed &placed : run.glyphs)
    {
        const Slot *s = slot(face, placed.glyph);
        if (s != nullptr && s->w > 0)
        {
            GlyphQuad quad;
            quad.x0 = pen + placed.offset_x * units + s->offset_x * pixels;
            quad.y0 = y - placed.offset_y * units + s->offset_y * pixels;
            quad.x1 = quad.x0 + static_cast<float>(s->w) * pixels;
            quad.y1 = quad.y0 + static_cast<float>(s->h) * pixels;
            constexpr float inverse = 1.0f / static_cast<float>(kAtlas);
            quad.u0 = static_cast<float>(s->x) * inverse;
            quad.v0 = static_cast<float>(s->y) * inverse;
            quad.u1 = static_cast<float>(s->x + s->w) * inverse;
            quad.v1 = static_cast<float>(s->y + s->h) * inverse;
            quad.texture = texture_;
            quad.range = static_cast<float>(kRange) * pixels;
            quads->push_back(quad);
        }
        pen += placed.advance * units;
    }
    return run.advance * units;
}

} // namespace hui::gfx
