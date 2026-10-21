# Hardware map of Glass XE-C, from the 3.4 sources

Phase 0 document. It describes what Google's 3.4 kernel does with the hardware, and whether the mainline device trees in `dts/` describe the same thing. Nothing here has been checked on a device yet. Phase 1 (an inactive draft, [future-phases-draft.md](future-phases-draft.md)) lists what to read on the device to confirm it.

## Sources and conventions

- **3.4 source:** Google `kernel/omap`, branch `glass-omap-xrx13b`, commit `989d925476700c0d07a75008b37725cbc558f862` (Linux 3.4.94), GPL-2.0. A bare `file:line` such as `board-notle.c:1932` means `arch/arm/mach-omap2/<file>` at that commit. Other paths are given from the tree root. Upstream's own comments cite GlassHack/factory-kernel@1091b53 (3.4.83), so its line numbers do not match the ones here. Each fact below was looked up again at 989d925.
- **The stock kernel** on XE24 is 3.4.94 `-01990-g1cee760235d`, which is not exactly 989d925 (OpenMyGlass/glass-kernel, `docs/gpu-module.md`). Small differences between that build and this source are possible. Phase 1 compares them (`/proc/config.gz`, `dmesg`).
- **Mainline:** Linux 7.2.9 from kernel.org, verified by `scripts/fetch-kernel.sh`. `mainline:<path>:<line>` refers to that tree.
- **Board revision:** the 3.4 code picks GPIOs by board revision, which it reads from GPIO 34/40/42 (`board-notle.c:157-190`). XE-C is a production unit, so the EVT2-and-later table applies (`board-notle.c:134-154`). The mainline DTs assume the same. This is **unverified** until the stock `dmesg` line `Notle board revision: ...` (`board-notle.c:2262`) is read.
- **GPIO numbers** are OMAP4 GPIO numbers. GPIO *n* is `<&gpio(n/32+1) (n%32)>` in DT. `gpio_wkN` is GPIO *N* in the wakeup pad bank.
- **Pad offsets** are the `OMAP4_CTRL_MODULE_PAD_*_OFFSET` values of `mux44xx.h` (3.4), the same numbers that mainline's `OMAP4_IOPAD()` takes. Every pad the mainline DTs use was checked against `mux44xx.h`, and all of them match.
- **Columns `safe` and `full`:** whether `dts/omap4-glass-xec-safe.dts` (with `omap4-glass-xec-common.dtsi`) and `dts/omap4-glass-xec.dts` describe the item. **yes** means described the same way, **differs** means described differently (see [safe-dt-review.md](safe-dt-review.md)), **no** means not described, so mainline leaves it as the bootloader set it, or switches it off if it is "unused" (see [reset-analysis.md](reset-analysis.md)), and **n/a** means nothing to describe.

## SoC, memory, boot

