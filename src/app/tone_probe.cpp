// ps5-spotify - Background-audio probe: run as a separate Daemon title.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ponytail: an experiment, not a feature. When the title holds the file
// /app0/daemon-probe, main() runs this instead of the app: it loops six
// routes (port type x user), each a 3 s tone at its own pitch and 2 s of
// silence, and logs every result, so a game can be started on top of it to
// hear which route the console lets through.

#include <cmath>
#include <cstdint>
#include <unistd.h>

#include "platform/ps5/system.hpp"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format);
    int sceAudioOutOutput(int handle, const void *data);
    int sceAudioOutClose(int handle);
    int sceAudioOutSetVolume(int handle, int flag, const int *volumes);
    int sceUserServiceInitialize(void *params);
    int sceUserServiceGetForegroundUser(int *user);
}

namespace app
{
namespace
{

constexpr unsigned kGrain = 256;
constexpr unsigned kRate = 48000;

// Seconds of a tone (hz > 0) or silence; 0, or the failing result.
int play(int handle, double seconds, double hz)
{
    static std::int16_t block[kGrain * 2];
    static double phase = 0;
    const int blocks = static_cast<int>(seconds * kRate / kGrain);
    for (int b = 0; b < blocks; ++b)
    {
        for (unsigned i = 0; i < kGrain; ++i)
        {
            const auto v = hz > 0 ? static_cast<std::int16_t>(8000 * std::sin(phase)) : std::int16_t{0};
            phase += 2 * M_PI * hz / kRate;
            block[2 * i] = block[2 * i + 1] = v;
        }
        if (const int r = sceAudioOutOutput(handle, block); r < 0)
            return r;
    }
    return 0;
}

} // namespace

int tone_probe()
{
    hui::sys::log("[DAEMON] tone probe: pid %d, audio init 0x%08x", getpid(), static_cast<unsigned>(sceAudioOutInit()));
    int user = -1;
    sceUserServiceInitialize(nullptr);
    sceUserServiceGetForegroundUser(&user);
    struct Route
    {
        const char *name;
        int user, type;
        double hz;
    };
    const Route routes[] = {{"MAIN system", 0xff, 0, 330},       {"BGM system", 0xff, 1, 440},
                            {"MAIN foreground", user, 0, 550},   {"BGM foreground", user, 1, 660},
                            {"PADSPK foreground", user, 4, 880}, {"AUX system", 0xff, 127, 990}};
    static const int full[8] = {32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768};
    for (int cycle = 1;; ++cycle)
        for (int k = 0; k < 6; ++k)
        {
            const Route &route = routes[k];
            const int handle = sceAudioOutOpen(route.user, route.type, 0, kGrain, kRate, 1);
            if (handle < 0)
            {
                hui::sys::log("[DAEMON] cycle %d route %d (%s, %d Hz): open 0x%08x", cycle, k + 1, route.name,
                              static_cast<int>(route.hz), static_cast<unsigned>(handle));
                sleep(1);
                continue;
            }
            sceAudioOutSetVolume(handle, 0xff, full);
            int r = play(handle, 3.0, route.hz);
            if (r == 0)
                r = play(handle, 2.0, 0);
            hui::sys::log("[DAEMON] cycle %d route %d (%s, %d Hz): played, output 0x%08x", cycle, k + 1, route.name,
                          static_cast<int>(route.hz), static_cast<unsigned>(r));
            sceAudioOutClose(handle);
        }
}

} // namespace app
