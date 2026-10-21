# Why mainline resets about a minute after boot

Phase 0 analysis, from source code only. **Every test below is a future step and is inactive** until the owner says the hardware is at hand. They also wait for the Phase 1 backup (rule 3). They run kernels only with `fastboot boot`, which loads into RAM and flashes nothing (rule 2). They write to no partition, and any script they need is read-only with a `--dry-run` default (rule 8). The risk note for each test is at the end (rule 9).

Source conventions are as in [hw-map.md](hw-map.md): a bare `file:line` is `arch/arm/mach-omap2/` at 3.4 commit 989d925, and `mainline:` is Linux 7.2.9.

## What was observed (upstream, one unit, 2026-10-07)

From [hardware-test.md](hardware-test.md), "What happened on the first hardware day":

> the mainline safe image took over, brought no USB up and reset after about a minute, leaving no crash record.

What that means and what it does not:

1. **The kernel was started.** `fastboot boot` runs the image it is sent (step 4 of the same session proved it).
2. **No USB device appeared on the host within about a minute.** On a working boot the initramfs brings up the gadget in about 15 s (`userland/initramfs/etc/glass/rcS`, `usr/sbin/glass-gadget`). So either userspace never ran, or the USB path itself failed.
3. **The device reset**, by its own doing. No one timed it, and nothing records whether it was a warm reset (SoC reset, RAM kept) or a power cycle.
4. **"No crash record" is weak evidence.** The ramoops log can be read only by a kernel that understands its layout, and every mainline boot failed the same way. The stock kernel's `/proc/last_kmsg` reads the same 2 MB, but as one ECC-protected zone, so it cannot read the default ramoops layout (see test T1).
5. **The boot.img command line** had `clk_ignore_unused regulator_ignore_unused pd_ignore_unused panic=10` (`scripts/build-images.sh`, `mcmd`). The bootloader passes the boot.img line to the kernel as an ATAG: the stock kernel has `CONFIG_CMDLINE=""` (`notle_defconfig:509`), and its `/proc/cmdline` holds the boot.img line (glass-kernel facts). Whether it passes the line unchanged, and how long a line it accepts, is unknown.

## Timers that could produce "about a minute"

| what | when | source |
| --- | --- | --- |
| Regulator late cleanup (off unless `regulator_ignore_unused`) | late_initcall_sync + 30 s, so about 32-35 s after start | `mainline:drivers/regulator/core.c:6881-6909` |
| Unused clocks off (unless `clk_ignore_unused`) | late_initcall_sync, a few seconds in | `mainline:drivers/clk/clk.c:1511-1558` |
| Unused power domains off (unless `pd_ignore_unused`) | late_initcall_sync | `mainline:drivers/pmdomain/core.c:1356-1387` |
| OMAP WDT2 default margin, if something starts it | 60 s | `mainline:drivers/watchdog/omap_wdt.h:29` |
| A watchdog the bootloader left running | its own timeout, unknown | (bootloader, not readable here) |
| Reboot after a panic | `panic=10`: 10 s after the panic | boot.img line; `config/glass.config` `PANIC_TIMEOUT=10` |
| TWL6030 charger watchdog (3.4 pets it with 32 s) | ≤ 32 s after the last pet, if the bootloader armed it | `drivers/power/twl6030_charger.c:38, 560-563, 1278-1280` |

## Hypotheses, most probable first

### H1. The kernel stops early, before it stops a watchdog that runs from before Linux

**Mechanism.** WDT2 (or the secure watchdog) is already counting when the kernel starts. Mainline stops WDT2 in two places, both well after early boot: when ti-sysc probes its target module, the WDT reset quirk writes the 0xAAAA/0x5555 disable sequence (`mainline:drivers/bus/ti-sysc.c:1580-1581, 1961-1983, 2057-2059`), and `omap_wdt` probe disables it unless `early_enable` is set (`mainline:drivers/watchdog/omap_wdt.c:101-112, 270-275`). If the kernel hangs or dies before those run (in the decompressor, early setup, SMP bring-up through the PPA, or an early driver), the running watchdog resets the SoC at whatever timeout the bootloader set.

**For it:**
- It explains all three symptoms with one cause: no USB (userspace never ran), no record (pstore never initialised, or never written), and a reset rather than a power-off.
- Both kernels clearly expect WDT2 to be running when they start. 3.4 disables it at hwmod setup (`wd_timer.c:29-50`) and mainline does so in two drivers.
- OMAP4 records a reset by the MPU watchdog and one by the **secure** watchdog as separate sources (`mainline:arch/arm/mach-omap2/prm-regbits-44xx.h:52, 74`; `prm44xx.c:72-91`), so the secure side has a watchdog of its own that Linux cannot stop on an HS part.
- On an HS 4430, mainline calls the PPA while it brings up the second CPU (`mainline:arch/arm/mach-omap2/omap-smp.c:156-158`), which is a plausible place for an early stall on a PPA that is not TI's stock one.

