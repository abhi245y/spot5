// ps5-spotify - App log: one line to the app log file and to klog.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// stdout is redirected to /download0/spt/app.log (runtime_shims.c), which FTP
// can read while the app runs; klog (port 3232) shows lines live.

#pragma once

namespace platform
{

void log(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace platform