| item | 3.4 value | source | safe | full |
| --- | --- | --- | --- | --- |
| SoC | OMAP4430, machine `NOTLE`, CBS package (CBL on ES1.0) | `board-notle.c:2195-2200, 2466` | yes (`omap443x.dtsi`) | yes |
| RAM size | read from the DMM LISA map: 1 GB or 2 GB; 2 GB units use two Elpida 4G-S4 LPDDR2 parts | `board-notle.c:2185-2191, 2228-2241` | placeholder 1 GB, replaced by the bootloader's ATAG_MEM (`ARM_ATAG_DTB_COMPAT`) | same |
| EMIF timings | Elpida 4G-S4 x2 tables for EMIF1 and EMIF2. Self-refresh after 512 cycles, and the performance timeout only above 400 MHz | `board-notle.c:2176-2183, 2233-2236` | no (mainline leaves the EMIF as the bootloader set it) | no |
| RAM console | 0x80000000 + 512 MB = **0xA0000000**, 2 MB, Android persistent_ram with ECC (128-byte blocks, 16 parity bytes) | `omap_ram_console.h:21-22`, `board-notle.c:2398-2399`, `drivers/staging/android/persistent_ram.c:185-188`, `ram_console.c:58` | ramoops at the same place (dmesg, console and pmsg zones) | same |
| Secure (SMC) workspace | **0x80000000 + total RAM − 3 MB**, 3 MB, removed from the memory map (`memblock_remove`) and handed to the secure driver. On 2 GB that is near the top of RAM. The TI default of 0xBFD00000 is for 1 GB | `board-notle.c:2362-2365, 2382, 2461`; `omap4_ion.h:24-25`; `omap-secure.c:219-250`; `include/mach/omap-secure.h:139-148` | **differs**: reserves 0xBFD00000 (the 1 GB value) | **differs** |
| Carve-outs below the SMC area | ION secure input 60 MB, tiler 71 MB, non-secure tiler 3 MB, multimedia 320 MB (when RAM > 1 GB), IPU 107 MB, DSP 50 MB (if enabled) | `omap4_ion.h:21-25`, `board-notle.c:2366-2388` | no (not needed until the Ducati/DSP work) | no |
| Kernel command line (stock) | `console=ttyO2,115200n8 vmalloc=500M ...`, passed by the bootloader as an ATAG (`CONFIG_CMDLINE=""`) | glass-kernel facts; `notle_defconfig:509` | the mainline boot.img passes its own line; see [reset-analysis.md](reset-analysis.md) H1 | same |
| Restart | `omap_prcm_restart` | `board-notle.c:2476` | mainline default (PRM) | same |
| Off mode, SmartReflex | enabled at the end of init | `board-notle.c:2337-2338` | no (mainline has no voltage scaling consumer on OMAP4, see Thermal and CPU) | no |

## PMIC: TWL6030 and its regulators

The TWL6030 sits on I2C1 at 0x48, and its interrupt is SYS_NIRQ1 (`twl-common.c:43-47, 105-113`). I2C1 runs at **384 kHz** (`twl-common.c:120-121`). `regulator_has_full_constraints()` is called (`board-notle.c:1944`), so 3.4 too switches off unused regulators that are not `always_on`. The list below is therefore exactly what 3.4 keeps on.

| regulator | 3.4 constraint | 3.4 always on? | 3.4 user (comment) | source | mainline `twl6030.dtsi` (+ common.dtsi) | safe | full |
| --- | --- | --- | --- | --- | --- | --- | --- |
| VAUX1 | 2.9 V fixed, apply | **yes** | eMMC (`vmmc` of omap_hsmmc.1) | `board-notle.c:811-830` | 2.9 V, always-on (common.dtsi) | yes | yes |
| VAUX2 | 2.8 V fixed, apply | **yes** | "sensors (mag, acc, gyro, light sensor, camera)", `cam2pwr`, ABE `av-switch` | `board-notle.c:833-859` | range 1.2-2.8 V, not always-on | **differs** | **differs** |
| VAUX3 | 1.2 V fixed, apply | **yes** | none named | `board-notle.c:861-876` | range 1.0-3.0 V, not always-on | **differs** | **differs** |
| VMMC | 1.8 V fixed, apply. Off in suspend | no | the gpio_100 camera power-down pad (MMC1 pad bank) | `board-notle.c:898-914` | range 1.2-3.0 V | yes (not always-on either) | yes |
| VPP | 1.8-2.5 V | no | "unused" | `board-notle.c:917-933` | 1.8-2.5 V | yes | yes |
| VUSIM | **3.0 V** fixed, apply | **yes** | touchpad | `board-notle.c:935-951` | range 1.2-**2.9 V**, not always-on | **differs** (see review) | **differs** |
| VANA | 2.1 V (fixed LDO) | **yes** | "unused" | `board-notle.c:953-971` | fixed 2.1 V, not always-on | **differs** | **differs** |
| VCXIO | 1.8 V (fixed LDO) | **yes** | "clocks, plls, etc." | `board-notle.c:973-991` | always-on | yes | yes |
| VDAC | 1.8 V (fixed LDO) | no | "A/D convertor?" | `board-notle.c:993-1010` | not always-on | yes | yes |
| VUSB | 3.3 V | no | the TWL6030 USB PHY (`vusb` of twl6030_usb) | `board-notle.c:1012-1036` | fixed 3.3 V, used by `twl_usb_comparator` | yes | yes |
| V2V1 | 2.1 V | **yes** | TWL6040 `v2v1` (1-004b) | `board-notle.c:1038-1058` | always-on | yes | yes |
| V1V8 (SMPS4) | 1.8 V | **yes** | TWL6040 `vio` (1-004b) | `twl-common.c:598-618, 778-783`; `board-notle.c:1929-1930` | always-on | yes | yes |
| CLK32KG | (a "regulator" in 3.4) | **yes** | the 32 kHz clock: the BCM4330 LPO, which Bluetooth also enables | `board-notle.c:1060-1071`; `board-notle-bluetooth.c:48-49, 88, 100` | a clock (`CLK_TWL`, `#clock-cells`), not always-on | **differs** (unused clock) | used by the Wi-Fi pwrseq and BT |
| VCORE1-3 | set by the voltage controller over the SR I2C, with SmartReflex | | | `opp4xxx_data.c:94-128` | `mainline:arch/arm/mach-omap2/omap_twl.c` registers the PMIC, but no DT consumer scales voltage (no `cpu-supply`) | n/a | n/a |

