// SPDX-License-Identifier: GPL-2.0-only
/*
 * drm_panel driver for the Google Glass XE-C ("notle") near-eye display.
 *
 * The panel is a 640x360 LCoS driven as 24-bit DPI from the OMAP4 DISPC,
 * behind a Lattice iCE40 FPGA reached over McSPI1 CS0. This driver is the SPI
 * client for that FPGA: it registers a drm_panel that omapdrm's DPI output
 * binds to via the OF graph (of_drm_find_panel -> drm_panel_bridge). On
 * prepare() it loads the FPGA bitstream (panel-notle-ice40.c); on enable() it
 * enforces the LED power limits and lights the backlight.
 *
 * Ported from Google's 3.4 omapfb driver panel-notle-dpi.c (commit 989d925 of
 * OpenMyGlass/kernel-omap, branch glass-omap-xrx13b), GPL-2.0:
 *   notle_config timings            (3.4: :254-274)
 *   led_config_to_linecuts()        (3.4: :1371-1432)  -> notle_led_apply()
 *   panel_notle_power_on()/_off()   (3.4: :1982-2165)  -> prepare/enable/...
 *
 * SAFETY. This display sits over the eye. The LED power caps (limit_mw 76,
 * per-channel max 63/96/96 mW, mix ratios 2444/4444/3111 of 10000) from
 * board-notle.c:569-588 are enforced in notle_led_apply() BEFORE the LEDs are
 * ever enabled. They must never be bypassed. See docs/mainline-bringup.md.
 *
 * STATUS (no device yet): the FPGA SPI path (load, revision, DISP_ENB,
 * pipeline/backlight, LED-limit linecuts) is complete and compiles. The LCoS
 * per-pixel register init that 3.4 writes over the separate I2C4 0x49 panel
 * subdevice (panel_init_regs/gamma) is NOT done here; it needs that I2C
 * subdevice wired and is a documented device-iteration follow-up. Timing and
 * bus polarity need on-device confirmation. Do not assume this lights a
 * correct image yet.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/math64.h>
#include <linux/media-bus-format.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include "panel-notle-ice40.h"

#define NOTLE_MAX_BRIGHTNESS	0xff

/* Exact DPI timings from 3.4 notle_config (panel-notle-dpi.c:254-274). */
static const struct drm_display_mode notle_mode = {
	.clock		= 85333,		/* kHz (85.333 MHz) */
	.hdisplay	= 640,
	.hsync_start	= 640 + 10,		/* + hfp */
	.hsync_end	= 640 + 10 + 68,	/* + hsw */
	.htotal		= 640 + 10 + 68 + 10,	/* + hbp -> 728 */
	.vdisplay	= 360,
	.vsync_start	= 360 + 5,		/* + vfp */
	.vsync_end	= 360 + 5 + 10,		/* + vsw */
	.vtotal		= 360 + 5 + 10 + 5,	/* + vbp -> 380 */
	.flags		= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

/* LED power limits (board-notle.c:569-588). Defaults; overridable by DT. */
struct notle_led_limits {
	u32 limit_mw;		/* total budget: 76 */
	u32 max_mw[3];		/* per-channel ceiling: R63 G96 B96 */
	u32 percent[3];		/* colour-mix ratio, of 10000: 2444/4444/3111 */
};

struct notle_panel {
	struct drm_panel	panel;
	struct spi_device	*spi;
	struct ice40		ice;
	struct notle_led_limits	led;
	u32			brightness;	/* 0..255 */
	bool			prepared;
};

static inline struct notle_panel *to_notle(struct drm_panel *panel)
{
	return container_of(panel, struct notle_panel, panel);
}

static inline u32 bclamp(u32 b)
{
	return min_t(u32, b, NOTLE_MAX_BRIGHTNESS);
}

/*
 * Compute the per-channel "linecut" (how many of the 380 lines the LED is off
 * for) that keeps each channel within its milliwatt cap, and program it into
 * the FPGA. This is the safety-critical path: the linecut rises (LED on for
 * fewer lines) as the cap bites, and a zero-percent channel is forced fully
 * off. Ported from 3.4 led_config_to_linecuts() for the linecut-based FPGA
 * revisions (<= 0x3e). Newer FPGA revisions use a PWM schedule (3.4
 * led_config_pwm()); that variant is a device-gated follow-up and is noted in
 * docs/mainline-bringup.md. The milliwatt cap is identical in both models, so
 * enabling the backlight here never exceeds the budget.
 */
static int notle_led_apply(struct notle_panel *np, bool enable)
{
	const struct notle_led_limits *l = &np->led;
	const int total_lines = notle_mode.vtotal;		/* 380 */
	const int y_res = notle_mode.vdisplay;			/* 360 */
	u32 b = bclamp(np->brightness);
	int cut[3];
	int i;
	int bl;
	int ret = 0;

	if (np->ice.fpga_rev > ICE40_FINAL_LINECUT_REVISION)
		dev_warn_once(&np->spi->dev,
			      "FPGA rev 0x%02x uses the PWM backlight model; "
			      "the linecut path still caps power but is not "
			      "the native schedule (device iteration)\n",
			      np->ice.fpga_rev);

	for (i = 0; i < 3; i++) {
		/* 3.4: lines * (10000 - 3*pct*bright*limit/(max*255)) / 10000 */
		s64 num = (s64)3 * l->percent[i] * b * l->limit_mw;
		int reduce = l->max_mw[i] ?
			div_s64(num, (s64)l->max_mw[i] * NOTLE_MAX_BRIGHTNESS) : 10000;

		cut[i] = (int)div_s64((s64)total_lines * (10000 - reduce), 10000);

		if (cut[i] > y_res - 3)
			cut[i] = y_res - 3;
		if (!l->percent[i])		/* channel disabled */
			cut[i] = total_lines;
		if (cut[i] < 0)			/* overflow -> full brightness */
			cut[i] = 0;
	}

	dev_dbg(&np->spi->dev, "LED linecuts R/G/B = %d/%d/%d (brightness %u)\n",
		cut[0], cut[1], cut[2], b);

	/* Legacy linecut registers: hi/lo byte per channel. */
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_RR_H, (cut[0] >> 8) & 0xff);
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_RR_H + 1, cut[0] & 0xff);
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_GG_H, (cut[1] >> 8) & 0xff);
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_GG_H + 1, cut[1] & 0xff);
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_BB_H, (cut[2] >> 8) & 0xff);
	ret |= ice40_write_register(&np->ice, ICE40_REG_LED_BB_H + 1, cut[2] & 0xff);

	/* Flip the LED-enable bit last. */
	bl = ice40_read_register(&np->ice, ICE40_REG_BACKLIGHT);
	if (bl < 0)
		return bl;
	if (enable && np->brightness)
		bl |= ICE40_REG_BACKLIGHT_LEDEN;
	else
		bl &= ~ICE40_REG_BACKLIGHT_LEDEN;
	ret |= ice40_write_register(&np->ice, ICE40_REG_BACKLIGHT, bl);

	return ret ? -EIO : 0;
}

