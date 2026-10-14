// spot5 - Control Room: the app's settings, opened with the touchpad.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-homebrew-ui's "Control Room" design (src/concepts/settings.cpp) with
// this app's settings: playback (quality, volume, phone volume), the device
// name (typed on the kit keyboard), the account (and forgetting it), reduce
// motion, about, and quit. Every control edits Context::settings live; the
// app saves and applies. Destructive choices ask first.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_control_room(Context &context);

// True once the user asked to leave Control Room (Back on the rail, or
// the touchpad); the shell then closes it and calls enter() the next time.
bool control_room_wants_close(const Screen &room);

} // namespace screens
