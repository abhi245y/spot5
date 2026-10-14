// spot5 - The song menu: Options on a song opens its radio, its album, its
// artists and its details. Two looks, chosen in Control Room: a panel from
// the right edge, or a pop-up at the song's row.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "core/tween.hpp"
#include "screens/screen.hpp"

namespace screens
{

class SongMenu
{
  public:
    explicit SongMenu(Context &context) : context_(context) {}

    // anchor: the song's row on screen (the pop-up points at it).
    // panel: the side panel even when the setting asks for the pop-up.
    void open(const TrackRow &song, const gfx::Rect &anchor, bool panel, ui::Feedback &feedback);
    // Closes at once, without a sound (the screen changed under it).
    void dismiss()
    {
        open_ = false;
    }
    bool is_open() const
    {
        return open_;
    }
    // Takes input while open; actions go to context.requests.
    void update(const InputFrame &input, float dt, ui::Feedback &feedback);
    void draw(gfx::DrawList &list) const;

  private:
    enum class View
    {
        actions,
        artists, // more than one artist: pick one
        details,
    };
    enum class Act
    {
        radio,
        album,
        artist,
        details,
    };

    void show(View view);
    int count() const;
    void activate(ui::Feedback &feedback);
    void draw_panel(gfx::DrawList &list, float amount) const;
    void draw_popup(gfx::DrawList &list, float amount) const;
    // Rows of the current view from (x, top), each row_h tall, inside width w.
    void draw_rows(gfx::DrawList &list, float x, float top, float w, float row_h, float size) const;

    Context &context_;
    TrackRow song_;
    gfx::Rect anchor_;
    std::vector<Act> acts_;
    View view_ = View::actions;
    int focus_ = 0;
    bool open_ = false;
    bool popup_ = false; // the look, fixed while open
    tween::Spring amount_;  // 0 closed .. 1 open
    tween::Spring pill_;    // focused row, as a fractional index
    tween::Spring view_in_; // 0 .. 1 as a new view's rows arrive
};

} // namespace screens
