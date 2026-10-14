// spot5 - Launch screen: title card and an honest loader.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// From ps5-homebrew-ui's "Launch Sequence" design (src/concepts/boot.cpp):
// the layered, banded wordmark with a light sweep, and a loader whose stages
// are the app's real connection steps (Model::step), not a timer. A failed
// connection turns the loader into the error, with retry on confirm.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_launch(Context &context);

// True once the loader has shown "ready" long enough to hand over.
bool launch_finished(const Screen &launch);

} // namespace screens
