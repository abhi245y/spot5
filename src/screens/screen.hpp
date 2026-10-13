// ps5-spotify - One screen of the app, and the frame it draws into.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same frame model as ps5-homebrew-ui's designs (app/concept.hpp), so the
// kit's techniques carry over unchanged: backdrop -> scene -> [glass capture]
// -> overlay -> post.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "screens/model.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

namespace screens
{

// The kit lives in hui:: (gfx, ui, tween, audio, input types).
using namespace hui;

struct Frame
{
    gfx::BackdropSpec backdrop; // procedural shader behind everything
    gfx::DrawList scene;        // the screen itself
    gfx::DrawList overlay;      // dialogs, above the glass capture
    gfx::BackdropSpec post;     // optional overlay shader (vignette)
    bool glass = false;         // blur backdrop + scene into glass_texture first
    std::uint32_t glass_texture = 0;

    void reset()
    {
        backdrop = {};
        post = {};
        scene.clear();
        overlay.clear();
        glass = false;
    }
};

// A cover ready to draw (texture 0 while it is still loading).
struct Art
{
    std::uint32_t texture = 0;
    style::AlbumPalette palette;
};

// Long-lived things every screen needs.
struct Context
{
    const ui::Fonts &fonts;
    Settings &settings;            // live: Control Room edits it
    bool settings_changed = false; // set by a screen; the app saves and applies, then clears
    // Covers by URL: returns what is loaded and asks for the rest. Cheap to
    // call every frame for every visible card.
    std::function<Art(const std::string &url)> art;
    std::optional<PlayRequest> play; // set by a screen; the app sends it on, then clears
};

class Screen
{
  public:
    virtual ~Screen() = default;
    // Became the visible screen: restart the entrance.
    virtual void enter() {}
    virtual void update(const Model &model, const InputFrame &input, float dt,
                        ui::Feedback &feedback, std::vector<Command> &commands) = 0;
    // Must not change state (it can be drawn twice in a transition).
    virtual void draw(const Model &model, Frame &frame) const = 0;
};

} // namespace screens
