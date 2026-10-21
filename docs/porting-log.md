# Porting log (OpenMyGlass)

The mainline port of Google Glass Explorer Edition XE-C (OMAP4430), as done in OpenMyGlass's fork of [Lasimeri/glass-xec-mainline](https://github.com/Lasimeri/glass-xec-mainline). This log records decisions, hypotheses and results, newest phase last. Upstream's own record stays in [build-log.md](build-log.md) and [hardware-test.md](hardware-test.md).

## Safety rules

The owner's rules, which cannot be waived. They bind everything in this repository, drafts included. In case of doubt, stop and ask.

1. **The bootloader is untouchable.** Never propose writes to x-loader/MLO, u-boot/bootloader, the calibration/EFS/MAC/serial partitions or the partition table. No exceptions.
2. **No flashing for tests.** New kernels are tried only with `fastboot boot` (in RAM). Flashing `boot` or `system` is allowed only with factory images, to restore, and only after the backup.
3. **Backup first.** Before any experiment there must be a complete eMMC backup script: `dd` partition by partition, SHA-256 hashes, and a check that restoring works.
4. **Voltages and regulators.** Every regulator, voltage or current limit in the DT cites its source: a 3.4 file and line, or a value measured with `regulator_summary` on 3.4. No "plausible" values and none from other boards. If a value cannot be verified, the regulator stays as it is and the fact is flagged.
5. **Thermal.** Never disable thermal zones, protections or cpufreq limits, and never raise frequencies or voltages beyond 3.4's. Glass sits against the temple.
6. **Battery and charging.** No changes to charger or fuel-gauge drivers, or to charge currents, without parameters verified from 3.4. When in doubt, charging stays disabled in mainline and the Glass charges under the original system.
7. **No physical actions.** Do not propose opening the device, soldering or attaching a UART. Work only over micro-USB.
8. **Scripts.** Every script for the device is read-only unless stated otherwise, defaults to `--dry-run`, asks for explicit confirmation before any write, and has an allowlist of the partitions it may touch. Read and check upstream's scripts before using or adapting them.
9. **Risk note.** Every change that will touch the device comes with: what can go wrong, how to notice, how to go back.
10. **Downloads.** Every external file (images, toolchains) is verified by hash or signature.

**Status of the device:** the owner has **no Glass** yet. Phase 0 is the only active phase. Phases 1-3 are an inactive draft ([future-phases-draft.md](future-phases-draft.md)).

## Phase 0 (2026-10-21): no device

### Starting point

- Upstream: kernel.org 7.2.9 with an LLVM build in a bubblewrap sandbox with no Python, two DTBs (`safe` and `full`) from one `common.dtsi`, the DTB appended to zImage, and the bootloader's ATAGs folded in.
- Upstream's hardware result: the mainline safe image starts, brings up no USB and resets after about a minute. Google's stock 3.4.94 kernel with upstream's userland works and is in daily use on their unit.
- Hardware reference: Google `kernel/omap` `glass-omap-xrx13b` at `989d925476700c0d07a75008b37725cbc558f862` (3.4.94), GPL-2.0. Files were read from android.googlesource.com and cited by file and line. None was committed (clean-room rule: no Google file, blob, firmware or image, and nothing extracted from one, is committed; facts and citations are).

### The build

**Result: a local build works on this Apple Silicon Mac, and CI builds the same thing on ubuntu-latest.** Both produce:

| artifact (7.2.9, local, macOS 27.0.1, Homebrew clang 23.1.3) | bytes |
| --- | --- |
| default `zImage` | 3,756,872 |
| default `omap4-glass-xec.dtb` (full) | 91,227 |
| default `omap4-glass-xec-safe.dtb` | 88,572 |
| debug `zImage` | 3,766,440 |
| debug `omap4-glass-xec-safe-debug.dtb` | 88,601 |

