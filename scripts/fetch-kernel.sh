#!/bin/bash
# fetch-kernel.sh: only what the kernel build needs, downloaded and verified,
# then unpacked. It is the kernel-only part of scripts/fetch.sh, so CI and a
# Mac can build without Google's images, Alpine or firmware.
#   linux-$KV.tar.xz: its sha256 must match kernel.org's sha256sums.asc, and
#     that file's signature must be good and made by the Kernel.org checksum
#     autosigner, fingerprint B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1 (the
#     same key scripts/fetch.sh checks). For 7.2.9 the sha256 is also pinned
#     below.
#   On macOS only: musl 1.2.5, signed by musl's release key
#     836489290BB6B70F99FFDA0556BCDB593020450F, with its sha256 pinned. Only its
#     <elf.h> (MIT licence) is used: macOS has none, and kbuild's host tools
#     (modpost, sorttable) include it.
# Everything goes under GLASS_WORK (default: the repository): dl/, src/,
# build/host-macos/. The kernel tree has paths that differ only in case, so
# on macOS GLASS_WORK must be on a case-sensitive volume (docs/porting-log.md
# says how to make one); the script refuses otherwise.
#   KV=7.2.9 GLASS_WORK=/path scripts/fetch-kernel.sh
set -euo pipefail
top=$(cd "$(dirname "$0")/.." && pwd)
work=${GLASS_WORK:-$top}
KV=${KV:-7.2.9}
KERNEL_FPR=B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1
MUSL_FPR=836489290BB6B70F99FFDA0556BCDB593020450F
MUSL=musl-1.2.5
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
# Pinned sums (bash 3.2 on macOS has no associative arrays).
pinned() {
    case "$1" in
        7.2.9) echo b4c5dfbe51a364a6c7f03869200f88c8e1f77403539005f14b7fc6bc91b8d8ba ;;
    esac
}

die() { echo "fetch-kernel: $*" >&2; exit 1; }
sha256() {
    if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1
}
case_sensitive() {   # DIR
    local d
    d=$(mktemp -d "$1/.case.XXXXXX")
    touch "$d/A"
    if [ -e "$d/a" ]; then rm -rf "$d"; return 1; fi
    rm -rf "$d"
}
# A throwaway keyring; the key must have the pinned fingerprint, and the
# signature must be a VALIDSIG by it.
verify_sig() {   # KEYFILE FPR SIGFILE [DATAFILE]
    local g ok
    g=$(mktemp -d "$work/dl/.gpg.XXXXXX")
    chmod 700 "$g"
    gpg --homedir "$g" --batch -q --import "$1" 2> /dev/null || true
    gpg --homedir "$g" --batch --with-colons --fingerprint | grep -q "^fpr:::::::::$2:" ||
        { rm -rf "$g"; die "$1 is not the key with fingerprint $2"; }
    ok=0
    gpg --homedir "$g" --batch --status-fd 1 --verify "$3" ${4:+"$4"} 2> /dev/null |
        grep -q "^\[GNUPG:\] VALIDSIG $2 " && ok=1
    rm -rf "$g"
    [ $ok = 1 ] || die "the signature $3 is not a good one from $2"
}

command -v gpg > /dev/null || die "gpg is needed (Debian/Ubuntu: gnupg; macOS: brew install gnupg)"
[ "$(uname -s)" = Darwin ] && ! case_sensitive "$work" &&
    die "$work is on a case-insensitive volume; set GLASS_WORK to a case-sensitive one (docs/porting-log.md)"
mkdir -p "$work/dl" "$work/src"

cd "$work/dl"
T=linux-$KV.tar.xz
B=https://cdn.kernel.org/pub/linux/kernel/v${KV%%.*}.x
[ -f "$T" ] || curl -fL -o "$T" "$B/$T"
curl -fsSL -o sha256sums.asc "$B/sha256sums.asc"
[ -f kernel-autosigner.asc ] ||
    curl -fsSL -o kernel-autosigner.asc https://git.kernel.org/pub/scm/docs/kernel/pgpkeys.git/plain/keys/632D3A06589DA6B1.asc
verify_sig kernel-autosigner.asc "$KERNEL_FPR" sha256sums.asc
want=$(grep " $T\$" sha256sums.asc | cut -d' ' -f1)
[ -n "$want" ] || die "sha256sums.asc has no line for $T"
got=$(sha256 "$T")
[ "$got" = "$want" ] || die "$T: sha256 $got, kernel.org says $want"
p=$(pinned "$KV")
if [ -n "$p" ]; then
    [ "$got" = "$p" ] || die "$T: sha256 $got, pinned $p"
else
    echo "fetch-kernel: no pinned sha256 for $KV; the signed kernel.org sum was checked"
fi
echo "fetch-kernel: $T sha256 $got (signed by $KERNEL_FPR)"

if [ "$(uname -s)" = Darwin ]; then
    for f in $MUSL.tar.gz $MUSL.tar.gz.asc; do
        [ -f "$f" ] || curl -fsSL -o "$f" "https://musl.libc.org/releases/$f"
    done
    [ -f musl.pub ] || curl -fsSL -o musl.pub https://musl.libc.org/musl.pub
    verify_sig musl.pub "$MUSL_FPR" $MUSL.tar.gz.asc $MUSL.tar.gz
    [ "$(sha256 $MUSL.tar.gz)" = "$MUSL_SHA256" ] || die "$MUSL.tar.gz: sha256 mismatch"
    mkdir -p "$work/build/host-macos"
    tar -xzf $MUSL.tar.gz -O $MUSL/include/elf.h > "$work/build/host-macos/elf.h"
    echo "fetch-kernel: <elf.h> from $MUSL (signed by $MUSL_FPR) in $work/build/host-macos"
fi

cd "$work/src"
[ -d "linux-$KV" ] || tar -xf "../dl/$T"
echo "fetch-kernel: sources in $work/src/linux-$KV"