static int notle_prepare(struct drm_panel *panel)
{
	struct notle_panel *np = to_notle(panel);
	int rev, ret;

	if (np->prepared)
		return 0;

	/*
	 * If the bootloader already configured the FPGA (CDONE/revision valid)
	 * we reuse it; otherwise we (re)load dss_fpga.img. 3.4 does the same
	 * check in panel_notle_power_on() (:2021-2027).
	 */
	rev = ice40_read_revision(&np->ice);
	if (rev < 0 || rev == ICE40_REV_INVALID) {
		dev_info(&np->spi->dev, "FPGA not configured (rev %d), loading\n",
			 rev);
		ret = ice40_program(&np->ice);
		if (ret)
			return ret;
	} else {
		dev_info(&np->spi->dev, "reusing bootloader FPGA config, rev 0x%02x\n",
			 rev);
		/* Known-good defaults even when reusing. */
		ice40_write_register(&np->ice, ICE40_REG_PIPELINE, 0x00);
		ice40_write_register(&np->ice, ICE40_REG_BACKLIGHT, 0x00);
	}

	/*
	 * TODO (device iteration): write the LCoS per-pixel init sequence
	 * (3.4 panel_init_regs + gamma) over the I2C4 0x49 panel subdevice.
	 * That subdevice is not wired here yet; without it the FPGA scans the
	 * DPI stream but the LCoS registers keep their bootloader state.
	 */

	np->prepared = true;
	return 0;
}

static int notle_unprepare(struct drm_panel *panel)
{
	struct notle_panel *np = to_notle(panel);

	if (!np->prepared)
		return 0;

	/* Leave the FPGA configured; just make sure the LEDs are off. */
	ice40_write_register(&np->ice, ICE40_REG_BACKLIGHT, 0x00);
	np->prepared = false;
	return 0;
}

static int notle_enable(struct drm_panel *panel)
{
	struct notle_panel *np = to_notle(panel);
	int ret;

	/* Enable the LCoS output (DISP_ENB) via the FPGA. */
	ret = ice40_write_register(&np->ice, ICE40_REG_LCOS,
				   ICE40_REG_LCOS_DISP_ENB);
	if (ret)
		return ret;

	/* Enforce the power cap, then light the backlight. */
	return notle_led_apply(np, true);
}

