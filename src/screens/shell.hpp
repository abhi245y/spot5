// spot5 - Chooses the screen, moves between screens, owns the overlays.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   launch  --(ready, loader done)--> home                  iris
//   home  <--(L1 / R1)--> library                             slide
//   home, library <--(Cross on a card / Circle)--> page or artist (stacked:
//   artist -> album page -> Circle -> artist)                  slide
//   any of those <--(Triangle / Circle)--> now playing          slide
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
        page,    // a playlist/album opened from Home, Library or an artist
        library, // the Library tab
        artist,  // an artist opened from Home or Library
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
    std::unique_ptr<Screen> launch_, home_, playing_, menu_, page_, library_, artist_;
    bool menu_open_ = false;
    bool playing_open_ = false; // Triangle on Home or a page opens Now Playing; Circle closes it
    bool library_tab_ = false;  // L1 / R1 between Home and Library
    std::vector<Id> stack_;     // pages opened over the tab (page / artist), newest last
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

    // Top bar: the now-playing bar (Home and pages) and one status pill
    // (busy action, reconnecting, updating Home). On a list's page the bar
    // docks under the cover (dock_ 1) and flies back to the corner elsewhere.
    tween::Spring mini_;
    tween::Spring dock_;
    tween::Spring status_shown_;
    tween::Spring status_width_;
    std::string status_text_, status_previous_;
    tween::Timer status_swap_;
    // The touchpad: a tap opens / closes Control Room; holding it quits.
    float touch_held_ = 0.0f;    // seconds the touchpad has been down
    float quit_progress_ = 0.0f; // 0..1 of the hold, kept while the ring fades
    bool quit_sent_ = false;
    tween::Spring quit_shown_;
    // Model::notice, shown once on whatever screen is up.
    std::uint32_t notice_id_ = 0;
    std::string notice_;
    float notice_age_ = 100.0f;
};

} // namespace screens
