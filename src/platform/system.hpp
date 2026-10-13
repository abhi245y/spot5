// ps5-spotify - Small PS5 system services the app needs.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace platform
{

void sleep_ms(std::uint32_t milliseconds);

// Never returns. A native title must not return from main or call exit()
// (both end in the crash reporter); it waits for the system to close it.
[[noreturn]] void park();

} // namespace platform