**Against it:** nothing is known about the bootloader's watchdog setting (the bootloader is Google's binary and is not analysed here). A one-minute timeout is plausible but unproven.

**Tests (future, inactive):**
- **T1, where the log ends.** Boot the debug image (`boot-safe-debug.img`: `config/glass-debug.config` and `dts/omap4-glass-xec-safe-debug.dts`). Its ramoops is a single ECC console zone laid out like the stock RAM console. After the reset, let the Glass boot its stock system and read `/proc/last_kmsg` (read-only). If the log is there, its last lines show how far the kernel got. If there is no buffer at all, the kernel never reached pstore (before about `device_initcall`), which supports H1. First run the control on the stock system (`adb reboot`, then `/proc/last_kmsg` shows the previous boot) to prove that the bootloader keeps that RAM across a warm reset.
- **T2, the reset source.** On the stock system, read PRM_RSTST once, read-only: OMAP4430 PRM base 0x4A306000 + device instance 0x1B00 + RSTST 0x0004 = **0x4A307B04** (`mainline:arch/arm/mach-omap2/prm44xx.h:28, 53, 60`). Bit 3 means an MPU watchdog reset (WDT2), bit 4 a secure watchdog reset, bit 1 a software warm reset (a panic reboot, H2), bit 2 an MPU security violation (H4), and bit 0 a cold reset (a power cycle, H5) (`prm-regbits-44xx.h:29-85`). Run the controls first: read it after `adb reboot` (expect bit 1) and after a power-on (expect bit 0). That shows whether the bootloader clears the register. If it does, T2 is useless and T1 carries the weight.
- **T3, the time.** On the host only, timestamp the end of `fastboot boot` and the moment the device re-enumerates (as fastboot or Android). Three runs: a fixed delay points to a watchdog, a varying one to something else.
- **If H1 is confirmed:** T1 says where the kernel stops. The fix is either for that stall, or to service the watchdog from the start (`omap_wdt.early_enable=1` with `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`, which mainline pings until userspace opens the device). That is only a stop-gap for WDT2. The secure watchdog would need the PPA.

### H2. A panic (or other software reboot) after a while, with USB failing for a separate reason

**Mechanism.** The kernel boots far enough to reach userspace, but the USB gadget never enumerates: the MUSB, the TWL6030 VBUS comparator (which needs the PMIC interrupt), the PHY or the mux. Then something panics, and `panic=10` reboots 10 s later.

**For it:** two independent faults are less likely than one, but each one is ordinary in a first bring-up. The default ramoops layout would keep the panic, but nothing could read it (observation 4).

**Against it:** it needs two faults. The mux hogs, the MUSB mode and the PMIC interrupt all match 3.4 ([safe-dt-review.md](safe-dt-review.md)).

**Tests (future, inactive):** T1 shows `Kernel panic` and the cause in `/proc/last_kmsg`. T2 shows bit 1. The debug variant's `console=ttyGS0` comes from the legacy serial gadget built into the kernel, which binds without the initramfs. If `/dev/ttyACM0` appears with it but NCM never does with the default image, the fault is in the userspace gadget setup. If neither appears, it is in the kernel's USB path.

### H3. Regulators, clocks or power domains switched off as "unused"

**Mechanism.** Mainline switches off what no driver claims: regulators 30 s after late init (`mainline:drivers/regulator/core.c:6806-6909`), clocks and power domains at late init (`clk.c:1519-1558`, `pmdomain/core.c:1367-1387`). In the safe DT, VAUX2, VAUX3, VUSIM and VANA are not always-on, although 3.4 keeps them on. CLK32KG has no consumer either ([safe-dt-review.md](safe-dt-review.md), D2). Switching off a rail that, for example, the FPGA or the glasshub needs could brown out or hang the board.

**For it:** it is the classic first-boot failure, and the late regulator cleanup lands at about 30-35 s, in the range of "about a minute" for an untimed observation.

**Against it:** all three `*_ignore_unused` flags were on the command line, and the bootloader does pass the boot.img line (observation 5). There are three ways it could still happen:
- the line arrives changed or truncated;
- the registration-time voltage move, which no flag stops: VUSIM "Bringing 3000000uV into 2900000-2900000uV" (`mainline:drivers/regulator/core.c:1315-1360`; D3);
- ti-sysc resetting and idling modules that have no driver, which the clock flag does not cover.

**Tests (future, inactive):**
- **T4.** With the debug image, `/proc/last_kmsg` (T1) must contain `regulator: Not disabling unused regulators`, `clk: Not disabling unused clocks` and the genpd equivalent. The debug variant forces the flags with `CONFIG_CMDLINE_EXTEND`, whatever the bootloader passes. If the reset still happens with those lines in the log, H3 is out, apart from the registration-time voltage move, which shows up as a `Bringing` line in the same log.
- **T5**, only after T4. In Phase 1, read `regulator_summary` and `clk_summary` on the stock kernel and compare them with the mainline log line by line.

