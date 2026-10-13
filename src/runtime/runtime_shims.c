// ps5-spotify - Process-level runtime shims: app log and never-return main.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trimmed from ps5-homebrew-ui src/runtime/runtime_shims.c (GPL-3.0-or-later).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

extern int sceKernelUsleep(uint32_t microseconds);

/* A title's sandbox has no /data. While the title runs, FTP reads this log at
 * /mnt/sandbox/<TITLE_ID>_000/download0/spt/app.log. */
#define SPT_DIR "/download0/spt"
#define SPT_LOG SPT_DIR "/app.log"

__attribute__((constructor)) static void spt_open_log(void)
{
    mkdir(SPT_DIR, 0755);
    rename(SPT_LOG, SPT_DIR "/app.prev.log");
    FILE *stream = freopen(SPT_LOG, "w", stdout);
    if (stream != NULL)
        setvbuf(stream, NULL, _IONBF, 0);
}

/* Returning from main or calling exit() crashes a native title. */
__attribute__((noreturn)) void catchReturnFromMain(int status)
{
    printf("[SPT] main returned status=%d\n", status);
    fflush(NULL);
    for (;;)
        sceKernelUsleep(100000);
}

__attribute__((noreturn)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    fprintf(stderr, "[SPT] assertion failed: %s (%s:%d, %s)\n", expression, file, line, function);
    abort();
}
