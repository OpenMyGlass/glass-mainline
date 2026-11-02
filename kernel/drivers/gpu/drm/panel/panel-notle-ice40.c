// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lattice iCE40 slave-serial loader for the Google Glass XE-C ("notle") panel.
 *
 * Ported from Google's 3.4 omapfb driver panel-notle-dpi.c (commit 989d925 of
 * OpenMyGlass/kernel-omap, branch glass-omap-xrx13b), GPL-2.0. The functions
 * here mirror, in order:
 *   ice40_read_register()/ice40_write_register()  (3.4: :1690-1709)
 *   ice40_load()                                   (3.4: :1814-1887)
 *   fpga_reconfigure_inner()                       (3.4: :1889-1956)
 *
 * The proprietary bitstream dss_fpga.img (302468 bytes on XE-C) is NOT in the
 * tree; it is loaded by request_firmware() from /lib/firmware at runtime. The
 * container format (header + per-board-revision entries + CRC32) is Google's
 * u-boot "fpga" format and is reproduced from the 3.4 driver's own copy.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include "panel-notle-ice40.h"

#define ICE40_SPI_READ_MASK	0x7f	/* reg & 0x7f on a read */
#define ICE40_SPI_WRITE_FLAG	0x80	/* reg | 0x80 on a write */

/* DMA-safe bounce buffer size for clocking the bitstream (3.4 used 32 KiB). */
#define ICE40_LOAD_CHUNK	32768

/*
 * Container format, copied verbatim (in layout) from the 3.4 driver, which in
 * turn copied it from Glass's u-boot. "fpga" (lower case) is the unpadded
 * variant used by dss_fpga.img.
 */
struct fpga_header {
	u8  magic[4];
	u32 entry_count;
	u32 crc32;		/* over the header (with this field zeroed) + entries */
} __packed;

struct fpga_entry {
	u32 revision;
	u8  supported_board_revs[8];
	u32 raw_length;
	u8  raw_image[4];	/* placeholder; real size is raw_length */
} __packed;

static const struct fpga_entry *ice40_next_entry(const struct fpga_entry *e)
{
	return (const struct fpga_entry *)
		((const u8 *)e + offsetof(struct fpga_entry, raw_image) +
		 le32_to_cpu(e->raw_length));
}

int ice40_read_register(struct ice40 *ice, u8 reg)
{
	int ret;

	if (!ice->spi)
		return -ENODEV;

	ret = spi_w8r8(ice->spi, reg & ICE40_SPI_READ_MASK);
	if (ret < 0)
		dev_err(&ice->spi->dev, "iCE40 read reg 0x%02x failed: %d\n",
			reg, ret);
	return ret;
}

int ice40_write_register(struct ice40 *ice, u8 reg, u8 val)
{
	u8 buf[2] = { reg | ICE40_SPI_WRITE_FLAG, val };
	int ret;

	if (!ice->spi)
		return -ENODEV;

	ret = spi_write(ice->spi, buf, sizeof(buf));
	if (ret < 0)
		dev_err(&ice->spi->dev, "iCE40 write reg 0x%02x failed: %d\n",
			reg, ret);
	return ret;
}

int ice40_read_revision(struct ice40 *ice)
{
	int rev = ice40_read_register(ice, ICE40_REG_REVISION);

	if (rev >= 0)
		ice->fpga_rev = rev;
	return rev;
}

/*
 * Clock one raw bitstream into the FPGA in slave-serial mode. Mirrors the 3.4
 * ice40_load(): flip CS to active-high so it is asserted while CRESET_B rises,
 * pulse CRESET_B, confirm CDONE went low, send a blank preamble byte, the body
 * in DMA-safe chunks, 13 trailing clocks, then wait for CDONE to rise.
 */
