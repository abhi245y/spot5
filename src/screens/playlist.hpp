// ps5-spotify - A playlist/album page: its cover, its story and every track.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apple Music's (tvOS) layout: the cover on the left; title, owner line,
// description, actions and the track list on the right. Spotify's touches:
// the round green Play, numbered rows, green bars on the playing track, the
// "owner · N songs · length" line. Ours (ps5-homebrew-ui's Aurora): the
// backdrop takes the cover's colours, one focus pill glides between the
// actions and the rows, the cover breathes while this playlist plays.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_playlist(Context &context);

} // namespace screens
