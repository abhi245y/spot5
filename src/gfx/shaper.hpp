// spot5 - Text in scripts the baked fonts lack (Malayalam, Devanagari, ...):
// shaped with HarfBuzz, its glyphs rasterised at runtime into one atlas.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/font.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hui::gfx
{

class GlBatch;

// Draw thread only (HarfBuzz is built without thread safety).
class Shaper
{
  public:
    Shaper();
    ~Shaper();
    Shaper(const Shaper &) = delete;
    Shaper &operator=(const Shaper &) = delete;

    // A TrueType/OpenType face; scale sizes its letters against the baked
    // fonts (1: same em). False if the data is not a font.
    bool add_face(std::string data, float scale = 1.0f);
    // Makes the atlas texture; call once the renderer is up.
    void attach(GlBatch &batch);
    // Sends glyphs rasterised since the last call to the GPU. Once per frame,
    // after the screens have laid out their text and before drawing.
    void upload(GlBatch &batch);

    // The first face that has the code point, or -1.
    int face_for(std::uint32_t codepoint) const;
    bool covers(int face, std::uint32_t codepoint) const;
    // Shapes text (all in one face) with its pen at x on baseline y; returns
    // the advance. Appends glyph quads when quads is given; a glyph not yet
    // rasterised (a few per frame) is left out until a later frame.
    float shape(int face, std::string_view text, float x, float y, float size,
                std::vector<GlyphQuad> *quads);

    // New glyphs rasterised per frame (~0.1 ms each on a PC); the rest wait
    // for later frames, so a screen of new text fills in instead of stalling.
    int per_frame = 12;

    // True when a line may not break or be cut between previous and next:
    // a mark, a joiner, or a letter after a virama belongs to the cluster.
    static bool joins(std::uint32_t previous, std::uint32_t next);

  private:
    struct Face;
    struct Placed
    {
        std::uint32_t glyph;
        float advance, offset_x, offset_y; // font units
    };
    struct Shaped
    {
        std::vector<Placed> glyphs;
        float advance = 0.0f; // font units
    };
    struct Slot
    {
        std::uint16_t x = 0, y = 0, w = 0, h = 0;
        float offset_x = 0.0f, offset_y = 0.0f; // atlas pixels from the pen
    };

    const Shaped &shaped(int face, std::string_view text);
    const Slot *slot(int face, std::uint32_t glyph);

    std::vector<std::unique_ptr<Face>> faces_;
    std::unordered_map<std::string, Shaped> shaped_;
    std::unordered_map<std::uint64_t, Slot> slots_;
    std::vector<std::uint8_t> atlas_;
    int pen_x_ = 1, pen_y_ = 1, shelf_ = 0;
    int dirty_top_ = 0, dirty_bottom_ = 0; // rows waiting for upload
    int rasterised_ = 0;                   // this frame
    std::uint32_t texture_ = 0;
};

} // namespace hui::gfx
