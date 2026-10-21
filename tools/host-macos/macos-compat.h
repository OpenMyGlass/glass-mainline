/* SPDX-License-Identifier: MIT */
/*
 * Force-included (-include) into the kernel's host programs on macOS by
 * scripts/build-kernel.sh: what they take from glibc and macOS lacks.
 * Only host tools see this; nothing here reaches the ARM kernel.
 */
#ifndef GLASS_MACOS_COMPAT_H
#define GLASS_MACOS_COMPAT_H
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>

/* scripts/mod/modpost.c */
static inline char *glass_strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}
#define strchrnul glass_strchrnul

/* usr/gen_init_cpio.c: fails, and the caller falls back to read/write. */
static inline ssize_t glass_copy_file_range(int in, void *inoff, int out,
					    void *outoff, size_t len,
					    unsigned int flags)
{
	(void)in; (void)inoff; (void)out; (void)outoff; (void)len; (void)flags;
	errno = ENOSYS;
	return -1;
}
#define copy_file_range glass_copy_file_range

/* usr/gen_init_cpio.c: every macOS file offset is 64 bits already. */
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#endif
