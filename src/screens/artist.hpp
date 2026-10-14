// spot5 - An artist: the landscape header, Popular, Albums, Singles.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The artist's wide header image (Spotify's landscape banner, 1920x1080)
// full-bleed across the top and fading into the backdrop; the name, monthly
// listeners and Spotify's green Play; then Popular (Cross plays from that
// song), the biography, and shelves of Albums and Singles (Cross opens their
// page). No header? The round photo stands at the right on the photo's colours.
// The page scrolls with a spring to keep the focus in view.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_artist(Context &context);

} // namespace screens
