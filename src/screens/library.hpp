// ps5-spotify - Your Library: playlists, albums and artists as a card grid.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-homebrew-ui's "Paper Library" (src/concepts/paper.cpp, GPL-3.0-or-later,
// BlackBearReloaded) on Spotify's dark palette: cards keyed by item with
// their own springs, so filtering (L2 / R2) and sorting (Square) make them
// travel to their new places; the focused card lifts with a sheet turning
// out from under it; one ring glides between cards; the grid scrolls with a
// spring and dissolves at the edges it scrolls past.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_library(Context &context);

} // namespace screens
