/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lattice iCE40 slave-serial loader for the Google Glass XE-C ("notle") panel.
 *
 * Ported from Google's 3.4 omapfb driver
 * drivers/video/omap2/displays/panel-notle-dpi.c (ice40_load(),
 * fpga_reconfigure_inner(), ice40_{read,write}_register()), commit 989d925 of
 * OpenMyGlass/kernel-omap (branch glass-omap-xrx13b), GPL-2.0.
 *
 * The iCE40LP4K sits between the OMAP4 DPI output and the 640x360 LCoS panel.
 * It is configured in slave-serial mode over McSPI1 CS0 with the proprietary
 * bitstream dss_fpga.img (never committed; loaded via request_firmware), using
 * two side-band GPIOs: FPGA_CRESET_B (out) and FPGA_CDONE (in). After
 * configuration the *same* SPI link carries 8-bit control registers.
 *
 * This is a self-contained helper: it owns no DT binding of its own and runs
 * no module_init. The panel driver (panel-notle.c) instantiates one struct
 * ice40 and drives it. Keeping it separate mirrors the 3.4 split and lets the
 * loader be reasoned about (and later device-tested) on its own.
 */
#ifndef __PANEL_NOTLE_ICE40_H
#define __PANEL_NOTLE_ICE40_H

#include <linux/types.h>

struct spi_device;
struct gpio_desc;

/*
 * iCE40 control registers, reached over the same SPI link once configured
 * (panel-notle-dpi.c:51-82). A read sends (reg & 0x7f); a write sends
 * (reg | 0x80) followed by the value.
 */
#define ICE40_REG_REVISION		0x00
#define ICE40_REG_PIPELINE		0x01
#define ICE40_REG_LCOS			0x03
#define ICE40_REG_LCOS_DISP_ENB		0x01
#define ICE40_REG_BACKLIGHT		0x10
#define ICE40_REG_BACKLIGHT_LEDEN	0x10

/* Legacy (linecut) LED registers, FPGA revision <= 0x3e. */
#define ICE40_REG_LED_RR_H		0x11
#define ICE40_REG_LED_GG_H		0x13
#define ICE40_REG_LED_BB_H		0x15

/* 0xff from the revision register means the FPGA is not configured. */
#define ICE40_REV_INVALID		0xff

/* The final FPGA revision that uses the linecut backlight model. */
#define ICE40_FINAL_LINECUT_REVISION	0x3e

/**
 * struct ice40 - one iCE40 instance on a McSPI link
 * @spi:	the SPI device (McSPI1 CS0, up to 48 MHz, mode 3)
 * @creset_b:	FPGA_CRESET_B, output (GPIO 87 on XE-C)
 * @cdone:	FPGA_CDONE, input (GPIO 85 on XE-C)
 * @fw_name:	request_firmware name of the container ("dss_fpga.img")
 * @board_rev:	the notle board revision used to pick a bitstream entry
 * @fpga_rev:	last FPGA revision read back, or -1 if unknown
 */
struct ice40 {
	struct spi_device	*spi;
	struct gpio_desc	*creset_b;
	struct gpio_desc	*cdone;
	const char		*fw_name;
	u8			board_rev;
	int			fpga_rev;
};

/*
 * Request the firmware container, pick the entry matching board_rev, and clock
 * it into the FPGA in slave-serial mode (deconfigure via CRESET_B, send the
 * bitstream, wait for CDONE). On success reads back and stores fpga_rev and
 * writes the pipeline/backlight defaults. Returns 0 or a negative errno.
 */
int ice40_program(struct ice40 *ice);

/* 8-bit control register access over SPI (post-configuration). */
int ice40_read_register(struct ice40 *ice, u8 reg);
int ice40_write_register(struct ice40 *ice, u8 reg, u8 val);

/* Read the FPGA revision register; also refreshes ice->fpga_rev. */
int ice40_read_revision(struct ice40 *ice);

#endif /* __PANEL_NOTLE_ICE40_H */
