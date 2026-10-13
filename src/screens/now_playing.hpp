// ps5-spotify - Now Playing: what the phone is playing on this console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-homebrew-ui's "Now Playing" design (src/concepts/player.cpp) on live
// data: artwork that breathes and dims when paused, title/artist/album that
// slide through on a track change, a spring scrubber, a gliding focus ring on
// previous / play / next. Its queue panel waits for queue data (wide layout).

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_now_playing(Context &context);

// True while Now Playing handles Circle itself (its queue has the focus);
// otherwise Circle leaves the screen.
bool now_playing_uses_back(const Screen &screen);

} // namespace screens
