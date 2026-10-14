// spot5 - Home: connected, nothing playing yet.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-homebrew-ui's "Aurora Shelf" hero (src/concepts/aurora.cpp): what to do
// next, in three steps, beside a floating card for this device.

#pragma once

#include <memory>

#include "screens/screen.hpp"

namespace screens
{

std::unique_ptr<Screen> make_home(Context &context);

} // namespace screens
