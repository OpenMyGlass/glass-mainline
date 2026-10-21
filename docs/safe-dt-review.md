# The `safe` device tree, checked against the 3.4 hardware map

Phase 0 document. It compares `dts/omap4-glass-xec-safe.dts` (which is `omap4-glass-xec-common.dtsi` plus a model string), and what it inherits from mainline's `omap443x.dtsi`, `twl6030.dtsi` and `twl6030_omap4.dtsi`, with [hw-map.md](hw-map.md). Source references follow the conventions there: a bare `file:line` is `arch/arm/mach-omap2/` at 3.4 commit 989d925, and `mainline:` is Linux 7.2.9.

**Nothing in the device trees has been changed in Phase 0.** Every proposal below is a proposal. Several of them touch regulators, GPIOs or memory that the device would use, so they wait for the owner's decision and for the Phase 1 readings that confirm them (rule 4: no regulator value without a source; rule 9: a risk note for every change that will touch the device).

## What the safe DT gets right

These were checked line by line:

- **Every pad offset** it uses matches `mux44xx.h` (3.4): UART3 0x144/0x146, I2C1-4 0x122-0x130, USBA0 0x194-0x198, the mux GPIOs 0x06a/0x06c, and SYS_NIRQ1 0x19e.
- **The USB mux** hogs (CB0=GPIO 46 high, CB1=GPIO 45 low) are the `usb` state of `notle-usb-mux.c:173-176`. GPIO 46 is the EVT2+ CB0 (`board-notle.h:74`).
- **MUSB:** UTMI, peripheral, `power = <50>` (2 mA units, so 100 mA), matching `board-notle.c:692-700`.
- **The TWL6030** at I2C1 0x48 on SYS_NIRQ1 (GIC SPI 7) matches `twl-common.c:43-47, 112`, with the SYS_NIRQ1 pad as in `board-notle.c:2005`.
- **VAUX1** at 2.9 V and always on matches `board-notle.c:811-830` (the eMMC's supply, kept on even though the safe image has no MMC).
- **No MMC controller** is enabled, so the image cannot reach the eMMC.
- **The display** is not touched: the DSS target module is disabled, the panel, FPGA, LCD_RST_N (94), FPGA_CRESET_B (87) and DISP_ENB are not named, and the bootloader's state stays. This also keeps the 3.4 LED power limits out of reach (`board-notle.c:569-588`).
- **ramoops** sits in the 3.4 RAM console's region (0xA0000000, 2 MB, `omap_ram_console.h:21-22`), which is memory both kernels already set aside.
- **The OMAP watchdog** is described (`omap4-l4.dtsi` wdt2) and `CONFIG_OMAP_WATCHDOG=y`, so it is stopped at probe, as 3.4 also does (`wd_timer.c:29-50`).
- **Charging and the fuel gauge** are absent from the safe DT, and `CHARGER_TWL6030` is a module that never loads, so mainline does not touch charging (rule 6).
- **The glasshub MCU** is not described, so nothing can reach its reflash path (`drivers/input/misc/glasshub.c:223-245`).

## Differences and gaps, most important first

### D1. The 2 GB secure workspace is not where the DT reserves it (memory)

- **3.4:** `smc_addr = 0x80000000 + ram_size - 3 MB`, removed from the memory map and given to the secure driver (`board-notle.c:2362-2365, 2382, 2461`; `omap-secure.c:219-250`). On a 2 GB unit that is near the top of RAM, around 0xFFC00000 if the bootloader reports a little under 2 GB ("one page or MB is taken off the top", upstream's note on Google's 2 GB commit). The exact value is printed at boot as `SMC Carve Out : 0x... Size: (00300000)` (`board-notle.c:2405`).
- **Safe DT:** `secure@bfd00000`, 3 MB, no-map. That is `PHYS_ADDR_SMC_MEM` from `omap4_ion.h:25` and the TI default for "at least 1 GB" (`include/mach/omap-secure.h:139-148`), not what the notle board code passes on 2 GB.
- **Mainline:** reserves only its own 1 MB of secure RAM storage at the top of lowmem (`mainline:arch/arm/mach-omap2/omap-secure.c:111-118`, `common.c:26-30`). Nothing reserves the 3.4 workspace, so on 2 GB it is ordinary highmem.
- **Why it matters:** the 3.4 comment says the PPA "will need to have a pre-knowledge of this address range ... [and] additional firewalls" may protect it (`omap-secure.c:230-236`). If the region is firewalled, a mainline access to it is a security violation, and OMAP4 has a reset source for exactly that (`mainline:arch/arm/mach-omap2/prm-regbits-44xx.h:51`, `MPU_SECURITY_VIOL_RST`). This is hypothesis H4 in [reset-analysis.md](reset-analysis.md).
- **Proposal (owner decision, after Phase 1):** read the stock `dmesg` line and `/proc/iomem` on the unit, then add a `no-map` reservation for exactly that range, keeping the existing one. Reserving memory changes no voltage, clock or pin. Risk note: if the address is wrong, 3 MB of RAM is lost and nothing else changes. You can see the result in `/proc/iomem` on mainline. To undo it, remove the node.

### D2. Four regulators that 3.4 keeps always on are not always-on in mainline (regulators)

| regulator | 3.4 | mainline (`twl6030.dtsi`) | source |
| --- | --- | --- | --- |
| VAUX2 | 2.8 V, `always_on` (sensors, camera, ABE `av-switch`) | 1.2-2.8 V, may be switched off | `board-notle.c:841-859` |
| VAUX3 | 1.2 V, `always_on` (no consumer named) | 1.0-3.0 V, may be switched off | `board-notle.c:861-876` |
| VUSIM | 3.0 V, `always_on` (touchpad) | 1.2-2.9 V, may be switched off | `board-notle.c:935-951` |
| VANA | 2.1 V, `always_on` | fixed 2.1 V, may be switched off | `board-notle.c:953-971` |

- In mainline, a DT system has full constraints (`mainline:drivers/regulator/core.c:6881-6905`), so 30 s after late init every regulator that is enabled, not always-on and unused is switched off (`core.c:6806-6847`). Only `regulator_ignore_unused` stops it (`core.c:6850-6866`). Upstream's boot.img command line has that flag. If the flag is lost, though, these four rails go off. VAUX3 in particular feeds something 3.4 never names.
- The same applies to CLK32KG, which 3.4 keeps always on (`board-notle.c:1060-1071`). In mainline it is a clock with no consumer in the safe DT, so `clk_disable_unused` would gate it unless `clk_ignore_unused` is passed (`mainline:drivers/clk/clk.c:1511-1558`).
- **Proposal (owner decision):** add `regulator-always-on;` to `&vaux2`, `&vaux3`, `&vusim` and `&vana` in `omap4-glass-xec-common.dtsi`, citing the lines above. This adds no voltage and no new value: it only stops mainline switching off what 3.4 keeps on. Risk note: the risk is a rail that 3.4 keeps on staying on, which is what the stock system does; you can see it in `regulator_summary` on mainline (Phase 2); to undo it, remove the property. CLK32KG needs a consumer or a protected-clocks style mechanism, still to be designed. Until then `clk_ignore_unused` stays on the command line (the debug variant forces it, see `config/glass-debug.config`).

### D3. VUSIM can be lowered from 3.0 V to 2.9 V at boot (regulator voltage)

- **3.4:** VUSIM is 3.0 V with `apply_uV` (`board-notle.c:936-940`).
- **Mainline:** `twl6030.dtsi` gives VUSIM 1.2-2.9 V. A DT range turns on `apply_uV` (`mainline:drivers/regulator/of_regulator.c:110-111`), and at registration a rail above its range is moved down to the maximum: "Bringing 3000000uV into 2900000-2900000uV" (`mainline:drivers/regulator/core.c:1315-1360`). `regulator_ignore_unused` does **not** stop this.
- So if the bootloader leaves VUSIM at 3.0 V, the safe image lowers the touchpad's supply by 0.1 V, a voltage that no Glass source names.
- **Proposal (owner decision, after Phase 1):** first read the stock `regulator_summary` (VUSIM's real voltage). If it is 3.0 V, set `regulator-min-microvolt = regulator-max-microvolt = <3000000>` on `&vusim`, citing `board-notle.c:938-939`. Rule 4 is met by the 3.4 source, and the reading confirms it. Risk note: a wrong value would put the touchpad's rail at a voltage it does not get on 3.4. You would see it in `regulator_summary` on mainline and in a touchpad that does not answer. To undo it, go back to the mainline range, which only ever lowers the voltage. **Until then, the current behaviour (a possible lowering to 2.9 V) is flagged and not changed.**
- The other ranges contain the 3.4 values, so no move happens if the bootloader uses those values: VAUX2 ≤ 2.8 V, VAUX3 1.2 V within 1.0-3.0 V, VMMC 1.8 V within 1.2-3.0 V, VPP 1.8-2.5 V. VAUX1 is fixed at 2.9 V, as in 3.4.

### D4. No thermal management equivalent to 3.4 (thermal, rule 5)

- **3.4:** three layers. A PCB thermistor on TWL6030 GPADC channel 6 (`notle_pcb_sensor.c:68-107`), a case governor (hot 62 °C, cold 57 °C; `case_governor.c:31-35`), and a duty-cycle governor that caps the CPU by PCB temperature (1008 MHz at most 37 % of the time above 30 °C, 800 MHz cap from 40 °C, 600 MHz from 70 °C; `board-notle.c:229-388`). On the die, alert at 100 °C, critical at 110 °C and shutdown at 125 °C (`omap_die_governor.c:45-51`). Upstream saw the case cap hold the CPU at 300 MHz when warm (docs/build-log.md).
- **Mainline safe image:** only the bandgap zone, with passive at 100 °C and critical at 125 °C (`mainline:arch/arm/boot/dts/ti/omap/omap4-cpu-thermal.dtsi:23, 28`). The passive trip has no cooling device in practice, because `CPUFREQ_DT` is a module that never loads, and nothing reads the PCB or the skin. The CPU runs at whatever rate the bootloader left. That rate is unknown and no higher than 3.4 allows, since mainline never raises it.
- **Proposal (owner decision):** no long mainline sessions, and never on the head, until a PCB-temperature zone exists. That zone would be TWL6030 GPADC channel 6, mainline `twl6030-gpadc` (IIO) and `generic-adc-thermal`, with the 3.4 polynomial turned into a lookup table and the 3.4 trip points. Before any cpufreq is enabled, its table must be capped at the 3.4 policy. Phase 1 reads the bootloader's MPU rate from the stock kernel (`/sys/kernel/debug/clock` or cpufreq stats) and the Phase 2 mainline `clk_summary`.

### D5. The camera sensor is not powered down, and the USIM and MMC1 pad banks are not set up (GPIOs and pads)

- **3.4** drives CAM_PWDN (GPIO 100) low at boot "such that its properly powering down the Camera Sensor" (`board-notle.c:1214-1223, 2292`). Pad sdmmc1_clk is in mode 3 with off-mode output low (`board-notle.c:1998`). It powers the USIM and MMC1 pad banks and sets them to 1.8 V in PBIASLITE ("Camera power down on gpio_100 needs the correct voltage of 1.8V", `board-notle.c:2252-2259`), and it sets GPIOWK_IO_PWRDNZ for the wakeup pads (`board-notle.c:2247-2250`).
- **Safe DT:** none of this. The sensor stays in whatever state the bootloader left. If it is not powered down, it draws current and makes heat. The wake pads (gpio_wk0-4) only matter for the full DT, where the touchpad, IMU and BT/Wi-Fi host-wake lines live, and there they may simply not work.
- **Proposal (owner decision, after Phase 1):** read the pad and GPIO state on the stock kernel (`/sys/kernel/debug/omap_mux/`, `/sys/kernel/debug/gpio`). If the bootloader leaves GPIO 100 high, add an output-low hog on GPIO 100 with its pad in mode 3, citing the lines above. Risk note: driving a line low that is meant to be low. If the number were wrong, a different line would be driven low. You would see it in `/sys/kernel/debug/gpio` and in the camera's current draw. To undo it, remove the hog. The PBIASLITE and PWRDNZ setup belongs in the full DT's work, through mainline's `pbias` regulator and the pinctrl-single wakeup bank.

### D6. The TWL6030 MSECURE pad is changed by an inherited include (pinmux)

- `twl6030_omap4.dtsi` muxes `fref_clk0_out` to `sys_drm_msecure` (mode 2) through the TWL node's pinctrl. The notle board mux does not mention this pad (`board-notle.c:1986-2019`), so on 3.4 it keeps its bootloader or default setting.
- On most OMAP4 boards this pad drives the PMIC's MSECURE input, and the include says as much. On Glass that is unconfirmed.
- **Proposal:** read the pad on the stock kernel (`/sys/kernel/debug/omap_mux/`). If it is not already mode 2, override `&twl { pinctrl-0 = <&twl6030_pins>; };` so that the safe DT does not change it. Risk note: changing a pad's function can disconnect whatever is wired to it. You would see it in the pad register and in PMIC access errors (RTC). To undo it, use the original pinctrl.

### D7. I2C buses at 400 kHz instead of 384 kHz (buses)

- **3.4:** I2C1 at 384 kHz (`twl-common.c:120-121`), and I2C2-4 at 384 kHz (`board-notle.c:1934-1938`). The common.dtsi comment says 384 and then sets `<400000>`.
- **Effect:** a 4 % faster clock, still within fast mode. On I2C1 it reaches the PMIC.
- **Proposal:** set `clock-frequency = <384000>` on all four, citing those lines. Low risk. Risk note: the bus runs at the 3.4 rate. If the controller cannot divide to it exactly you would see a slightly different rate, and the change is undone by going back to 400000.

### D8. The command line depends on the bootloader (boot)

- With `ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER`, the boot.img line replaces the DT's. If the bootloader passes no ATAG_CMDLINE, mainline falls back to `omap2plus_defconfig`'s `CONFIG_CMDLINE="root=/dev/mmcblk0p2 rootwait console=ttyO2,115200"` (`CONFIG_CMDLINE_FROM_BOOTLOADER`). Then `clk_ignore_unused`, `regulator_ignore_unused` and `pd_ignore_unused` are all missing.
- **Evidence that it does pass the line:** the stock kernel is built with `CONFIG_CMDLINE=""` (`notle_defconfig:509`), and its `/proc/cmdline` carries the boot.img line (`console=ttyO2,115200n8 vmalloc=500M ...`, glass-kernel facts). So the ATAG path works on this bootloader. Whether it adds to the line or shortens it is unknown.
- **Done in Phase 0:** the debug variant appends its flags with `CONFIG_CMDLINE_EXTEND`, so they hold either way. The default build is unchanged. Making the default build do the same is an owner decision.

### D9. Smaller differences, no action proposed

- **UART3 CTS/RTS** pads are not muxed; 3.4 muxes all four (`board-notle.c:1121-1141`). Only TX/RX are needed.
- **The USB mux** is fixed to USB at boot. 3.4 starts floating and chooses by cable (`board-notle.c:1515-1524`; `notle-usb-mux.c:150-158`). Mainline does not read the ID pin, so a factory cable does not reboot it into the bootloader the way 3.4 does (`notle-usb-mux.c:145-149`). That is harmless.
- **EMIF** settings are left as the bootloader set them (3.4 reprograms them: `board-notle.c:2176-2183, 2233-2236`). Mainline's EMIF driver needs an LPDDR2 device in the DT, and there is none.
- **The `framebuffer@9c700000`** reservation takes 1 MB of what 3.4 uses as ordinary RAM. It is harmless.
- **I2C2-4** are enabled with no devices. 3.4 registers them too.

## Not described in the safe DT, by design

The eMMC, Wi-Fi and Bluetooth, the touchpad, IMU, light sensor, glasshub, fuel gauge, charger, audio, camera, GPS, panel and FPGA are not described. The full DT adds the eMMC, BCM4330 (Wi-Fi and BT), touchpad, IMU, gauge and camera button. Its gaps relative to the map are in [hw-map.md](hw-map.md) (the `full` column). The main ones are no host-wake lines, no IRQ for the gauge, no AK8975 compass, and the wake-pad bank not powered (D5).