Other TWL6030 functions:

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Charger (TWL6030 CHARGERUSB) | `CONFIG_CHARGER_NOTLE` (`twl6030_charger.c`). Limits: 1500 mA, 4560 mV input, battery 4200 mV, low 3300 mV, polled every 15 s. A **charger watchdog of 32 s is petted** by the driver | `board-notle.c:1343-1350`; `notle_defconfig:1718`; `drivers/power/twl6030_charger.c:38, 560-563, 628, 1010, 1278-1280` | no (`CHARGER_TWL6030=m`, and no module ships, so it never loads) | no |
| Battery temperature table | EVT1 only (on PMIC GPADC) | `board-notle.c:1352-1368, 1887-1889` | n/a | n/a |
| GPADC | `twl4030_madc_platform_data` irq_line 1. The PCB thermistor is **GPADC channel 6** | `board-notle.c:1395-1397`; `drivers/staging/thermal_framework/sensor/notle_pcb_sensor.c:68-70, 166-172` | node present (`twl6030.dtsi`), driver `=m`, so not loaded | same |
| Power button, RTC, poweroff | `INPUT_TWL6030_PWRBUTTON`, `RTC_DRV_TWL4030`, `TWL6030_POWEROFF` | `notle_defconfig:1497, 2458, 1893` | RTC yes, `system-power-controller` yes | same |
| MSECURE pad | not muxed by notle | `board-notle.c:1986-2019` (absent) | **differs**: `twl6030_omap4.dtsi` sets `fref_clk0_out` to `sys_drm_msecure` (mode 2) | same |

## Thermal and CPU

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| MPU OPPs (4430) | 300 MHz/1.025 V, 600/1.2 V, 800/1.325 V, 1008/1.388 V enabled. 1200 MHz present but disabled | `opp4xxx_data.c:94-109, 225-234` | `omap443x.dtsi:14-20` lists 300/600/800/1008 at 1.025/1.2/1.313/1.375 V, but `CPUFREQ_DT=m` never loads and there is no `cpu-supply`, so the CPU stays at whatever rate the bootloader left | same |
| Duty-cycle governor | PCB-temperature sections: 1008 MHz capped at 37 % above 30 °C PCB, 800 MHz cap at 40 °C, 600 MHz cap from 70 °C, down to 1 % "nitro" at 85 °C | `board-notle.c:229-388, 448` | **no** | **no** |
| Case governor | hot 62 °C, cold 57 °C | `drivers/staging/thermal_framework/governor/case_governor.c:31-35` | **no** | **no** |
| Die governor | monitor 85, alert 100, critical 110, shutdown 125 °C | `drivers/staging/thermal_framework/governor/omap_die_governor.c:45-51` | partly: `omap4-cpu-thermal.dtsi` passive 100 °C (no cooling device is loaded), critical 125 °C | same |
| PCB sensor | `notle_pcb_sensor`, GPADC channel 6, fifth-degree polynomial | `board-notle.c:1331-1333, 2309`; `notle_pcb_sensor.c:72-107` | **no** | **no** |
| Bandgap | `OMAP_BANDGAP`, `OMAP4_BG_TEMP_SENSOR_DATA` | `notle_defconfig:1835-1836` | `omap443x.dtsi:34-41` (4430 has no TSHUT feature in mainline, `mainline:drivers/thermal/ti-soc-thermal/omap4-thermal-data.c:58-62`) | same |

