// ps5-homebrew-ui - Host (Linux/WSL) implementation of the system services.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/system.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

namespace hui::sys
{

// The preview's own clock: preview_main advances it by each simulated frame,
// so time read from the clock matches the script, not how fast it renders.
std::int64_t g_preview_us = 0;

std::int64_t monotonic_us()
{
    return g_preview_us;
}

void log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
}

bool hide_splash_screen()
{
    return true;
}

void sleep_us(std::uint32_t microseconds)
{
    usleep(microseconds);
}

void park()
{
    std::exit(1);
}

void quit()
{
    std::exit(0);
}

} // namespace hui::sys
