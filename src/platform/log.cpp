// ps5-spotify - App log: one line to the app log file and to klog.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" int sceKernelDebugOutText(int channel, const char *text);

namespace platform
{

void log(const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    std::strcat(line, "\n");
    std::fputs(line, stdout);
    sceKernelDebugOutText(0, line);
}

} // namespace platform