## Buses and the devices on them

| bus / device | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| I2C1 | 384 kHz, hwspinlock 0: TWL6030 0x48, TWL6040 0x4b, bq27520 0x55 | `twl-common.c:43-51, 120-121`; `board-notle.c:1586-1593, 1870` | enabled at **400 kHz**, TWL6030 only | +bq27520 |
| I2C2 | 384 kHz, no devices | `board-notle.c:1934` | enabled at 400 kHz, no devices | same |
| I2C3 | 384 kHz: Synaptics RMI4 `rmi_i2c` 0x20 | `board-notle.c:1595-1602, 1935-1936` | enabled at 400 kHz, no devices | +touchpad |
| I2C4 | 384 kHz: panel `panel-notle-panel` 0x49, MPU-9150 0x68 (AK8975 compass behind it at 0x0C), LTR-506ALS 0x3a, glasshub 0x35 | `board-notle.c:1737-1765, 1608-1622, 1937-1938` | enabled at 400 kHz, no devices | +MPU-9150 only |
| McSPI1 CS0 | iCE40 FPGA (`ice40-spi`), 48 MHz, master, single channel | `board-notle.c:607-620, 674-677` | no | no |
| UART2 (ttyO1) | Bluetooth, CTS/RTS/TX/RX, no DMA, `wake_peer` for BT LPM | `board-notle.c:1099-1119, 1189-1197, 1227` | no | yes (serdev `brcm,bcm4330-bt`) |
| UART3 (ttyO2) | debug console, CTS/RTS/TX/RX, RX is a wake pad | `board-notle.c:1121-1141, 1198-1204, 1228` | RX/TX pins only, `stdout-path` | same |
| UART4 (ttyO3) | GPS, TX/RX | `board-notle.c:1143-1155, 1205-1211, 1229` | no | no |
| UART1 | defined, never initialised | `board-notle.c:1075-1097, 1225-1230` | no | no |
| MMC2 | eMMC, 8-bit, 1.8 V DDR, non-removable, OCR 2.9-3.0 V, `no_off_init` | `board-notle.c:707-721` | **disabled** (on purpose) | yes |
| MMC5 | BCM4330 SDIO (`bcm4329` slot), 4-bit, OCR 1.65-1.95 V and 2.0-2.1 V, keep power | `board-notle.c:722-733` | disabled | yes |
| MMC1 | only on "Dog" devices: blanked unless `CONFIG_MMC_NOTLE` | `board-notle.c:734-744, 2274-2277` | disabled | disabled |
| USB OTG (MUSB) | UTMI, **peripheral** (no `CONFIG_USB_OTG`), power 100 mA | `board-notle.c:692-700, 2295` | yes (`interface-type 1`, `mode 2`, `power 50` = 100 mA in 2 mA units) | yes |
| USB PHY | TWL6030 USB, empty platform data | `board-notle.c:702-705` | yes (`twl_usb_comparator`, `vusb`) | yes |

## GPIOs (EVT2 and later)

