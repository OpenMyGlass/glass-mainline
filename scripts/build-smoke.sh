#!/bin/bash
# build-smoke.sh: the mainline SMOKE-BOOT image, out/mainline-smoke-boot.img.
#
# This is the first-boot attempt (OpenMyGlass). It answers one question on
# real OMAP4430 hardware: does the mainline kernel reach userspace, and for
# how long before the ~1-minute reset (docs/reset-analysis.md)? It needs no
# Alpine userland, no firmware blob and no network: a freestanding, static
# ARM init (tools/smoke-init/smoke-init.c) is built into the kernel and logs
# OMGB-MAINLINE: heartbeats to /dev/kmsg.
#
# It uses the DEBUG kernel variant (config/glass-debug.config) and the
# safe-debug DTB, so:
#   - the ramoops region is one ECC console zone laid out exactly like the
#     stock 3.4 Android RAM console, which the stock kernel reads back as
#     /proc/last_kmsg after the device reboots itself into Model-A Android.
#     So the result is readable over adb, with no serial wire.
#   - console=ttyGS0 (the legacy USB serial gadget) is forced in, so if USB
#     comes up a watcher sees the log live on /dev/ttyACM0 too.
#
# Boot it ONLY with `fastboot boot out/mainline-smoke-boot.img` (RAM, writes
# nothing; safety rule 2). It carries no MMC controller: it cannot touch the
# eMMC. Expect NO display (panel-notle is the big TODO).
#
#   KV=7.2.9 scripts/build-smoke.sh
#
# Needs bc on PATH (kbuild's timeconst.h), as the CI job and the macOS flow
# already have; on a host without it, build GNU bc and prepend it to PATH.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
work=${GLASS_WORK:-$top}
KV=${KV:-7.2.9}
out=$top/out
mkdir -p "$out"
die() { echo "build-smoke: $*" >&2; exit 1; }

# 1. The freestanding static ARM init, built by the same clang the kernel is.
CLANG=${CLANG:-clang}
TARGET=${SMOKE_TARGET:-arm-linux-gnueabihf}
isrc=$top/build/smoke-initramfs
rm -rf "$isrc"
mkdir -p "$isrc/dev" "$isrc/proc" "$isrc/sys"
"$CLANG" --target="$TARGET" -march=armv7-a -marm -O2 -static -nostdlib \
    -ffreestanding -fno-stack-protector -fno-pie -fuse-ld=lld \
    -Wl,-e,_start -Wl,--build-id=none \
    -o "$isrc/init" "$top/tools/smoke-init/smoke-init.c"
command -v llvm-strip >/dev/null && llvm-strip "$isrc/init" || true
echo "build-smoke: init $(stat -c %s "$isrc/init") bytes ($("$CLANG" --version | head -1))"

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
#    initramfs is inside the kernel; the bootloader's ramdisk address
#    0x81000000 is overrun by the ~15 MB decompressed kernel). The header
#    offsets are Glass's stock layout; the cmdline is build-images.sh's
#    $mcmd, and the debug kernel's CONFIG_CMDLINE_EXTEND appends
#    console=ttyGS0 and the ignore_unused flags.
: | cpio -o -H newc --quiet 2>/dev/null | gzip -9n > "$out/empty-ramdisk.gz"
cat "$z" "$d" > "$out/zImage-dtb-smoke"
hdr=(--base 0x80000000 --kernel-offset 0x00008000 --ramdisk-offset 0x01000000
     --second-offset 0x00f00000 --tags-offset 0x00000100 --pagesize 2048)
mcmd="console=ttyS2,115200n8 earlycon panic=10 rdinit=/init loglevel=7 clk_ignore_unused regulator_ignore_unused pd_ignore_unused fbcon=font:VGA8x16"
"$bi" pack "$out/mainline-smoke-boot.img" "$out/zImage-dtb-smoke" "$out/empty-ramdisk.gz" \
    "${hdr[@]}" --cmdline "$mcmd" >/dev/null

sz=$(stat -c %s "$out/mainline-smoke-boot.img")
LIMIT=8388608
[ "$sz" -le "$LIMIT" ] || echo "build-smoke: WARNING image is $sz bytes, over the $LIMIT-byte partition bound (fastboot boot is RAM, so this is only a note)"
( cd "$out" && sha256sum mainline-smoke-boot.img )
echo "build-smoke: out/mainline-smoke-boot.img $sz bytes; boot with: fastboot boot out/mainline-smoke-boot.img"