static int notle_disable(struct drm_panel *panel)
{
	struct notle_panel *np = to_notle(panel);

	notle_led_apply(np, false);			/* LEDs off first */
	ice40_write_register(&np->ice, ICE40_REG_LCOS, 0x00);	/* DISP_ENB off */
	return 0;
}

static int notle_get_modes(struct drm_panel *panel,
			   struct drm_connector *connector)
{
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &notle_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	/*
	 * DE-active-high, pixel data clocked on the rising edge. These, like
	 * the sync polarities above, are best-guess DPI defaults and need
	 * on-device confirmation against the bootloader's working setup.
	 */
	connector->display_info.bus_flags =
		DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_DRIVE_POSEDGE;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	return 1;
}

static const struct drm_panel_funcs notle_panel_funcs = {
	.prepare	= notle_prepare,
	.unprepare	= notle_unprepare,
	.enable		= notle_enable,
	.disable	= notle_disable,
	.get_modes	= notle_get_modes,
};

static void notle_read_led_limits(struct device *dev,
				  struct notle_led_limits *l)
{
	/* Defaults are the 3.4 board-notle.c values (:569-588). */
	l->limit_mw = 76;
	l->max_mw[0] = 63; l->max_mw[1] = 96; l->max_mw[2] = 96;
	l->percent[0] = 2444; l->percent[1] = 4444; l->percent[2] = 3111;

	device_property_read_u32(dev, "google,led-limit-milliwatt", &l->limit_mw);
	device_property_read_u32_array(dev, "google,led-max-milliwatt",
				       l->max_mw, 3);
	device_property_read_u32_array(dev, "google,led-mix-permyriad",
				       l->percent, 3);

	/* Clamp to the known-safe ceiling so DT can only tighten, not raise. */
	if (l->limit_mw > 76)
		l->limit_mw = 76;
	if (l->max_mw[0] > 63) l->max_mw[0] = 63;
	if (l->max_mw[1] > 96) l->max_mw[1] = 96;
	if (l->max_mw[2] > 96) l->max_mw[2] = 96;
}

static int notle_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct notle_panel *np;
	u32 board_rev = 4;		/* V1_DVT1; override via DT */
	u32 brightness = 64;		/* conservative default; capped anyway */
	int ret;

	np = devm_drm_panel_alloc(dev, struct notle_panel, panel,
				  &notle_panel_funcs, DRM_MODE_CONNECTOR_DPI);
	if (IS_ERR(np))
		return PTR_ERR(np);

	np->spi = spi;
	spi_set_drvdata(spi, np);

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "spi_setup failed: %d\n", ret);
		return ret;
	}

	np->ice.spi = spi;
	np->ice.fw_name = "dss_fpga.img";
	np->ice.fpga_rev = -1;
	device_property_read_u32(dev, "google,notle-version", &board_rev);
	np->ice.board_rev = board_rev;

	np->ice.creset_b = devm_gpiod_get(dev, "creset", GPIOD_OUT_HIGH);
	if (IS_ERR(np->ice.creset_b))
		return dev_err_probe(dev, PTR_ERR(np->ice.creset_b),
				     "no CRESET_B gpio\n");

	np->ice.cdone = devm_gpiod_get(dev, "cdone", GPIOD_IN);
	if (IS_ERR(np->ice.cdone))
		return dev_err_probe(dev, PTR_ERR(np->ice.cdone),
				     "no CDONE gpio\n");

	notle_read_led_limits(dev, &np->led);
	device_property_read_u32(dev, "google,default-brightness", &brightness);
	np->brightness = bclamp(brightness);

	dev_info(dev,
		 "notle panel: board 0x%02x, LED cap %u mW (max %u/%u/%u), brightness %u\n",
		 np->ice.board_rev, np->led.limit_mw, np->led.max_mw[0],
		 np->led.max_mw[1], np->led.max_mw[2], np->brightness);

	drm_panel_add(&np->panel);
	return 0;
}

static void notle_remove(struct spi_device *spi)
{
	struct notle_panel *np = spi_get_drvdata(spi);

	drm_panel_remove(&np->panel);
}

static const struct of_device_id notle_of_match[] = {
	{ .compatible = "google,notle-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, notle_of_match);

static const struct spi_device_id notle_spi_ids[] = {
	{ "notle-panel", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, notle_spi_ids);

static struct spi_driver notle_spi_driver = {
	.probe		= notle_probe,
	.remove		= notle_remove,
	.id_table	= notle_spi_ids,
	.driver		= {
		.name		= "panel-notle",
		.of_match_table	= notle_of_match,
	},
};
module_spi_driver(notle_spi_driver);

MODULE_DESCRIPTION("Google Glass XE-C notle LCoS/iCE40 DRM panel");
MODULE_LICENSE("GPL");
