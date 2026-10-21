/* SPDX-License-Identifier: MIT */
/* <byteswap.h> for building the kernel's host tools on macOS (glibc has it). */
#ifndef GLASS_HOST_BYTESWAP_H
#define GLASS_HOST_BYTESWAP_H
#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
#endif
