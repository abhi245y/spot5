// ps5-spotify - The app: display, input and drawing on the main thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws with ps5-homebrew-ui's kit at 60 fps and reads the controller; the
// Spotify side runs on app::Controller's thread. Quits through the system
// (Options -> Quit), or the user closes it from the PS menu (Close app).

#pragma once

namespace app
{

class App
{
  public:
    [[noreturn]] void run();
};

} // namespace app