| GPIO | function | pad, mux (EVT2) | source | safe | full |
| --- | --- | --- | --- | --- | --- |
| 34, 40, 42 | board ID 0-2 (input, pull-up) | gpmc_ad10, gpmc_a16, gpmc_a18, mode 3 | `board-notle.h:31-41`; `board-notle.c:157-190` | no | no |
| 46 | USB_MUX_CB0 | gpmc_a22 (0x06c), mode 3, out | `board-notle.h:74`; `board-notle.c:1989` | yes, hog high | yes |
| 45 | USB_MUX_CB1 | gpmc_a21 (0x06a), mode 3, out | `board-notle.h:72`; `board-notle.c:1990` | yes, hog low | yes |
| 43 | WL_RST_N (also switches the 2.0 V `vbcm4329` supply) | gpmc_a19 (0x066), mode 3, out | `board-notle.h:99`; `board-notle.c:1988`; `board-notle-wifi.c:172-180, 373-378, 397` | no | yes (regulator-fixed 2.0 V, 70 ms) |
| 48 | WL_BT_REG_ON | gpmc_a24 (0x070), mode 3, out | `board-notle.h:100`; `board-notle.c:1991`; `board-notle-bluetooth.c:37, 89, 98` | no | yes, hog high |
| 36 | BCM_BT_WAKE | gpmc_ad12 (0x058), mode 3, out | `board-notle.h:101`; `board-notle.c:1987` | no | yes |
| 113 | BT_RST_N | abe_mcbsp2_fsx (0x0fc), mode 3, out | `board-notle.h:87`; `board-notle.c:1997` | no | yes |
| 2 (wk2) | BCM_BT_HOST_WAKE | sim_reset, mode 3, in, wake | `board-notle.h:105`; `board-notle.c:2015` | no | no (BT without host-wake) |
| 0 (wk0) | BCM_WLAN_HOST_WAKE (SDIO OOB IRQ) | sim_io, mode 3, in, wake | `board-notle.h:96`; `board-notle.c:2010`; `board-notle-wifi.c:360-361, 398` | no | no (in-band SDIO IRQ) |
| 97 | `GPIO_BCM_WLAN_HOST_WAKE_HOG` (defined, not used at 989d925) | | `board-notle.h:97` | no | no |
| 170 | BCM_WLAN_WAKE, requested high and then freed | usbb2_hsic_strobe, mode 3, out | `board-notle.h:98`; `board-notle.c:1996`; `board-notle-wifi.c:367-384` | no | no |
| 3 (wk3) | TOUCHPAD_INT_N, level low, wake | sim_cd (0x046 wkup), mode 3, pull-up, wake | `board-notle.h:92`; `board-notle.c:2012, 2090-2109` | no | yes |
| 4 (wk4) | MPU9000_INT, wake | sim_pwrctrl (0x048 wkup), mode 3, in, wake (board comment: "MPU9000_INT_TIMER") | `board-notle.h:71`; `board-notle.c:2014, 2111-2144` | no | yes |
| 1 (wk1) | BLINK_INT (glasshub) | sim_clk, mode 3, pull-up | `board-notle.h:106`; `board-notle.c:2013, 2146-2161` | no | no |
| 30 (wk30) | camera button, KEY_CAMERA, active low, wake, 5 ms debounce | fref_clk3_req (0x056 wkup), mode 3, pull-up, wake | `board-notle.h:90`; `board-notle.c:1249-1273, 2011` | no | yes |
| 103 | ALS/PROX INT (LTR-506) | sdmmc1_dat1, mode 3, pull-up | `board-notle.h:94`; `board-notle.c:1999` | no | no |
| 13 | SOC_INT (bq27520), input, wake IRQ | dpm_emu2, mode 3, pull-up | `board-notle.h:107`; `board-notle.c:2001, 1836-1866` | no | no (gauge without its IRQ) |
| 31 (wk31) | BAT_LOW | fref_clk3_out, **mode 7 (safe)**, so not used | `board-notle.h:108`; `board-notle.c:2016` | no | no |
| 100 | CAM_PWDN, driven **low at boot** ("properly powering down the camera sensor"), off-mode output low | sdmmc1_clk, mode 3, out | `board-notle.h:89`; `board-notle.c:1998, 1214-1223, 2292` | **no** | **no** |
| 94 | LCD_RST_N, requested high (DSS `reset_gpio`) | usbb1_ulpitll_dat6, mode 3, out | `board-notle.h:82`; `board-notle.c:1995, 634-636` | no (left alone on purpose) | no |
| 84 | DISP_ENB (EVT1 only) | | `board-notle.h:83`; `board-notle.c:462-473, 626-632` | n/a | n/a |
| 85 | FPGA_CDONE, input | usbb1_ulpitll_stp, mode 3, in | `board-notle.h:84`; `board-notle.c:2002, 648-650` | no | no |
| 87 | FPGA_CRESET_B, requested high | usbb1_ulpitll_nxt, mode 3, out | `board-notle.h:85`; `board-notle.c:2003, 660-662` | no (left alone on purpose) | no |
| — | FPGA_CBSEL0, FPGA_CBSEL1 | sys_pwron_reset_out, usbb1_ulpitll_dat7, mode 7 | `board-notle.c:2004, 2017` | no | no |
| 139, 140, 138 | GPS ON_OFF (low), RESET_N (high), AWAKE (in); functional only on V1.5 | mcspi1_cs2, cs3, cs1, mode 3 | `board-notle.h:76-80`; `board-notle.c:1992-1994, 2047-2088` | no | no |
| 127 | TWL6040 AUDPWRON | | `board-notle.h:95`; `board-notle.c:1384, 1391, 2024-2031` | no | no |
| 7, 8 | `notleboard::status1/2` LEDs (heartbeat, mmc0), a leftover from the TI board file | | `board-notle.c:419-443, 1322` | no | no |
| — | SYS_NIRQ1 (TWL6030 IRQ) | sys_nirq1 (0x19e), mode 0, pull-up, wake | `board-notle.c:2005` | yes (`twl6030_omap4.dtsi`) | yes |
| — | SYS_NIRQ2 (TWL6040 IRQ) | sys_nirq2, mode 0, pull-up | `board-notle.c:2029` | no | no |

