// ps5-spotify - Spotify Connect for the PS5, without PSN.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <unistd.h>

#include "app/app.hpp"

namespace app
{
int tone_probe(); // app/tone_probe.cpp
}

int main()
{
    if (access("/app0/daemon-probe", F_OK) == 0)
        return app::tone_probe(); // the background-audio experiment, run as its own title
    app::App app;
    app.run(); // never returns: the system closes the app (Close app)
}
