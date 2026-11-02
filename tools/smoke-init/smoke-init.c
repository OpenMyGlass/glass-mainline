/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * smoke-init: the smallest possible /init for the mainline smoke-boot image.
 *
 * It is a freestanding, statically-linked ARM EABI program: no libc, no
 * dynamic loader, no external blob and nothing to download. It is built by
 * the same clang the kernel is (scripts/build-smoke.sh), so the whole image
 * is reproducible from this tree.
 *
 * What it does, and why: the mainline kernel has never been proven to reach
 * userspace on this OMAP4430 (upstream saw "no USB, reset after about a
 * minute, no crash record", docs/reset-analysis.md). This init answers the
 * one question that matters for a first boot -- does the mainline kernel
 * reach userspace at all, and for how long before the reset -- by:
 *
 *   1. writing a banner to /dev/console (fd 1: ttyGS0 on the debug kernel,
 *      or ttyS2), so a serial/USB watcher sees it live;
 *   2. mounting devtmpfs on /dev and proc on /proc;
 *   3. opening /dev/kmsg and logging OMGB-MAINLINE: lines there. On the debug
 *      image those land in the pstore/ramoops console zone, which the stock
 *      3.4 kernel reads back as /proc/last_kmsg after the device reboots
 *      itself into Model-A Android (docs/reset-analysis.md, test T1). So the
 *      result is readable remotely over adb, with no serial wire.
 *   4. looping forever, printing a heartbeat with an incrementing counter and
 *      an elapsed-seconds estimate every 5 s. The last heartbeat in
 *      /proc/last_kmsg then tells you how long the kernel survived before the
 *      reset -- the timing evidence reset-analysis.md's test T3 wants.
 *
 * The OMGB-MAINLINE: prefix mirrors the existing Model B diagnostic images so
 * the logs are grep-able the same way. init never exits (a dying PID 1 panics
 * the kernel, which would muddy the result).
 */

/* ---- raw ARM EABI syscalls (number in r7, args r0..r4, result in r0) ---- */
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_mkdir      39
#define SYS_mount      21
#define SYS_nanosleep 162

static long sys3(long n, long a, long b, long c)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	__asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
	return r0;
}

static long sys5(long n, long a, long b, long c, long d, long e)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	__asm__ volatile("svc #0" : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4) : "memory");
	return r0;
}

/* ---- tiny helpers (no libc) ---- */
static unsigned slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}

static void wr(int fd, const char *s, unsigned n)
{
	sys3(SYS_write, fd, (long)s, n);
}

static void wrs(int fd, const char *s)
{
	wr(fd, s, slen(s));
}

/* decimal of an unsigned into buf, return length */
static unsigned utoa(unsigned v, char *buf)
{
	char tmp[12];
	unsigned i = 0, j = 0;

	if (v == 0)
		tmp[i++] = '0';
	while (v) {
		tmp[i++] = '0' + (v % 10);
		v /= 10;
	}
	while (i)
		buf[j++] = tmp[--i];
	buf[j] = 0;
	return j;
}

/* one OMGB-MAINLINE line to both /dev/kmsg (if open) and the console */
static int kmsg_fd = -1;

static void say(const char *msg)
{
	char line[256];
	const char *pfx = "OMGB-MAINLINE: ";
	unsigned i = 0, p = 0;

	while (pfx[p])
		line[i++] = pfx[p++];
	p = 0;
	while (msg[p] && i < sizeof(line) - 2)
		line[i++] = msg[p++];
	line[i++] = '\n';
	line[i] = 0;

	if (kmsg_fd >= 0)
		wr(kmsg_fd, line, i);
	wr(1, line, i);		/* /dev/console: fd 1 */
}

static void say_n(const char *msg, unsigned a, const char *msg2, unsigned b)
{
	char line[256];
	const char *pfx = "OMGB-MAINLINE: ";
	unsigned i = 0, p = 0;

	while (pfx[p])
		line[i++] = pfx[p++];
	p = 0;
	while (msg[p])
		line[i++] = msg[p++];
	i += utoa(a, line + i);
	p = 0;
	while (msg2[p])
		line[i++] = msg2[p++];
	i += utoa(b, line + i);
	line[i++] = '\n';
	line[i] = 0;

	if (kmsg_fd >= 0)
		wr(kmsg_fd, line, i);
	wr(1, line, i);
}

static void sleep5(void)
{
	/* struct timespec { long tv_sec; long tv_nsec; } -- 32-bit on ARM */
	long ts[2] = { 5, 0 };

	sys3(SYS_nanosleep, (long)ts, 0, 0);
}

int main(void)
{
	char ver[256];
	long n;

	/* Earliest possible proof of life, before anything can fail. */
	wrs(1, "\nOMGB-MAINLINE: userspace reached, smoke-init is PID 1\n");

	/* /dev (devtmpfs) gives us /dev/kmsg; /proc gives us the version. */
	sys3(SYS_mkdir, (long)"/dev", 0755, 0);
	sys3(SYS_mkdir, (long)"/proc", 0755, 0);
	sys5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	sys5(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0);

	kmsg_fd = sys3(SYS_open, (long)"/dev/kmsg", 1 /*O_WRONLY*/, 0);

	say("smoke-init up; kmsg+console logging armed");

	/* /proc/version: exactly which mainline this is, into last_kmsg. */
	n = sys3(SYS_open, (long)"/proc/version", 0 /*O_RDONLY*/, 0);
	if (n >= 0) {
		long r = sys3(SYS_read, n, (long)ver, sizeof(ver) - 1);
		if (r > 0) {
			if (ver[r - 1] == '\n')
				r--;
			ver[r] = 0;
			say(ver);
		}
		sys3(SYS_close, n, 0, 0);
	}

	say("reached userspace OK; no display is expected (panel-notle is the TODO)");
	say("heartbeat follows; the last one in last_kmsg dates the reset");

	/* Heartbeat forever. The counter and elapsed estimate let a reader time
	 * the self-reboot from /proc/last_kmsg (reset-analysis.md T3). */
	for (unsigned beat = 1;; beat++) {
		sleep5();
		say_n("alive beat=", beat, " elapsed~=", beat * 5);
	}
	return 0;	/* not reached */
}

void _start(void)
{
	__asm__ volatile("mov fp, #0\n\tmov lr, #0");
	main();
	/* If main ever returns, spin rather than exit (exit of PID 1 panics). */
	for (;;)
		;
}