Pad setup that is not a GPIO:

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| USIM pad bank power (GPIOWK_IO_PWRDNZ) | set after muxing, "before you set it to input": the wakeup GPIOs above (wk0-wk4) are in this bank | `board-notle.c:2247-2250` | **no** | **no** |
| PBIASLITE | MMC1 and USIM pad banks powered, at 1.8 V ("camera power down on gpio_100 needs the correct voltage of 1.8V") | `board-notle.c:2252-2259` | **no** | **no** |

## Display

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Panel | `notle_nhd_panel`, driver `panel_notle`, DPI 24-bit on **LCD2**, `skip_init` (the bootloader left it running) | `board-notle.c:590-605` | `simple-framebuffer` at 0x9c700000 on the running DISPC. The DSS target module is disabled so that nothing resets it | same |
| Timings | 640x360, pixel clock 85.333 MHz, hsw 68 hfp 10 hbp 10, vsw 10 vfp 5 vbp 5 | `drivers/video/omap2/displays/panel-notle-dpi.c:254-268` | (in the comment only) | same |
| iCE40 FPGA | registers over SPI: revision 0x00, pipeline 0x01, LCOS 0x03 (DISP_ENB bit), backlight 0x10 (LED enable, R/G/B force) | `panel-notle-dpi.c:52-82` | no | no |
| LED power limits | `limit_mw` 76, red/green/blue max 63/96/96 mW, colour mix 2444/4444/3111 (‱). These are what the 3.4 brightness maths scales by. **A future panel driver must enforce them** | `board-notle.c:569-588`; `panel-notle-dpi.c:1388-1399, 1531-1533` | n/a | n/a |
| Gamma | the 1.1 table, enabled | `board-notle.c:475-546, 586-587` | no | no |
| Panel on I2C4 0x49 | `panel-notle-i2c` driver | `board-notle.c:1738-1740`; `panel-notle-dpi.c:2401-2411` | no | no |
| Framebuffer | 640x360x4, 3 buffers | `board-notle.c:2164-2174` | one 640x360x4 buffer | same |
| GPU | SGX540, DDK 1.9@2166536 on XE24 (OpenMyGlass/glass-kernel) | `board-notle.c:2348-2358` | no (`sgx_module` exists in `omap4.dtsi`, with no driver for it) | no |

## Touchpad, sensors, glasshub

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Touchpad | Synaptics `tm2240`, RMI4 over I2C3 0x20, attention active low, level-triggered, reset delay 100 ms, F11 with `flip_y`, type A, no-sleep off. Powered by VUSIM 3.0 V | `board-notle.c:1425-1513, 1596-1601, 935-951` | no | yes (`syna,rmi4-i2c`) |
| IMU | InvenSense MPU-9150 at I2C4 0x68, `int_config` 0x10, level shifter, orientation {0,1,0; 0,0,1; 1,0,0}, AK8975 compass at 0x0C with orientation {1,0,0; 0,0,-1; 0,1,0} | `board-notle.c:1607-1622, 1741-1745` | no | yes (compass not described) |
| Light/proximity | LTR-506ALS at I2C4 0x3a, IRQ GPIO 103, ALS gain 3, PS gain 3, LED 60 kHz, 50 %, 5 mA, 127 pulses | `board-notle.c:1624-1731, 1746-1752` | no | no (no mainline driver) |
| glasshub MCU | I2C4 0x35, IRQ on BLINK_INT (GPIO 1). The 3.4 driver can reflash both the hub's application and its **bootloader** ("a failure to write the boot code is unrecoverable"). **Never port or use that path** | `board-notle.c:1733-1765, 2146-2161`; `drivers/input/misc/glasshub.c:51-57, 223-245` | no | no |
| Gas gauge | TI bq27520 at I2C1 0x55, SOC_INT IRQ (GPIO 13). EVT2 translates temperature, EVT3+ does not | `board-notle.c:1580-1593, 1835-1866, 1891-1921` | no | yes (no IRQ, no charger) |

