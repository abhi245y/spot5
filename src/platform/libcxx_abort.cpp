// spot5 - libc++ abort hook for the PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// libc++ calls this where a no-exceptions path cannot throw; the SDK does not
// provide it. Log and park instead of exit(), which crashes a native title.

#include <cstdarg>
#include <cstdio>

#include "platform/ps5/system.hpp"

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
    hui::sys::log("[APP] libc++ abort");
    hui::sys::park();
}
} // namespace __1
} // namespace std
