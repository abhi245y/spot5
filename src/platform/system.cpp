// ps5-spotify - Small PS5 system services the app needs.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/system.hpp"

#include <cstdarg>
#include <cstdio>

#include "platform/log.hpp"

extern "C" int sceKernelUsleep(unsigned int microseconds);

namespace platform
{

void sleep_ms(std::uint32_t milliseconds)
{
    sceKernelUsleep(milliseconds * 1000);
}

void park()
{
    std::fflush(nullptr);
    for (;;)
        sceKernelUsleep(100000);
}

} // namespace platform

// libc++ calls this where a no-exceptions path cannot throw; the SDK does not
// provide it. Log and park instead of exit(), which crashes a native title.
namespace std
{
inline namespace __1
{
[[noreturn]] void __libcpp_verbose_abort(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    platform::log("[APP] libc++ abort");
    platform::park();
}
} // namespace __1
} // namespace std