The images (boot.img) were not built in Phase 0. They need the userland and the initramfs (Alpine, apk, Google's XE24 images for the stock-kernel variants), which is upstream's `scripts/glass build` on Linux. The kernel and DTBs are what Phase 0 asks for.

#### Local build on macOS (Apple Silicon, no Docker or Podman)

Three things stand in the way, and each has a fix:

| obstacle | fix |
| --- | --- |
| The kernel tree has paths that differ only in case; APFS is case-insensitive by default | a case-sensitive APFS sparse image under the scratch area, outside the repo; `scripts/fetch-kernel.sh` refuses a case-insensitive `GLASS_WORK` |
| BSD make/sed/find/coreutils; no lld | GNU tools, LLVM and lld from Homebrew (bottles verified by Homebrew's sha256), put first on `PATH` by `build-kernel.sh` |
| The host tools (`modpost`, `sorttable`, `gen_init_cpio`) include `<elf.h>` and `<byteswap.h>` and call `strchrnul()` and `copy_file_range()`, which macOS lacks; `<uuid/uuid.h>`'s `uuid_t` clashes | musl 1.2.5's `<elf.h>` (MIT, signature checked); `tools/host-macos/` with `byteswap.h` and `macos-compat.h` (our own, MIT); `-D_UUID_T -D__GETHOSTUUID_H` |

No bubblewrap exists on macOS, so the sandbox is off there (`GLASS_SANDBOX=none`). The nopython shims are still first on `PATH` and kbuild gets `PYTHON3=false`. `out/python-calls.log` stayed empty.

The exact steps, as run:

1. Tools: `brew install llvm lld make gnu-sed gnupg findutils coreutils gawk bison flex`.
2. A case-sensitive volume (20 GB maximum, sparse, so it grows only as used): `hdiutil create -type SPARSE -fs 'Case-sensitive APFS' -size 20g -volname glassbuild <scratch>/glassbuild.sparseimage`, then `hdiutil attach <scratch>/glassbuild.sparseimage -mountpoint <scratch>/vol -nobrowse`.
3. Sources: `GLASS_WORK=<scratch>/vol scripts/fetch-kernel.sh`. It checks:
   - `linux-7.2.9.tar.xz` sha256 `b4c5dfbe51a364a6c7f03869200f88c8e1f77403539005f14b7fc6bc91b8d8ba`, against kernel.org's `sha256sums.asc` (good signature, `VALIDSIG` by `B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1`, the Kernel.org checksum autosigner) and against the pinned value;
   - `musl-1.2.5.tar.gz` sha256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`, with a good signature by `836489290BB6B70F99FFDA0556BCDB593020450F`.
4. Default variant: `GLASS_WORK=<scratch>/vol JOBS=8 scripts/build-kernel.sh zImage ti/omap/omap4-glass-xec.dtb ti/omap/omap4-glass-xec-safe.dtb ti/omap/omap4-glass-xec-safe-debug.dtb` → `config: every fragment option applied (default)`, `no Python called`.
5. Debug variant: `GLASS_VARIANT=debug GLASS_WORK=<scratch>/vol JOBS=8 scripts/build-kernel.sh zImage ti/omap/omap4-glass-xec-safe-debug.dtb` → `config: every fragment option applied (debug)`.
6. Check: `dtc -I dtb -O dts` of the safe and safe-debug DTBs. They differ only in `model` and in the ramoops properties (`record-size 0`, `console-size 0x200000`, `pmsg-size 0`, `ecc-size 16`).

Disk: the host had 11 GB free. The Homebrew tools took about 2 GB, and the sparse image grew to about 3.5 GB for the source tree and both builds.

#### CI (ubuntu-latest)

`.github/workflows/kernel.yml` does the following:
- installs `clang lld llvm make bison flex bc gnupg bubblewrap cpio xz-utils device-tree-compiler` from Ubuntu's signed archive;
- turns off Ubuntu 24.04's AppArmor block on unprivileged user namespaces, so that the bubblewrap sandbox works;
- runs `scripts/fetch-kernel.sh`;
- builds both variants inside the sandbox;
- checks that the default `.config` has none of the debug options (`USB_G_SERIAL`, `U_SERIAL_CONSOLE`, `CMDLINE_EXTEND`), that the debug one has all of them, and that the two safe DTBs differ only in ramoops and the model;
- uploads the zImages, DTBs and `.config` files with their sha256 sums (kept 14 days).

Actions are pinned by commit SHA (`actions/checkout` v7.0.1 `3d3c42e5…`, `actions/upload-artifact` v7.0.2 `cf430e03…`), and the job has `permissions: contents: read`.

### Decisions

- **D-0.1 Keep upstream's default build unchanged.** It has the same fragment, the same sandbox on Linux, the same targets and the same images. The new behaviour (macOS, `GLASS_WORK`, `GLASS_SANDBOX`, variants) is opt-in through environment variables.
- **D-0.2 The debug variant is opt-in** (`GLASS_VARIANT=debug`), with its own build directory, DTB and boot image (`boot-safe-debug.img`, made only when the debug kernel exists). The reasons are these:
  - It adds the legacy serial gadget, which takes the USB controller, so the default image's NCM network would stop working.
  - Its command line is forced with `CMDLINE_EXTEND`, which changes how the bootloader's line is treated.
  - Neither change belongs in the default build until it has been tested.
- **D-0.3 Make the debug ramoops readable by the stock kernel.** The 3.4 Android RAM console and mainline's pstore/ram share the header, signature, Reed-Solomon parameters (128-byte blocks, 16 parity bytes, symbol size 8, polynomial 0x11d) and the parity placement. Compare `drivers/staging/android/persistent_ram.c:28-35, 175-210` (3.4) with `mainline:fs/pstore/ram_core.c:32-39` and `persistent_ram_init_ecc`. With no dmesg records, ramoops puts the console zone first (`mainline:fs/pstore/ram.c:786-796`). So the safe-debug DTB lays the 2 MB at 0xA0000000 out as one ECC-16 console zone, and the stock kernel should then show mainline's log as `/proc/last_kmsg`. Until tested this is only an expectation from reading the code (test T1).
- **D-0.4 No device tree change in Phase 0.** The differences found ([safe-dt-review.md](safe-dt-review.md)) are listed with proposals and risk notes. The ones that touch regulators, pins or memory wait for Phase 1 readings and the owner's decision.
- **D-0.5 Upstream's device-facing scripts and docs are not reviewed yet.** The README says not to use them. Reviewing them against rules 1-3 and 8 is part of Phase 1 (rule 8).

### Phase 0 findings, in short

- **Hardware map:** [hw-map.md](hw-map.md), every item with its 3.4 `file:line` and whether the safe and full DTs describe it. Every pad offset in the DTs matches 3.4's `mux44xx.h`.
- **Safe DT against the map:** [safe-dt-review.md](safe-dt-review.md). The biggest gaps:
  - **D1**: on 2 GB the secure workspace is at the top of RAM, not at 0xBFD00000 as the DT reserves;
  - **D2**: VAUX2, VAUX3, VUSIM and VANA (and CLK32KG) are always on in 3.4 but switchable in mainline;
  - **D3**: mainline can lower VUSIM from 3.0 V to 2.9 V at registration, which no `*_ignore_unused` flag stops;
  - **D4**: none of 3.4's thermal layers (PCB sensor, case and duty-cycle governors) exists in mainline;
  - **D5**: the camera sensor is not powered down (GPIO 100), and the USIM and MMC1 pad banks are not set up.
- **The reset:** [reset-analysis.md](reset-analysis.md). The ranking:
  - H1: an early stall, with a watchdog left running from before Linux;
  - H2: a panic or another software reboot, with USB failing separately;
  - H3: rails or clocks switched off as unused, which the command-line flags should have prevented;
  - H4: the secure workspace used as RAM;
  - H5: a PMIC or thermal shutdown, or a brown-out.

  The tests are future and inactive. The first two need no new kernel feature: `/proc/last_kmsg` on the stock system after a debug boot, and the reset source in PRM_RSTST (0x4A307B04).

### Waiting for the owner

1. D1-D8 in [safe-dt-review.md](safe-dt-review.md): which proposals to take, once Phase 1 has the readings they need.
2. Thermal policy for any mainline session (D4): how long, off the head, and what CPU cap until a PCB thermal zone exists.
3. Whether the default image should also force its flags with `CMDLINE_EXTEND` (D8).
4. Phases 2 and 3: the brief was cut off in Phase 1.
