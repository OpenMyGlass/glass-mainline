#!/bin/bash
# build-kernel.sh: the mainline kernel for Glass (zImage, modules, and the
# Glass DTBs from dts/), built with LLVM (clang, lld) with Python kept out:
#   tools/nopython/ (shims that log and fail) is first on PATH, kbuild gets
#   PYTHON3=false, and on Linux the build runs in a bubblewrap sandbox where
#   every real python binary under /usr/bin is covered by /dev/null.
# out/python-calls.log must stay empty; the script fails if it does not.
# Then every option of the config fragments is checked against .config.
#
#   KV=7.2.9 JOBS=4 scripts/build-kernel.sh [make targets]
#
# Environment:
#   GLASS_VARIANT  default (config/glass.config) or debug (glass.config,
#                  then config/glass-debug.config). The debug variant builds
#                  into build/kernel-$KV-debug; it is opt-in and leaves the
#                  default build alone.
#   GLASS_SANDBOX  bwrap (the default on Linux) or none. none still has the
#                  nopython shims and PYTHON3=false, but no masking. macOS
#                  has no bubblewrap, so there it is always none.
#   GLASS_WORK     where src/ and build/ are (default: the repository). On
#                  macOS it must be a case-sensitive volume (see
#                  scripts/fetch-kernel.sh and docs/porting-log.md).
#   GLASS_INITRAMFS  a directory from scripts/build-userland.sh to build in.
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
work=${GLASS_WORK:-$top}
KV=${KV:-7.2.9}
variant=${GLASS_VARIANT:-default}
os=$(uname -s)
die() { echo "build-kernel: $*" >&2; exit 1; }

frags=("$top/config/glass.config")
case "$variant" in
    default) suffix="" ;;
    debug) suffix=-debug; frags+=("$top/config/glass-debug.config") ;;
    *) die "GLASS_VARIANT is default or debug, not $variant" ;;
esac
src=$work/src/linux-$KV
out=$work/build/kernel-$KV$suffix
log=$top/out/python-calls.log
mkdir -p "$out" "$top/out"
[ -d "$src" ] || die "no $src (run scripts/fetch-kernel.sh, or scripts/fetch.sh)"

sandbox=${GLASS_SANDBOX:-bwrap}
[ "$os" = Darwin ] && sandbox=none
case "$sandbox" in bwrap | none) ;; *) die "GLASS_SANDBOX is bwrap or none" ;; esac

