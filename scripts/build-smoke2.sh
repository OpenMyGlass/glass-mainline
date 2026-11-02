#!/bin/bash
# build-smoke2.sh: the SECOND smoke-boot candidate, out/mainline-smoke2-boot.img.
#
# The first smoke image (scripts/build-smoke.sh) proved nothing yet on
# hardware; this candidate carries the boot-survival and display-interim work
# of the OpenMyGlass "boot survival" pass. It is built exactly like
# build-smoke.sh -- the DEBUG kernel variant (config/glass-debug.config) with
# the freestanding static ARM init (tools/smoke-init) built in, the safe-debug
# DTB, packed into an AOSP boot.img for `fastboot boot` -- and differs only in
# what the shared sources now contain:
#
#   1. Boot survival (docs/reset-analysis.md hypothesis H3). The four rails
#      3.4 keeps always on -- VAUX2, VAUX3, VUSIM, VANA -- are now described
#      regulator-always-on in dts/omap4-glass-xec-common.dtsi, so mainline's
#      late "disable unused regulators" pass (~30-35 s, inside the reset
#      window) cannot drop them even if the bootloader eats the
#      regulator_ignore_unused flag off the command line (D8). This is the one
#      host-side reset fix available; the OMAP WDT2 (H1) is already disabled by
#      omap_wdt at probe, so it is exonerated the moment a heartbeat appears.
#
#   2. Dense heartbeats. smoke-init logs OMGB-MAINLINE: alive beat=N once a
#      SECOND for the first 120 s (then every 5 s), so the last beat in
#      /proc/last_kmsg dates the reset to the second across the whole ~1-minute
#      window. CONFIG_PRINTK_TIME stamps every line with the kernel clock too.
#
#   3. Interim display (docs/mainline-bringup.md). If the boot survives,
#      smoke-init re-points the running DISPC's graphics pipe at the device
#      tree's simple-framebuffer (0x9c700000) over /dev/mem and paints an
#      eight-bar colour test pattern, so a surviving boot shows real pixels on
#      the prism -- the first unblinded sign of life without a serial wire.
#
# Boot it ONLY with `fastboot boot out/mainline-smoke2-boot.img` (RAM, writes
# nothing; safety rule 2). It carries no MMC controller: it cannot touch the
# eMMC. Build:
#
#   KV=7.2.9 scripts/build-smoke2.sh
#
# Needs bc on PATH (kbuild's timeconst.h), as the CI job already has.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
work=${GLASS_WORK:-$top}
KV=${KV:-7.2.9}
out=$top/out
mkdir -p "$out"
die() { echo "build-smoke2: $*" >&2; exit 1; }

# 1. The freestanding static ARM init, built by the same clang the kernel is.
CLANG=${CLANG:-clang}
TARGET=${SMOKE_TARGET:-arm-linux-gnueabihf}
isrc=$top/build/smoke2-initramfs
rm -rf "$isrc"
mkdir -p "$isrc/dev" "$isrc/proc" "$isrc/sys"
"$CLANG" --target="$TARGET" -march=armv7-a -marm -O2 -static -nostdlib \
    -ffreestanding -fno-stack-protector -fno-pie -fuse-ld=lld \
    -Wl,-e,_start -Wl,--build-id=none \
    -o "$isrc/init" "$top/tools/smoke-init/smoke-init.c"
command -v llvm-strip >/dev/null && llvm-strip "$isrc/init" || true
echo "build-smoke2: init $(stat -c %s "$isrc/init") bytes ($("$CLANG" --version | head -1))"

# 2. The DEBUG kernel with that initramfs built in, and the safe-debug DTB.
GLASS_VARIANT=debug GLASS_INITRAMFS="$isrc" GLASS_WORK="$work" KV="$KV" \
    "$top/scripts/build-kernel.sh" zImage ti/omap/omap4-glass-xec-safe-debug.dtb

k=$work/build/kernel-$KV-debug
z=$k/arch/arm/boot/zImage
d=$k/arch/arm/boot/dts/ti/omap/omap4-glass-xec-safe-debug.dtb
[ -f "$z" ] || die "no $z"
[ -f "$d" ] || die "no $d"
grep -q '^CONFIG_INITRAMFS_SOURCE="..*"' "$k/.config" || die "kernel has no built-in initramfs"

# 3. bootimg (AOSP boot.img, header v0). tcc if present, else any cc.
bi=$top/tools/bootimg/bootimg
if [ ! -x "$bi" ]; then
    cc=$(command -v tcc || command -v cc || command -v gcc) || die "no C compiler for bootimg"
    "$cc" -O2 -o "$bi" "$top/tools/bootimg/bootimg.c"
fi

# 4. Pack: zImage + appended safe-debug DTB, an empty external ramdisk (the
#    initramfs is inside the kernel). Header offsets are Glass's stock layout;
#    the cmdline is build-images.sh's $mcmd, and the debug kernel's
#    CONFIG_CMDLINE_EXTEND appends console=ttyGS0 and the ignore_unused flags.
: | cpio -o -H newc --quiet 2>/dev/null | gzip -9n > "$out/empty-ramdisk.gz"
cat "$z" "$d" > "$out/zImage-dtb-smoke2"
hdr=(--base 0x80000000 --kernel-offset 0x00008000 --ramdisk-offset 0x01000000
     --second-offset 0x00f00000 --tags-offset 0x00000100 --pagesize 2048)
mcmd="console=ttyS2,115200n8 earlycon panic=10 rdinit=/init loglevel=7 clk_ignore_unused regulator_ignore_unused pd_ignore_unused fbcon=font:VGA8x16"
"$bi" pack "$out/mainline-smoke2-boot.img" "$out/zImage-dtb-smoke2" "$out/empty-ramdisk.gz" \
    "${hdr[@]}" --cmdline "$mcmd" >/dev/null

sz=$(stat -c %s "$out/mainline-smoke2-boot.img")
LIMIT=8388608
[ "$sz" -le "$LIMIT" ] || die "image is $sz bytes, over the $LIMIT-byte partition bound"
( cd "$out" && sha256sum mainline-smoke2-boot.img )
echo "build-smoke2: out/mainline-smoke2-boot.img $sz bytes ($((LIMIT - sz)) under the 8 MiB bound)"
echo "build-smoke2: boot with: fastboot boot out/mainline-smoke2-boot.img"
