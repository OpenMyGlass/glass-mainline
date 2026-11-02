/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * smoke-init: the smallest possible /init for the mainline smoke-boot image.
 *
 * It is a freestanding, statically-linked ARM EABI program: no libc, no
 * dynamic loader, no external blob and nothing to download. It is built by
 * the same clang the kernel is (scripts/build-smoke.sh / build-smoke2.sh), so
 * the whole image is reproducible from this tree.
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
 *   4. a DISPLAY HANDOFF: it re-points the running DISPC's graphics pipe at
 *      the device tree's simple-framebuffer (0x9c700000) over /dev/mem and
 *      paints an eight-bar colour test pattern there, so that IF the boot
 *      survives, the prism shows real, unmistakable pixels -- the first
 *      sign of life a future boot would have without a serial wire. This
 *      mirrors the busybox display_handoff in userland/initramfs/init, using
 *      the same, hardware-proven DISPC register addresses (the Model-A
 *      glass-fb flips frames through these on real hardware).
 *   5. looping forever, printing a heartbeat with an incrementing counter and
 *      an elapsed-seconds estimate. The cadence is DENSE early -- one beat a
 *      second for the first two minutes -- so the last beat in
 *      /proc/last_kmsg dates the ~1-minute reset to the second (and the
 *      kernel's own CONFIG_PRINTK_TIME stamp on every line confirms it).
 *
 * The OMGB-MAINLINE: prefix mirrors the existing Model B diagnostic images so
 * the logs are grep-able the same way. init never exits (a dying PID 1 panics
 * the kernel, which would muddy the result).
 */

/* ---- raw ARM EABI syscalls (number in r7, args r0..r5, result in r0) ---- */
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_mkdir      39
#define SYS_mount      21
#define SYS_nanosleep 162
#define SYS_mmap2     192

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

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	__asm__ volatile("svc #0" : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
			 : "memory");
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

/* lower-case hex of an unsigned (0x-prefixed) into buf, return length */
static unsigned xtoa(unsigned v, char *buf)
{
	const char *hex = "0123456789abcdef";
	char tmp[8];
	unsigned i = 0, j = 0;

	buf[j++] = '0';
	buf[j++] = 'x';
	if (v == 0)
		tmp[i++] = '0';
	while (v) {
		tmp[i++] = hex[v & 0xf];
		v >>= 4;
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

/* one OMGB-MAINLINE line "msg<hex a> msg2<hex b>" (for register values) */
static void say_x(const char *msg, unsigned a, const char *msg2, unsigned b)
{
	char line[256];
	const char *pfx = "OMGB-MAINLINE: ";
	unsigned i = 0, p = 0;

	while (pfx[p])
		line[i++] = pfx[p++];
	p = 0;
	while (msg[p])
		line[i++] = msg[p++];
	i += xtoa(a, line + i);
	p = 0;
	while (msg2[p])
		line[i++] = msg2[p++];
	i += xtoa(b, line + i);
	line[i++] = '\n';
	line[i] = 0;

	if (kmsg_fd >= 0)
		wr(kmsg_fd, line, i);
	wr(1, line, i);
}

static void sleep_s(long secs)
{
	/* struct timespec { long tv_sec; long tv_nsec; } -- 32-bit on ARM */
	long ts[2] = { secs, 0 };

	sys3(SYS_nanosleep, (long)ts, 0, 0);
}

/*
 * The display handoff (docs/mainline-bringup.md "Interim stepping stone").
 *
 * The bootloader programmed the iCE40 FPGA and left the DISPC scanning its
 * graphics pipe from 0x80500000, which the decompressed mainline kernel now
 * occupies -- so the prism currently shows kernel memory. We cannot write a
 * panel driver here, but we can REUSE the running DISPC: point GFX_BA0/BA1 at
 * the device tree's reserved simple-framebuffer (0x9c700000, no-map RAM), set
 * the pixel format to xRGB (no alpha) and latch it with GOLCD2. Then whatever
 * we paint into 0x9c700000 appears over the eye.
 *
 * Addresses are the hardware-proven ones from the Model-A userland
 * (userland/initramfs/init display_handoff, scripts/.../glass-collect,
 * glass-fb): CM_DSS_DSS_CLKCTRL 0x4a009120, DISPC GFX_BA0 0x48041080, GFX_BA1
 * 0x48041084, GFX_ATTRIBUTES 0x480410a0, DISPC_CONTROL2 0x48041238 (GOLCD2 =
 * bit 5). Reading a GATED DISPC faults, so we first check CM_DSS: the module
 * is functional only when IDLEST (bits 17:16) == 0; otherwise we leave the
 * DISPC alone (but still paint the RAM, which is harmless).
 *
 * STRICT_DEVMEM is off on 32-bit ARM (lib/Kconfig.debug: default y only on
 * PPC/X86/ARM64/S390), so /dev/mem reaches both the device registers and the
 * reserved framebuffer RAM.
 */
#define FB_PHYS   0x9c700000u
#define FB_W      640u
#define FB_H      360u
#define FB_BYTES  (FB_W * FB_H * 4u)		/* 921600 = 225 pages exactly */

static void paint_pattern(volatile unsigned *fb)
{
	/* Eight vertical SMPTE-style bars, 80 px each, in xRGB (x8r8g8b8). */
	static const unsigned bar[8] = {
		0x00ffffffu, /* white   */
		0x00ffff00u, /* yellow  */
		0x0000ffffu, /* cyan    */
		0x0000ff00u, /* green   */
		0x00ff00ffu, /* magenta */
		0x00ff0000u, /* red     */
		0x000000ffu, /* blue    */
		0x00000000u, /* black   */
	};
	unsigned x, y;

	for (y = 0; y < FB_H; y++) {
		volatile unsigned *row = fb + (unsigned long)y * FB_W;
		for (x = 0; x < FB_W; x++)
			row[x] = bar[x / 80u];
	}
}

static int bad_map(long r)
{
	return (unsigned long)r >= (unsigned long)-4096;
}

static void display_handoff(void)
{
	long memfd;
	volatile unsigned *cm, *dispc, *fb;
	unsigned clkctrl, attr, ctrl2;

	say("display handoff: starting (/dev/mem, DISPC -> 0x9c700000)");

	memfd = sys3(SYS_open, (long)"/dev/mem", 2 /*O_RDWR*/, 0);
	if (memfd < 0) {
		say("display handoff: no /dev/mem, skipped");
		return;
	}

	/* The reserved framebuffer RAM: paint it first (always safe). */
	fb = (volatile unsigned *)sys6(SYS_mmap2, 0, FB_BYTES, 3 /*RW*/,
				       1 /*MAP_SHARED*/, memfd, FB_PHYS >> 12);
	if (bad_map((long)fb)) {
		say("display handoff: framebuffer mmap failed, skipped");
		sys3(SYS_close, memfd, 0, 0);
		return;
	}
	paint_pattern(fb);
	say("display handoff: test pattern painted at 0x9c700000");

	/* Is the DSS clocked? Reading a gated DISPC would fault. */
	cm = (volatile unsigned *)sys6(SYS_mmap2, 0, 0x1000, 3, 1,
				       memfd, 0x4a009000u >> 12);
	if (bad_map((long)cm)) {
		say("display handoff: CM_DSS mmap failed; DISPC left alone");
		sys3(SYS_close, memfd, 0, 0);
		return;
	}
	clkctrl = cm[0x120 / 4];
	say_x("display handoff: CM_DSS_DSS_CLKCTRL=", clkctrl, "", 0);
	if (((clkctrl >> 16) & 3u) != 0u) {
		say("display handoff: DSS not functional, DISPC left alone");
		sys3(SYS_close, memfd, 0, 0);
		return;
	}

	/* Re-point the graphics pipe and latch it (GOLCD2). */
	dispc = (volatile unsigned *)sys6(SYS_mmap2, 0, 0x1000, 3, 1,
					  memfd, 0x48041000u >> 12);
	if (bad_map((long)dispc)) {
		say("display handoff: DISPC mmap failed; left alone");
		sys3(SYS_close, memfd, 0, 0);
		return;
	}
	attr  = dispc[0xa0 / 4];
	ctrl2 = dispc[0x238 / 4];
	dispc[0x80 / 4] = FB_PHYS;			/* GFX_BA0 */
	dispc[0x84 / 4] = FB_PHYS;			/* GFX_BA1 */
	dispc[0xa0 / 4] = (attr & ~0x1eu) | (0x8u << 1);/* GFXFORMAT = xRGB */
	dispc[0x238 / 4] = ctrl2 | 0x20u;		/* GOLCD2 latch */
	say_x("display handoff: latched; was GFX_ATTR=", attr,
	      " CONTROL2=", ctrl2);
	say_x("display handoff: GFX_BA0 now=", dispc[0x80 / 4], "", 0);

	sys3(SYS_close, memfd, 0, 0);
}

int main(void)
{
	char ver[256];
	long n;
	unsigned secs = 0;

	/* Earliest possible proof of life, before anything can fail. */
	wrs(1, "\nOMGB-MAINLINE: userspace reached, smoke-init is PID 1\n");

	/* /dev (devtmpfs) gives us /dev/kmsg and /dev/mem; /proc the version. */
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

	say("reached userspace OK; attempting interim display handoff");

	/* Paint the prism via the running DISPC, if it survives to here. */
	display_handoff();

	say("heartbeat follows (1 s for 120 s, then 5 s); the last one in last_kmsg dates the reset");

	/*
	 * Heartbeat forever. Dense early (1 s) so the self-reboot is dated to
	 * the second across the whole ~1-minute reset window; CONFIG_PRINTK_TIME
	 * adds the kernel's own stamp to every line (reset-analysis.md T3).
	 */
	for (unsigned beat = 1;; beat++) {
		if (beat <= 120) {
			sleep_s(1);
			secs += 1;
		} else {
			sleep_s(5);
			secs += 5;
		}
		say_n("alive beat=", beat, " elapsed~=", secs);
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