# The build itself, run directly or inside the sandbox (this same script,
# called again with GLASS_IN_SANDBOX=1).
build() {
    local hostcflags="" M merged bad=0 l k
    if [ "$os" = Darwin ]; then
        # GNU make, sed, find, coreutils, awk, bison and flex from Homebrew
        # before the BSD ones; Homebrew's LLVM and lld for LLVM=1.
        local b
        b=$(brew --prefix)
        PATH=$b/opt/make/libexec/gnubin:$b/opt/gnu-sed/libexec/gnubin:$b/opt/findutils/libexec/gnubin:$b/opt/coreutils/libexec/gnubin:$b/opt/gawk/libexec/gnubin:$b/opt/bison/bin:$b/opt/flex/bin:$b/opt/llvm/bin:$b/opt/lld/bin:$PATH
        export PATH
        [ -f "$work/build/host-macos/elf.h" ] || die "no $work/build/host-macos/elf.h (run scripts/fetch-kernel.sh)"
        # musl's <elf.h>, and tools/host-macos for <byteswap.h> and the
        # few glibc functions the host tools use.
        hostcflags="-I$top/tools/host-macos -I$work/build/host-macos -include $top/tools/host-macos/macos-compat.h -D_UUID_T -D__GETHOSTUUID_H"
    fi
    M=(make -C "$src" O="$out" ARCH=arm LLVM=1 PYTHON3=false ${hostcflags:+HOSTCFLAGS="$hostcflags"})

    # The initramfs built into the kernel (GLASS_INITRAMFS, a directory from
    # scripts/build-userland.sh): kbuild maps the build user's files to root.
    local frag2=()
    if [ -n "${GLASS_INITRAMFS:-}" ]; then
        frag2=("$top/out/initramfs$suffix.config")
        cat > "${frag2[0]}" <<FRAG
CONFIG_INITRAMFS_SOURCE="$GLASS_INITRAMFS $top/userland/initramfs.list"
CONFIG_INITRAMFS_ROOT_UID=$(id -u)
CONFIG_INITRAMFS_ROOT_GID=$(id -g)
CONFIG_INITRAMFS_COMPRESSION_XZ=y
FRAG
    fi
    # The Glass device trees, built by kbuild from the kernel's own tree.
    cp "$top"/dts/*.dts "$top"/dts/*.dtsi "$src/arch/arm/boot/dts/ti/omap/"

    [ -f "$out/.config" ] || "${M[@]}" omap2plus_defconfig
    bash "$src/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" "${frags[@]}" ${frag2[@]+"${frag2[@]}"} > /dev/null
    "${M[@]}" olddefconfig > /dev/null

    # Every option the fragments set, as it ended up; when two fragments set
    # the same option, the later one is what must hold.
    merged=$(awk '
        /^CONFIG_[A-Za-z0-9_]+=/ { k = $0; sub(/=.*/, "", k); v[k] = $0; if (!(k in o)) { o[k] = n++ } }
        /^# CONFIG_[A-Za-z0-9_]+ is not set/ { k = $2; v[k] = $0; if (!(k in o)) { o[k] = n++ } }
        END { for (k in o) print o[k] "\t" v[k] }' "${frags[@]}" | sort -n | cut -f2-)
    while IFS= read -r l; do
        case "$l" in
            CONFIG_*=*)
                k=${l%%=*}
                grep -qxF "$l" "$out/.config" ||
                    { echo "config: $l wanted, got: $(grep -E "^$k=|^# $k is not set" "$out/.config" || echo absent)"; bad=1; } ;;
            "# CONFIG_"*" is not set")
                k=${l#\# }; k=${k%% *}
                grep -qE "^$k=" "$out/.config" && { echo "config: $k should be unset"; bad=1; } ;;
        esac
    done <<< "$merged"
    [ $bad = 0 ] || die "a fragment option did not apply ($variant)"
    echo "config: every fragment option applied ($variant)"

    local nice=()
    command -v chrt > /dev/null && nice+=(chrt -i 0)
    nice+=(nice -n 19)
    command -v ionice > /dev/null && nice+=(ionice -c 3)
    "${nice[@]}" "${M[@]}" -j"${JOBS:-4}" "${targets[@]}"
}

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(zImage modules)

if [ "${GLASS_IN_SANDBOX:-0}" = 1 ] || [ "$sandbox" = none ]; then
    if [ "${GLASS_IN_SANDBOX:-0}" != 1 ]; then
        : > "$log"
        export PATH="$top/tools/nopython:$PATH" NOPYTHON_LOG="$log"
    fi
    build
    [ "${GLASS_IN_SANDBOX:-0}" = 1 ] && exit 0
else
    : > "$log"
    masks=()
    for p in /usr/bin/python*; do
        [ -f "$p" ] && [ ! -L "$p" ] && masks+=(--ro-bind /dev/null "$p")
    done
    for d in /usr/lib/python3*; do [ -d "$d" ] && masks+=(--tmpfs "$d"); done
    bwrap --dev-bind / / ${masks[@]+"${masks[@]}"} \
        --setenv PATH "$top/tools/nopython:$PATH" --setenv NOPYTHON_LOG "$log" \
        --setenv GLASS_IN_SANDBOX 1 \
        -- "$top/scripts/build-kernel.sh" "${targets[@]}"
fi
if [ -s "$log" ]; then
    echo "build-kernel: PYTHON WAS CALLED:" >&2
    cat "$log" >&2
    exit 3
fi
echo "build-kernel: no Python called ($log empty; sandbox: $sandbox)"
ls -la "$out/arch/arm/boot/zImage" 2> /dev/null || true
