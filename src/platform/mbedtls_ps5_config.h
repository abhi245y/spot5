// spot5 - mbedTLS settings for the PS5 (passed as MBEDTLS_USER_CONFIG_FILE).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// No /dev/urandom in a title's sandbox: entropy comes from
// the kernel CSPRNG (sysctl KERN_ARND) through mbedtls_hardware_poll (entropy_ps5.c).
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
// Directory/file loading of certs pulls opendir/readdir, which only the
// WebKit library (not loaded by a native title) provides. Roots come from
// bell X509Bundle instead.
#undef MBEDTLS_FS_IO
// Persistent PSA key storage sits on FS_IO; nothing here stores keys.
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
