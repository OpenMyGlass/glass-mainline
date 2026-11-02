# Mainline bring-up roadmap: reaching Model B parity

The ordered plan to take the mainline Glass XE-C port (Linux 7.2.9, OMAP4430
"notle") from "boots to a smoke-test userspace" to **Model B parity**: a
head-mounted display, touchpad and IMU for `glassd`, audio on the bone-conduction
speaker, battery awareness, and the payoff — a native **V4L2 camera** through
`omap4iss`.

This document is forward-looking. It builds on, and does not repeat, the Phase 0
record: [porting-log.md](porting-log.md) (decisions), [hw-map.md](hw-map.md)
(every peripheral with its 3.4 `file:line`), [safe-dt-review.md](safe-dt-review.md)
(the `safe` DT's gaps D1-D9) and [reset-analysis.md](reset-analysis.md) (why the
kernel resets after ~1 minute). Source conventions are the same: a bare
`file:line` is `arch/arm/mach-omap2/` in Google's 3.4.94 tree (`glass-omap-xrx13b`
@ 989d925), and `mainline:` is Linux 7.2.9. Glass-specific 3.4 display code lives
in `drivers/video/omap2/displays/panel-notle-dpi.c` and `include/video/omap-panel-notle.h`.

**Safety.** Everything here is bound by the ten rules
([porting-log.md](porting-log.md#safety-rules)). Kernels are tried only with
`fastboot boot` (RAM); nothing is flashed; the bootloader, calibration and
charging are untouchable; and the glasshub **reflash** path is never ported.

## The gate: the boot must survive first

Nothing below can be verified on the device until the ~1-minute reset is
understood and fixed, because without the display we are blind and depend on
`/proc/last_kmsg` and serial. The sequence is therefore:

0. **Boot survival (prerequisite, not a parity feature).** Run
   `out/mainline-smoke2-boot.img` (built by `scripts/build-smoke2.sh`): a static
   ARM init logging `OMGB-MAINLINE:` heartbeats to `/dev/kmsg`, now **once a
   second for the first 120 s** so the last beat in `/proc/last_kmsg` dates the
   reset to the second. The candidate carries the one host-side reset fix — the
   four 3.4 always-on rails (VAUX2/VAUX3/VUSIM/VANA) described `always-on`, so
   mainline's ~30 s "disable unused regulators" pass cannot drop them
   (hypothesis **H3**); OMAP WDT2 (**H1**) is already disabled by `omap_wdt` at
   probe, so it is ruled out the moment the first heartbeat appears. It also
   paints an interim display test pattern (see §1 below). Read the result with
   the step-by-step in [reset-analysis.md](reset-analysis.md) ("How to read the
   result after an owner-assisted power-cycle"): **a last `alive beat=N` of ~60
   or more with no `Kernel panic` line means the boot now plausibly survives the
   reset window.** Then work the rest of the H1-H5 ladder. Expected effort:
   **1-3 device sessions** once hardware is at hand. **Until the kernel survives
   several minutes, do not start the driver work below.** (The first candidate,
   `scripts/build-smoke.sh` / `mainline-smoke-boot.img`, is the earlier baseline;
   smoke2 supersedes it.)

Then bring peripherals up in the order of the sections that follow. The order is
chosen so that each step is verifiable with what the previous steps provide, and
so the hardest, most uncertain work (the display) is attacked first while we
still have the budget for it.

## Bring-up order at a glance

| # | subsystem | upstream state | Glass-specific work | port 3.4→mainline? | risk | rough effort |
|---|---|---|---|---|---|---|
| 1 | **Display** (panel + iCE40 + glasshub) | omapdrm DPI exists; no notle panel | write a DRM panel/bridge: load FPGA bitstream, glasshub handshake, timings, gamma, **LED power limits** | **yes, substantial** — rewrite `panel-notle-dpi.c` from omapfb/DSS2 to DRM | **high** | **multi-week** |
| 2 | Touchpad + IMU | RMI4 and inv_mpu6050 upstream; in the `full` DT | enable, verify protocol, wire wake pads, add AK8975 compass | mostly no (DT + verify) | low-med | days |
| 3 | Audio (TWL6040 / McPDM / ABE) | `omap-abe-twl6040` machine driver upstream | DT for McPDM/DMIC/codec; routing to earphone (bone), mic; optional ABE | no (DT + machine config) | medium | 1-2 weeks |
| 4 | Power: fuel gauge + charger | bq27xxx upstream; TWL6030 charger weak in mainline | enable gauge (read-only); **leave charging on Model A** (rule 6) | gauge no; charger deferred | low (by design) | days (gauge) |
| 5 | GPU (SGX540) | **no** open driver; out-of-tree TI SGX DDK-km + closed UM | build DDK-km against 7.2.9, DRM render node, closed `pvr` userspace | separate out-of-tree tree | high | multi-week, optional |
| 6 | **Camera** (omap4iss) | `omap4iss` native since v3.17 | ISS + CSI-2 DT, sensor subdev driver, cam regulators/PWDN, media-ctl pipeline | sensor driver likely new | med-high | multi-week (the payoff) |

Cross-cutting prerequisites (sections at the end): **thermal gate** (D4 — no long
sessions until a PCB zone exists), **DT evolution** (safe → full → parity), and
the **fbdev-vs-DRM decision**.

---

## 1. Display — the hard part, done first

The panel is a Himax HX7309 LCoS, 640x360, driven as **24-bit DPI on DISPC LCD2**
behind a **Lattice iCE40LP4K FPGA** panel controller, with a **glasshub** MCU for
on-head detect, wink, buttons and backlight. Three pieces must come together
before a single correct pixel appears:

1. **The DPI video path.** 3.4 drives it through omapfb/DSS2 with the
   `panel_notle` driver on LCD2, `skip_init` because the bootloader already lit
   it (`board-notle.c:590-605`), timings 640x360 / pixel clock 85.333 MHz / hsw 68
   hfp 10 hbp 10 / vsw 10 vfp 5 vbp 5 (`panel-notle-dpi.c:254-268`), and the 1.1
   gamma table (`board-notle.c:475-546`).
   - **Upstream:** mainline has **omapdrm** (`drivers/gpu/drm/omapdrm/`) with a
     working **DPI output** for OMAP4 DSS. This is the modern replacement for the
     DSS2 omapfb stack the 3.4 driver targets. The DSS core, DISPC and DPI encoder
     are all present and used by other OMAP4 boards.
   - **Glass-specific:** a DRM component — a `drm_panel` (or `drm_bridge`) driver
     — that attaches to the omapdrm DPI output, declares the notle timings/bus
     format, and in its `prepare()`/`enable()` does the FPGA and glasshub work
     below.

2. **The iCE40 bitstream.** At boot the FPGA is configured over **McSPI1 CS0 at
   48 MHz** with the 302468-byte bitstream (`dss_fpga.img`, present under
   `modelb-firmware/`), using the slave-serial pins **FPGA_CRESET_B (GPIO 87)** and
   **FPGA_CDONE (GPIO 85)** (`hw-map.md` Display; `board-notle.c:607-620,
   648-662`). Then its registers are written over SPI: revision 0x00, pipeline
   0x01, LCOS enable 0x03 (the DISP_ENB bit), backlight 0x10 (LED enable and R/G/B
   force) (`panel-notle-dpi.c:52-82`).
   - **Upstream:** there is **no in-tree iCE40 slave-serial loader** suitable here
     (the `fpga-mgr` framework exists but not a notle-shaped SPI config path). The
     load sequence must be ported from the 3.4 driver.
   - **Glass-specific / clean-room:** `dss_fpga.img` is a **proprietary blob** and
     is **never committed** (clean-room rule); it ships in `/lib/firmware` and is
     loaded via `request_firmware()`. The sequencing logic (toggle CRESET_B, clock
     the bitstream, wait CDONE, write the control registers) is ported from
     `panel-notle-dpi.c`.
   - **LED power limits are a safety item, not a nicety.** 3.4 enforces
     `limit_mw` 76, red/green/blue max 63/96/96 mW and the colour-mix ratios
     (`board-notle.c:569-588`, `panel-notle-dpi.c:1388-1399`). **The mainline
     driver must enforce the same limits before it ever enables the LEDs** — this
     sits against the temple and over the eye.

3. **glasshub handshake.** The glasshub MCU (I2C4 0x35) owns on-head/wink/buttons
   and backlight (`board-notle.c:1733-1765`). The panel enable path coordinates
   with it. A mainline glasshub driver is an input/misc + possibly backlight
   driver. **Its reflash path (`glasshub.c:223-245`) is never ported** (rule: "a
   failure to write the boot code is unrecoverable"). `glasshub.s19` is under
   `modelb-firmware/` but we do not flash it.

**Interim stepping stone (low risk, gets pixels early).** The `safe` DT already
keeps the bootloader's running DISPC alive and exposes a `simple-framebuffer` at
0x9c700000, and the init re-points DISPC `GFX_BA0/BA1` there over `/dev/mem`.
Two inits now do this: the busybox `userland/initramfs/init` `display_handoff`
(full/rootfs path), and — new in the boot-survival pass — the freestanding
`tools/smoke-init` in the **smoke2** candidate, which does the same handoff with
the same hardware-proven addresses (CM_DSS_DSS_CLKCTRL 0x4a009120, DISPC GFX_BA0
0x48041080 / BA1 0x48041084, GFX_ATTRIBUTES 0x480410a0, DISPC_CONTROL2 0x48041238
GOLCD2) and then **paints an eight-bar colour test pattern** into the
framebuffer. The mechanism:

1. the bootloader programmed the iCE40 and left the DISPC scanning 0x80500000
   (now occupied by the decompressed kernel — so the prism shows garbage until
   handoff);
2. the init checks CM_DSS IDLEST (reading a gated DISPC faults), re-points
   GFX_BA0/BA1 at the reserved 0x9c700000, forces the format to xRGB, latches
   GOLCD2, and writes pixels there;
3. result: **a static framebuffer that reuses the FPGA the bootloader already
   programmed** — no mode set, no pipeline, but real pixels. In smoke2 the
   colour bars double as the first serial-free "the boot survived to here and
   the display is reachable" signal (`CONFIG_DEVMEM=y`,
   `STRICT_DEVMEM` off, pinned in `config/glass.config`).

This confirms the display is reachable before the DRM driver exists. The full
DRM path (1-3 above) is what enables mode control, the camera preview and GPU
compositing.

**Risk: high. Effort: multi-week** and the single biggest item in the project.
Porting `panel-notle-dpi.c` (≈2400 lines of omapfb/DSS2-era code) to a clean
DRM panel/bridge driver, plus the FPGA loader and glasshub, is a genuine
multi-week subtask even for someone fluent in DRM. Recommend splitting into:
(1a) omapdrm DPI brought up with a dumb static panel using the bootloader's
config; (1b) FPGA loader as a standalone SPI driver tested against CDONE; (1c)
glasshub input/backlight; (1d) the glue panel driver that sequences them and
enforces LED limits.

## 2. Touchpad and IMU — what `glassd` needs

Both are already described in the **`full`** DT (`dts/omap4-glass-xec.dts`) and
have mature upstream drivers; this step is mostly *enable + verify on hardware*.

- **Touchpad:** Synaptics `tm2240`, RMI4 over **I2C3 0x20**, attention active-low
  on GPIO 3, F11, `flip_y`, reset 100 ms (`board-notle.c:1425-1513`). Upstream
  `RMI4_CORE/RMI4_I2C/RMI4_F11/RMI4_F12` are enabled in `config/glass.config` and
  the DT node exists. Powered by **VUSIM 3.0 V** — watch D3 (mainline may lower it
  to 2.9 V). *Work:* verify the reporting protocol (3.4 uses type A; the Model A
  userland found protocol A/B differences — see `glass-tap` history), wire the
  wake pad (GPIO wk3, part of the USIM bank that D5 says is not powered yet).
- **IMU:** InvenSense **MPU-9150 at I2C4 0x68**, INT on GPIO 4
  (`board-notle.c:1607-1622`). Upstream `INV_MPU6050_I2C` (IIO) covers the 9150's
  gyro/accel; the DT node and `mount-matrix` exist. *Work:* the **AK8975 compass**
  behind the MPU (0x0C) is not described — expose it via the inv_mpu aux-I2C mux
  (upstream supports this) if `glassd` wants the magnetometer.

**Port from 3.4? Mostly no.** The drivers are upstream; the effort is DT details,
the wake-pad bank (D5), and on-device verification of the touchpad protocol.
**Risk: low-medium. Effort: days.**

## 3. Audio — TWL6040 / McPDM / ABE

3.4 uses the `omap-abe-twl6040` machine driver ("Notle"): **TWL6040 codec at I2C1
0x4b**, IRQ SYS_NIRQ2, AUDPWRON **GPIO 127**, MCLK 38.4 MHz, DMIC yes, **earpiece =
the bone-conduction output** (the Model A work confirmed this), jack detect on
(`board-notle.c:1298-1319, 1371-1393`).

- **Upstream:** mainline has the whole family — `TWL6040` MFD + codec, OMAP4
  **McPDM** and **DMIC** CPU DAIs, and the **`omap-abe-twl6040`** machine driver
  (`sound/soc/ti/`). `config/glass.config` already builds `TWL6040_CORE`.
- **Glass-specific:** DT nodes for McPDM, the TWL6040 (with AUDPWRON and the
  SYS_NIRQ2 pad, which the safe DT omits), DMIC, and the `ti,model = "Notle"`
  machine node with the routing map (DL1 → earphone/bone, the two Wolfson
  MEMS mics). The Model A **V5 ALSA findings** (`docs/` in the sibling repo;
  the bone output is the TWL6040 *Earphone* output, mixer `DL1`) map directly
  onto the mainline `snd_soc` route names and should seed the `audio-routing`.
- **ABE vs no-ABE:** full ABE needs the DSP/ABE firmware path; a first target is
  **McPDM → TWL6040 without ABE** (simpler, known to work on sdp4430-class
  boards), then add ABE if latency/power needs it.

**Port from 3.4? No** (DT + machine routing, no custom driver). **Risk: medium**
(ABE and the exact mixer routes). **Effort: 1-2 weeks** including on-device mixer
bring-up.

## 4. Power — fuel gauge now, charging never (in mainline)

- **Fuel gauge:** TI **bq27520 at I2C1 0x55** (`board-notle.c:1580-1593`).
  Upstream `BATTERY_BQ27XXX_I2C` is enabled and the `full` DT has the node (no IRQ
  yet — GPIO 13 SOC_INT is undescribed). *Work:* read-only state-of-charge for
  `glassd`/UI; optionally wire the IRQ. **Low risk, days.**
- **Charger:** 3.4's `twl6030_charger` pets a **32 s charger watchdog**
  (`twl6030_charger.c:560-563`) and sets charge currents from verified values.
  Mainline's TWL6030 charging support is weak, and **safety rule 6 forbids
  touching charge current or the charger without 3.4-verified parameters**. The
  decision stands: **charging stays disabled in mainline; the Glass charges under
  Model A.** This also means mainline sessions run on battery and must be short
  (ties into the thermal gate). Not a parity blocker for a tethered device.

## 5. GPU — SGX540 (optional, defer)

XE24 ran the **SGX540** with TI's DDK 1.9@2166536 (closed). Mainline has **no open
SGX driver**; the path is the **out-of-tree TI SGX DDK-km** kernel module plus the
closed `pvr` userspace, built against this kernel, exposed as a DRM render node
beside omapdrm's display node.

- **Upstream:** display/KMS comes from omapdrm (section 1); the GPU is entirely
  out-of-tree. The community `img-sgx`/`ti-img-rogue`-style trees target SGX; a
  540-era DDK-km must be matched to 7.2.9's DRM/GEM and build system.
- **Why defer:** Model B's camera preview and UI composite through **DRM dumb
  buffers / the display pipeline without GL**. The GPU is only needed for
  GL-accelerated rendering. **Risk: high, effort: multi-week, and optional** — do
  it after display+camera prove out, if the UI needs GL.

## 6. Camera — the payoff (omap4iss, native V4L2)

The whole reason mainline is the Model B kernel target: OMAP4 has the **ISS**
(Imaging SubSystem) with a native driver, **`omap4iss`**
(`drivers/media/platform/ti/omap4iss/`), in mainline **since v3.17** — no Ducati,
no DOMX, a real V4L2 device.

- **Upstream:** `omap4iss` (ISS + CSI-2 receiver) is present and maintained. It
  exposes the ISS as V4L2 subdevices wired with the Media Controller.
- **Glass-specific:**
  - **DT:** an `iss` node (it is in `omap4.dtsi`/`omap443x.dtsi` as the ISS target
    module), the CSI-2 port with the sensor's lane mapping, and the camera
    regulators/GPIOs — **VAUX2 2.8 V (`cam2pwr`)** and **CAM_PWDN GPIO 100**
    (`board-notle.c:833-859, 1214-1223`; note D5: 3.4 drives GPIO 100 low at boot
    to power the sensor *down*, so the sequence must be inverted deliberately for
    capture).
  - **Sensor subdev:** 3.4 drove the sensor through the Ducati firmware, so there
    is **no kernel sensor driver to port** — the mainline path needs a V4L2
    subdev for the actual sensor. The Model A work reports an **OV5680**; if so,
    an `ov5680`/`ov5693`-class driver must be adapted or written (clock, CSI-2
    lanes, power sequence). Confirm the real part first (Phase 1 read / teardown).
  - **Pipeline:** bring-up is the usual `media-ctl` graph (sensor → CSI-2 → ISS)
    and a `v4l2` capture test, then hand frames to the display (section 1) for a
    native preview.

**Port from 3.4? The ISS is upstream; the sensor subdev is likely new/adapted.**
**Risk: medium-high** (sensor identification and CSI-2 lane/clock details).
**Effort: multi-week**, but it is the deliverable that justifies the whole kernel.

---

## Cross-cutting

### Thermal gate (D4, rule 5)

Mainline has **none of 3.4's thermal layers** (PCB sensor on GPADC ch6, case
governor, duty-cycle CPU cap) — only the bandgap zone with a passive trip that has
no cooling device loaded ([safe-dt-review.md](safe-dt-review.md) D4). Until a
**PCB-temperature thermal zone** exists (TWL6030 GPADC ch6 → `twl6030-gpadc` IIO →
`generic-adc-thermal`, with the 3.4 polynomial as a lookup table and the 3.4 trip
points, and any cpufreq capped at the 3.4 policy), **no long mainline sessions and
never on the head.** This zone should land early — it is a safety prerequisite for
the sustained sessions that display/camera testing need.

### Device-tree evolution

The DTs progress in three stages, all citing 3.4 sources (rule 4):

1. **`safe`** (today): memory, PMIC, USB gadget, ramoops; no MMC. The first-boot
   image.
2. **`full`** (today): + eMMC (MMC2), BCM4330 Wi-Fi/BT (MMC5/UART2), touchpad,
   IMU, fuel gauge, camera button. This is the Model A-equivalent hardware set.
3. **parity** (this roadmap): + display (panel/FPGA/glasshub), audio
   (McPDM/TWL6040), camera (ISS/CSI-2/sensor), and the thermal zone. Plus the
   Phase 1-gated fixes **D1-D9** (the 2 GB secure carve-out, the always-on
   regulators, VUSIM 3.0 V, GPIO 100, MSECURE pad, 384 kHz I2C) once the on-device
   readings confirm them.

### fbdev vs DRM/KMS

Mainline OMAP display **is DRM** (`omapdrm`); omapfb/DSS2 is gone. So the display
work is necessarily a **DRM port**, not a straight copy of the 3.4 omapfb
`panel-notle-dpi.c`. A `CONFIG_DRM_FBDEV_EMULATION` `/dev/fb0` can still back the
existing framebuffer-based Model A userland during transition, but the native
driver is a `drm_panel`/`drm_bridge` on the omapdrm DPI output.

## Honest effort estimate

This is a **multi-month** effort, not a weekend. Rough shape, assuming hardware is
at hand and the boot-survival gate is passed:

- **Boot survival + thermal gate:** ~1-2 weeks (mostly on-device diagnosis;
  Phase 1 readings + the reset ladder are already specified).
- **Touchpad/IMU + fuel gauge:** ~1 week (enable/verify).
- **Audio:** ~1-2 weeks.
- **Display (the dominant risk):** **several weeks**, realistically the long pole;
  the interim `/dev/mem` framebuffer buys early pixels while the DRM panel/FPGA/
  glasshub driver is written.
- **Camera (omap4iss + sensor):** **several weeks**, gated on knowing the sensor
  part and on the display for preview.
- **GPU (SGX):** optional, multi-week, only if GL is required.

A reasonable **minimum viable Model B** (display via the DRM path, touchpad/IMU,
audio, gauge, and the V4L2 camera, no GPU) is on the order of **2-4 months** of
focused kernel work. The display and camera dominate; everything else is DT and
verification on top of upstream drivers. The value of doing display first is that
it is both the hardest and the thing that unblinds every later step.
