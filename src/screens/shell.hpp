// ps5-spotify - Chooses the screen, moves between screens, owns the overlays.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   launch  --(ready, loader done)--> home or now playing   iris
//   home   <--(a track arrives / ends)--> now playing        slide
//   any    --(connection error)--> launch (shows the error)  through black
//   any   <--(touchpad / Back)--> Control Room                 slide
// Overlay on every screen: a "Reconnecting" pill.

#pragma once

#include <memory>
#include <vector>

#include "core/tween.hpp"
#include "screens/screen.hpp"
#include "ui/motion.hpp"

namespace hui::gfx
{
class Renderer;
}

namespace screens
{

class Shell
{
  public:
    explicit Shell(Context &context);
    ~Shell();

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands);
    void compose(gfx::Renderer &renderer, const Model &model);

  private:
    enum class Id
    {
        launch,
        home,
        playing,
        menu, // Control Room
    };
    enum class Move
    {
        none,
        iris,  // out of the launch screen
        black, // back to it
        slide, // between home and now playing
    };

    Screen &screen(Id id);
    Id wanted(const Model &model) const;
    void go(Id next, Move move, float seconds);
    void record(Frame &frame, Id id, const Model &model, float opacity, float dx, float scale);
    void draw_overlays(gfx::DrawList &list, const Model &model);

    Context &context_;
    std::unique_ptr<Screen> launch_, home_, playing_, menu_;
    bool menu_open_ = false;
    Id current_ = Id::launch;
    Id previous_ = Id::launch;
    Move move_ = Move::none;
    tween::Timer transition_;
    Frame frames_[2];
    // Separate lists: the renderer keeps a reference to each list it is
    // given until the frame is presented, so a list cannot be reused within
    // one frame.
    gfx::DrawList cover_;  // the iris / black of a move
    gfx::DrawList chrome_; // overlays
    float clock_ = 0.0f;
    std::uint32_t glass_texture_ = 0;

    tween::Spring reconnecting_;
};

} // namespace screens