static int ice40_clock_bitstream(struct ice40 *ice, const u8 *bits, u32 size)
{
	const u8 zero = 0;
	unsigned long deadline;
	u8 *bounce;
	u32 off;
	int ret;
	int i;

	bounce = kmalloc(ICE40_LOAD_CHUNK, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	dev_dbg(&ice->spi->dev, "iCE40 CDONE before deconfig: %d\n",
		gpiod_get_value_cansleep(ice->cdone));

	/*
	 * Active-high CS so it is de-asserted (low) exactly when CRESET_B
	 * rises, as the iCE40 slave-serial sequence requires. 3.4 does the
	 * same with SPI_CS_HIGH + spi_setup(). NOTE (device iteration): on
	 * omap2-mcspi native chip-select, the polarity flip may not behave as
	 * it did on the 3.4 McSPI driver; a dedicated cs-gpio on McSPI1 CS0
	 * is the fallback if CDONE never drops here.
	 */
	ice->spi->mode |= SPI_CS_HIGH;
	ret = spi_setup(ice->spi);
	if (ret)
		goto out;

	gpiod_set_value_cansleep(ice->creset_b, 0);	/* assert reset (active low) */
	mdelay(1);
	gpiod_set_value_cansleep(ice->creset_b, 1);	/* release reset */
	mdelay(1);

	if (gpiod_get_value_cansleep(ice->cdone) == 1) {
		dev_err(&ice->spi->dev, "CDONE high after reset; FPGA did not clear\n");
		ret = -EIO;
		goto restore;
	}

	/* Blank preamble byte. */
	ret = spi_write(ice->spi, &zero, 1);
	if (ret)
		goto restore;

	/* Body, copied through a DMA-safe buffer (firmware data may not be). */
	for (off = 0; off < size; off += ICE40_LOAD_CHUNK) {
		u32 n = min_t(u32, ICE40_LOAD_CHUNK, size - off);

		memcpy(bounce, bits + off, n);
		ret = spi_write(ice->spi, bounce, n);
		if (ret)
			goto restore;
	}

	/* 13 trailing clocks to let the FPGA finish. */
	for (i = 0; i < 13; i++) {
		ret = spi_write(ice->spi, &zero, 1);
		if (ret)
			goto restore;
	}

	/* Wait for CDONE to rise (configuration complete). */
	ret = -ETIMEDOUT;
	deadline = jiffies + msecs_to_jiffies(100);
	do {
		if (gpiod_get_value_cansleep(ice->cdone) == 1) {
			ret = 0;
			break;
		}
		cpu_relax();
	} while (time_before(jiffies, deadline));

	if (ret)
		dev_err(&ice->spi->dev, "timeout waiting for CDONE\n");

restore:
	ice->spi->mode &= ~SPI_CS_HIGH;
	if (spi_setup(ice->spi) && !ret)
		ret = -EIO;
out:
	kfree(bounce);
	return ret;
}

static int ice40_verify_crc(struct device *dev, const struct fpga_header *hdr,
			    const void *image, size_t image_size)
{
	const u32 zero_posn = offsetof(struct fpga_header, crc32);
	const struct fpga_entry *end = (const void *)(hdr + 1);
	const u32 zero = 0;
	u32 expected, actual;
	const void *lim = (const u8 *)image + image_size;
	u32 i, n = le32_to_cpu(hdr->entry_count);

	for (i = 0; i < n && (const void *)end < lim; i++)
		end = ice40_next_entry(end);

	if ((const void *)end > lim) {
		dev_err(dev, "FPGA image truncated\n");
		return -EINVAL;
	}

	/*
	 * Glass's CRC32 brackets the standard crc32() with ~ on entry and exit,
	 * and treats the embedded crc32 field as zero. Reproduced from 3.4.
	 */
	actual = crc32(~0, image, zero_posn);
	actual = crc32(actual, (const u8 *)&zero, sizeof(zero));
	actual = ~crc32(actual,
			(const u8 *)image + zero_posn + sizeof(zero),
			(const u8 *)end -
			((const u8 *)image + zero_posn + sizeof(zero)));

	expected = le32_to_cpu(hdr->crc32);
	if (expected != actual) {
		dev_err(dev, "FPGA image CRC mismatch: want 0x%08x got 0x%08x\n",
			expected, actual);
		return -EINVAL;
	}
	return 0;
}

int ice40_program(struct ice40 *ice)
{
	struct device *dev = &ice->spi->dev;
	const struct fpga_header *hdr;
	const struct fpga_entry *entry;
	const struct firmware *fw;
	u32 i, n;
	int ret;

	ret = request_firmware(&fw, ice->fw_name, dev);
	if (ret) {
		dev_err(dev, "request_firmware(%s) failed: %d\n",
			ice->fw_name, ret);
		return ret;
	}

	if (fw->size < sizeof(*hdr)) {
		ret = -EINVAL;
		goto out;
	}

	hdr = (const void *)fw->data;
	if (memcmp(hdr->magic, "fpga", 4)) {
		dev_err(dev, "bad FPGA container magic\n");
		ret = -EINVAL;
		goto out;
	}

	ret = ice40_verify_crc(dev, hdr, fw->data, fw->size);
	if (ret)
		goto out;

	/* Find the first entry that lists our board revision. */
	n = le32_to_cpu(hdr->entry_count);
	entry = (const void *)(hdr + 1);
	ret = -ENOENT;
	for (i = 0; i < n; i++, entry = ice40_next_entry(entry)) {
		int j;

		for (j = 0; j < sizeof(entry->supported_board_revs) &&
			    entry->supported_board_revs[j]; j++) {
			if (entry->supported_board_revs[j] != ice->board_rev)
				continue;

			dev_info(dev,
				 "loading iCE40 rev 0x%02x for board 0x%02x (%u bytes)\n",
				 le32_to_cpu(entry->revision), ice->board_rev,
				 le32_to_cpu(entry->raw_length));
			ret = ice40_clock_bitstream(ice, entry->raw_image,
						    le32_to_cpu(entry->raw_length));
			break;
		}
		if (!ret)
			break;
	}

	if (ret) {
		dev_err(dev, "no usable FPGA bitstream for board 0x%02x\n",
			ice->board_rev);
		goto out;
	}

	ret = ice40_read_revision(ice);
	if (ret < 0)
		goto out;
	if (ret == ICE40_REV_INVALID) {
		dev_err(dev, "FPGA reports invalid revision 0xff after load\n");
		ret = -EIO;
		goto out;
	}
	dev_info(dev, "iCE40 configured, revision 0x%02x\n", ret);

	/* Safe defaults: pipeline passthrough off, backlight (LEDs) off. */
	ice40_write_register(ice, ICE40_REG_PIPELINE, 0x00);
	ice40_write_register(ice, ICE40_REG_BACKLIGHT, 0x00);
	ret = 0;

out:
	release_firmware(fw);
	return ret;
}