### H4. The 2 GB secure workspace is used as ordinary memory

**Mechanism.** On 2 GB, 3.4 hides 3 MB at the top of RAM for the secure side (`board-notle.c:2382, 2461`). Mainline hides 0xBFD00000 instead, the 1 GB value (D1). If the PPA firewalls its real workspace, the first mainline access to that page causes a security-violation reset (`MPU_SECURITY_VIOL_RST`, `prm-regbits-44xx.h:51`). If it does not firewall it, the PPA and the kernel may overwrite each other's data.

**For it:** the mismatch is certain from the code.

**Against it:** that RAM is at the top of highmem. The page allocator hands out low pages first, so in the first minute it is rarely touched. The secure side may not use the region at all without the 3.4 Trusted Foundations driver (only its platform device is in the tree, `omap-secure.c:204-214`). That makes it a weak explanation for a reset at a fixed time, though still a gap that must be closed before long sessions.

**Tests (future, inactive):** T2 bit 2. In Phase 1, read the stock `dmesg` line `SMC Carve Out : 0x...` (`board-notle.c:2405`) and `/proc/iomem`. A later DT variant with that range `no-map` should change the behaviour if H4 is the cause.

### H5. A PMIC or thermal shutdown, or a brown-out

**Mechanism.** These candidates power the device off rather than reset it, and with USB plugged in the PMIC may start it again, which looks like a reset:
- the bandgap's critical trip at 125 °C calls an orderly power-off (`mainline:arch/arm/boot/dts/ti/omap/omap4-cpu-thermal.dtsi:28`);
- the TWL6030's own thermal protection;
- a low battery once charging stops. Mainline never pets the TWL6030 charger watchdog that 3.4 pets every 32 s (`drivers/power/twl6030_charger.c:560-563, 1278-1280`; `CHARGER_TWL6030=m` never loads).

**For it:** mainline has none of 3.4's thermal layers (D4), and the CPU runs at an unknown, unmanaged rate.

**Against it:** a cold Glass does not reach 125 °C on the die in a minute while idle in an initramfs. The 4430 has no hardware TSHUT line in mainline (`omap4-thermal-data.c:58-62` has no `TSHUT` feature), so a spurious TSHUT GPIO cannot fire. A brown-out needs a weak battery, and upstream's checklist starts with a full charge.

**Tests (future, inactive):** charge fully on the stock system first, and note the bq27520 voltage in Phase 1. With the debug image, T1's log shows `critical temperature reached` if the thermal core shut down. T2 bit 0 (cold reset) points to a power cycle. Watch whether the device restarts unplugged: unplug the cable during the test, so that a power-off stays off. A watchdog reset reboots anyway.

## Order of the tests, when the hardware is here (inactive)

1. Phase 1 backup (rule 3) and the read-only stock readings: `dmesg` (SMC carve-out, board revision), `regulator_summary`, `clk_summary`, `/proc/iomem`, pad mux, the controls for T1 and T2.
2. One `fastboot boot` of `boot-safe-debug.img`, unplugged once it starts (H5) and timed from the host (T3).
3. After the reset, on the stock system: `/proc/last_kmsg` (T1, T4) and PRM_RSTST (T2).
4. Decide between H1-H5 from that, then change one thing at a time.

## Risk notes (rule 9), for when the tests become active

- **Booting the debug image with `fastboot boot`.**
  - *What can go wrong:* the same failure as before (a hang, then a reset). It cannot write the eMMC, because no MMC controller is enabled. Its regulators, clocks and pins are those of the safe image: the DT differs only in the ramoops layout, which CI checks on every build. It inherits the safe image's possible VUSIM move from 3.0 V to 2.9 V (D3). It has no thermal management (D4).
  - *How to notice:* no `/dev/ttyACM0` on the host, a warm device, or a reset.
  - *How to undo it:* nothing was written. A reset or a long press of power ends it, and the next boot is the stock system. Keep the test short (a few minutes at most) and never on the head.
- **Reading `/proc/last_kmsg` and PRM_RSTST on the stock system.**
  - *What can go wrong:* nothing is written, as long as the PRM read is a plain 32-bit read of one register with a tool that only reads.
  - *How to notice:* not applicable.
  - *How to undo it:* not applicable.
  - Getting root on the stock system for `/dev/mem` uses the sibling repository's `fastboot boot` of an insecure ramdisk (OpenMyGlass/glass-rom), never a flash.
- **Unplugging during the test (H5).**
  - *What can go wrong:* the device runs on battery. If it powers off, it stays off.
  - *How to notice:* no LED and no USB.
  - *How to undo it:* a long press of power starts the stock system.