## Wi-Fi and Bluetooth (BCM4330)

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Wi-Fi | bcmdhd on MMC5. Supply `vbcm4329` 2.0 V fixed, switched by WL_RST_N (GPIO 43), 70 ms start-up, off at boot. Power on/off and reset waits of 100 ms and 200 ms | `board-notle-wifi.c:128-145, 159-185, 187-212, 350-403`; `notle_defconfig:1373-1376` | no | yes (brcmfmac) |
| Bluetooth | `bcm4330_bluetooth` rfkill: CLK32KG enabled, then WL_BT_REG_ON and BT_RST_N high. LPM over BT_WAKE / BT_HOST_WAKE | `board-notle-bluetooth.c:37-41, 83-106`; `board-notle.c:1243-1246` | no | yes (serdev, no LPM) |

## Audio, camera, GPS

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Codec | TWL6040 at I2C1 0x4b, IRQ SYS_NIRQ2, AUDPWRON GPIO 127. HS/HF steps 0x0f/0x1d | `twl-common.c:48-50, 115-118`; `board-notle.c:1371-1393` | no | no |
| Machine driver | `omap-abe-twl6040` "Notle": ABE, MCLK 38.4 MHz, DMIC yes, earpiece yes (the bone-conduction output, upstream found), HS mic no, jack detection on | `board-notle.c:1298-1319`; README (upstream) | no | no |
| Other codecs | `dmic-codec`, `spdif-dit` | `board-notle.c:1289-1296` | no | no |
| Camera | sensor power-down GPIO 100 (low at boot), supply VAUX2 2.8 V (`cam2pwr`). The sensor is driven by the Ducati firmware (no kernel sensor driver in the board file). Upstream reports OV5680 | `board-notle.c:833-859, 1214-1223` | no | no |
| Remote processors | IPU (Ducati) and DSP heaps, `omap_rproc_reserve_cma` | `board-notle.c:2367-2397, 2462` | no | no |
| GPS | SiRF (`gps_elton`) on UART4, GPIO 138-140, functional only on V1.5 | `board-notle.c:1275-1288, 2047-2088` | no | no |

## USB connector mux (notle-usb-mux.c)

| item | 3.4 | source | safe | full |
| --- | --- | --- | --- | --- |
| Modes | floating 0, usb 1, tty 2, audio 3 | `notle-usb-mux.c:50-53` | n/a | n/a |
| GPIO states | floating: CB1=0 CB0=0. **usb: CB1=0 CB0=1**. tty: CB1=1 CB0=0. audio: CB1=1 CB0=1 | `notle-usb-mux.c:163-185` | usb (hogs) | usb |
| Initial state | both low (floating) at request | `board-notle.c:1515-1524` | n/a | n/a |
| Choice | USB when a host is online or `force_usb`, else tty if `tty_priority`, else the requested mode | `notle-usb-mux.c:150-158` | always USB | always USB |
| tty_priority | true only with `CONFIG_FIQ_DEBUGGER_CONSOLE_DEFAULT_ENABLE` | `notle-usb-mux.c:103-107` | n/a | n/a |
| Factory cable | ID pin above **4200 mV** reboots into the bootloader, unless the factory-cable boot flag is set (`androidboot.mode=factorycable`, glass-kernel facts) | `notle-usb-mux.c:38, 98-102, 145-149` | not implemented (mainline does not read ID) | same |
