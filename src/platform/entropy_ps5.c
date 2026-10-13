// ps5-spotify - mbedTLS entropy source: the kernel CSPRNG via sysctl.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// kern.arandom (KERN_ARND) is what the SDK's own arc4random uses. It lives in
// libkernel, which every title loads; sceRandomGetRandomNumber does not work
// here because a native title does not load libSceRandom (the call jumped to
// address 0 on the console).

#include <stddef.h>
#include <sys/sysctl.h>

// The kernel returns at most 256 bytes per call.
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    int mib[2] = {CTL_KERN, KERN_ARND};
    *olen = 0;
    while (*olen < len)
    {
        size_t n = len - *olen < 256 ? len - *olen : 256;
        if (sysctl(mib, 2, output + *olen, &n, NULL, 0) != 0 || n == 0)
            return -1; // mbedTLS turns this into a TLS error, not a crash
        *olen += n;
    }
    return 0;
}
