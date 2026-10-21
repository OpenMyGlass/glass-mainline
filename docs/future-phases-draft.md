# Later phases: an INACTIVE DRAFT

> **Inactive.** Nothing in this file is to be done until the owner says the Glass is at hand. It is a plan, not instructions. It deliberately has no ready-to-run commands. Each step that will touch the device is written out in full when its phase starts, and is reviewed against the ten safety rules ([porting-log.md](porting-log.md#safety-rules)) first.
>
> The owner's brief was cut off partway through Phase 1. Phase 1 below follows the part that arrived. Phases 2 and 3 are placeholders until the owner describes them.

## Phase 1: with the device, read-only (draft)

**Goal:** a full backup, and the stock 3.4 system's own view of the hardware, so that every value in the device trees can cite either a 3.4 source line or a reading (rule 4).

### 1.1 Backup first (rule 3)

- A complete eMMC backup, partition by partition with `dd`, each with a SHA-256 hash, and a check that restoring would work. The check reads the image back and compares hashes. It writes nothing.
- Before writing a script of our own, review upstream's `scripts/glass backup` against rule 8 (it was written for upstream's unit and its own flow). The script that comes out of this must:
  - be read-only, with `--dry-run` as the default;
  - write only to the host;
  - carry an allowlist of partitions it may read;
  - refuse to continue if any image's size differs from its partition's;
  - never offer a write. Restoring is a separate script, with typed confirmation and its own allowlist, which never includes `xloader`, `bootloader`, `efs`/`bootconfig`, calibration or the partition table (rule 1).

### 1.2 Read-only data from the stock 3.4 system

What to collect, and what each item settles:

| reading | settles |
| --- | --- |
| `/proc/version`, `/proc/config.gz` | which 3.4 build this is, against 989d925 (OpenMyGlass/glass-kernel already plans this) |
| `dmesg`: `Notle board revision: ...` | EVT2+ GPIO table assumed everywhere ([hw-map.md](hw-map.md)) |
| `dmesg`: `Notle Board Ram Size`, `Notle Carveout` block, `SMC Carve Out : 0x...` | the real secure workspace (D1, H4) |
| `/proc/iomem`, `/proc/meminfo` | memory map as 3.4 sees it |
| `/proc/cmdline` | what the bootloader adds to the boot.img line (D8) |
| `regulator_summary` (debugfs) | every TWL6030 rail's voltage and state: VUSIM 3.0 V? (D2, D3, H3) |
| `clk_summary` / 3.4 clock debugfs, cpufreq stats | the MPU rate the bootloader leaves, the 3.4 policy in practice (D4) |
| `/sys/kernel/debug/omap_mux/` | pad modes: GPIO 100, fref_clk0_out (MSECURE), the wake bank (D5, D6) |
| `/sys/kernel/debug/gpio` | the levels of GPIO 100, 43, 48, 45/46 and so on |
| thermal framework debugfs, the PCB sensor | the 3.4 temperatures at idle, for the future mainline thermal zone (D4) |
| bq27520 `power_supply` | battery state before any mainline test (H5) |
| `/proc/last_kmsg` after `adb reboot` | the RAM console survives a warm reset: the control for test T1 |
| PRM_RSTST (0x4A307B04), one 32-bit read, after `adb reboot` and after a power-on | whether the bootloader keeps the reset source: the control for test T2 |

All of these are reads. Getting root for debugfs and `/dev/mem` uses a `fastboot boot` of an insecure stock ramdisk (rule 2: RAM only), as the sibling OpenMyGlass/glass-rom repository does. Nothing is flashed.

### 1.3 Risk note for Phase 1 (rule 9)

- **`fastboot boot` of the insecure stock image:** at worst it does not boot, and the next power-on is the normal stock system, with nothing written. Upstream saw Google's rooted ramdisk reboot itself after 23 s on their unit (docs/hardware-test.md). The readings must therefore be quick, or come from another route that also writes nothing.
- **debugfs and `/dev/mem` reads:** reading the PRM register and the debugfs summaries changes nothing. A write would, so the collection script must open `/dev/mem` read-only and map it `PROT_READ`.

## Phase 2: with the device, the first mainline tests (placeholder)

To be defined by the owner. From Phase 0, the first candidate is the reset diagnosis in [reset-analysis.md](reset-analysis.md) ("Order of the tests"). It boots `boot-safe-debug.img` with `fastboot boot` only, and then reads `/proc/last_kmsg` and PRM_RSTST on the stock system. Before that, these owner decisions are needed: D1-D8 in [safe-dt-review.md](safe-dt-review.md), and the thermal limits for any mainline session (D4).

## Phase 3: (placeholder)

To be defined by the owner. Candidates the Phase 0 map points to:
- a PCB-temperature thermal zone (GPADC channel 6);
- the panel and FPGA driver, enforcing the 3.4 LED power limits;
- audio (TWL6040, ABE);
- the light sensor (LTR-506);
- the glasshub, read-only, with no reflash path.
