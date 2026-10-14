// spot5 - Spotify Connect for the PS5, without PSN.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

int main()
{
    app::App app;
    app.run(); // never returns: the system closes the app (Close app)
}
