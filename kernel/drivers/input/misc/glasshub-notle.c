// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime handshake driver for the Google Glass XE-C ("notle") glasshub MCU.
 *
 * The glasshub is a small MCU on I2C4 0x35 that owns on-head (don/doff)
 * detection, wink, the buttons and the display backlight enable. This driver
 * brings it up at runtime and exposes don/doff + wink as input events.
 *
 * Ported (handshake only) from Google's 3.4 drivers/input/misc/glasshub.c
 * (commit 989d925 of OpenMyGlass/kernel-omap, branch glass-omap-xrx13b),
 * GPL-2.0:
 *   _check_part_id()       (3.4: :462-482)
 *   get_app_version_l()    (3.4: :1450-1469)
 *   boot_device_l()        (3.4: :484-...)
 *
 * DELIBERATELY OMITTED: the firmware-reflash path (CMD_FLASH, the SREC parser,
 * write_glasshub_memory(), and especially any bootloader rewrite). The 3.4
 * driver warns that "a failure to write the boot code is unrecoverable"; this
 * driver can neither flash the application nor the bootloader, by design. Do
 * not add it. See docs/mainline-bringup.md.
 *
 * STATUS (no device yet): the handshake (part-id, version, boot) and the input
 * reporting compile and follow the 3.4 register protocol, but are unverified
 * on hardware. The interrupt decode is a best-effort stub.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

/* 8-bit registers (3.4 glasshub.c:64-87). */
#define GH_REG_STATUS			0
#define GH_REG_PART_ID			1
#define GH_REG_VERSION			2
#define GH_REG_ENABLE_INT		3
#define GH_REG_ENABLE_DON_DOFF		5
#define GH_REG_DON_DOFF			6
#define GH_REG_ENABLE_WINK		7
#define GH_REG_WINK_STATUS		16

/* Command "registers" (write-only opcodes, 3.4 glasshub.c:106-111). */
#define GH_CMD_APP_VERSION		0xf6
#define GH_CMD_BOOTLOADER_VERSION	0xf7
#define GH_CMD_BOOT			0xfa

#define GH_PART_ID			0xbb
#define GH_BOOT_RETRIES			5

/* Input has no dedicated "wink" code; map it to a programmable key. */
#define GH_KEY_WINK			KEY_PROG1

struct glasshub {
	struct i2c_client	*client;
	struct input_dev	*input;
	struct mutex		lock;
	u8			app_major;
	u8			app_minor;
	bool			booted;
};

/* Single-byte register read (3.4 _i2c_read_reg8 for reg < 0x80). */
static int gh_read8(struct glasshub *gh, u8 reg, u8 *val)
{
	int ret = i2c_smbus_read_byte_data(gh->client, reg);

	if (ret < 0)
		return ret;
	*val = ret;
	return 0;
}

/* Issue a command opcode (a bare one-byte write, 3.4 _i2c_write_mult). */
static int gh_cmd(struct glasshub *gh, u8 cmd)
{
	u8 buf = cmd;
	int ret = i2c_master_send(gh->client, &buf, 1);

	return ret < 0 ? ret : 0;
}

/* Read an N-byte response to the last command. */
static int gh_cmd_read(struct glasshub *gh, u8 cmd, u8 *buf, int len)
{
	int ret = gh_cmd(gh, cmd);

	if (ret)
		return ret;
	ret = i2c_master_recv(gh->client, buf, len);
	return ret < 0 ? ret : 0;
}

static int gh_check_part_id(struct glasshub *gh)
{
	u8 id;
	int ret = gh_read8(gh, GH_REG_PART_ID, &id);

	if (ret) {
		dev_err(&gh->client->dev, "cannot read part id: %d\n", ret);
		return ret;
	}
	if (id != GH_PART_ID) {
		dev_err(&gh->client->dev, "unexpected part id 0x%02x\n", id);
		return -ENODEV;
	}
	return 0;
}

static int gh_read_app_version(struct glasshub *gh)
{
	u8 buf[2];
	int ret = gh_cmd_read(gh, GH_CMD_APP_VERSION, buf, sizeof(buf));

	if (ret) {
		dev_err(&gh->client->dev, "cannot read app version: %d\n", ret);
		return ret;
	}
	gh->app_major = buf[0];
	gh->app_minor = buf[1];
	dev_info(&gh->client->dev, "glasshub firmware %u.%u\n",
		 buf[0], buf[1]);
	return 0;
}

/* Tell the MCU to start its application (3.4 boot_device_l). */
static int gh_boot(struct glasshub *gh)
{
	int ret = -EIO;
	int i;

	if (gh->booted)
		return 0;

	for (i = 0; i < GH_BOOT_RETRIES; i++) {
		ret = gh_cmd(gh, GH_CMD_BOOT);
		msleep(100);		/* app code needs time to come up */
		if (!ret)
			break;
	}
	if (ret) {
		dev_err(&gh->client->dev, "boot failed: %d\n", ret);
		return ret;
	}
	gh->booted = true;
	return 0;
}

static irqreturn_t gh_irq_thread(int irq, void *data)
{
	struct glasshub *gh = data;
	u8 don_doff = 0, wink = 0;

	mutex_lock(&gh->lock);
	/*
	 * Best-effort decode (device iteration): read the don/doff and wink
	 * status and report them. The exact clear-on-read semantics and the
	 * status register's bit layout need confirmation on hardware.
	 */
	gh_read8(gh, GH_REG_DON_DOFF, &don_doff);
	gh_read8(gh, GH_REG_WINK_STATUS, &wink);
	mutex_unlock(&gh->lock);

	input_report_switch(gh->input, SW_LID, !don_doff);	/* on-head */
	if (wink)
		input_report_key(gh->input, GH_KEY_WINK, 1);
	input_sync(gh->input);
	if (wink) {
		input_report_key(gh->input, GH_KEY_WINK, 0);
		input_sync(gh->input);
	}
	return IRQ_HANDLED;
}

static int gh_setup_input(struct glasshub *gh)
{
	struct input_dev *in;
	int ret;

	in = devm_input_allocate_device(&gh->client->dev);
	if (!in)
		return -ENOMEM;

	in->name = "glasshub";
	in->phys = "glasshub/input0";
	in->id.bustype = BUS_I2C;

	input_set_capability(in, EV_SW, SW_LID);	/* on-head / off-head */
	input_set_capability(in, EV_KEY, GH_KEY_WINK);

	ret = input_register_device(in);
	if (ret)
		return ret;
	gh->input = in;
	return 0;
}

static int gh_probe(struct i2c_client *client)
{
	struct glasshub *gh;
	int ret;

	gh = devm_kzalloc(&client->dev, sizeof(*gh), GFP_KERNEL);
	if (!gh)
		return -ENOMEM;

	gh->client = client;
	mutex_init(&gh->lock);
	i2c_set_clientdata(client, gh);

	ret = gh_check_part_id(gh);
	if (ret)
		return ret;

	ret = gh_read_app_version(gh);
	if (ret)
		return ret;

	ret = gh_boot(gh);
	if (ret)
		return ret;

	ret = gh_setup_input(gh);
	if (ret)
		return ret;

	if (client->irq) {
		/* Enable don/doff and wink reporting, then take the IRQ. */
		i2c_smbus_write_byte_data(client, GH_REG_ENABLE_DON_DOFF, 1);
		i2c_smbus_write_byte_data(client, GH_REG_ENABLE_WINK, 1);
		i2c_smbus_write_byte_data(client, GH_REG_ENABLE_INT, 1);

		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, gh_irq_thread,
						IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
						"glasshub", gh);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "cannot request irq\n");
	} else {
		dev_info(&client->dev,
			 "no irq: handshake only, no don/doff/wink events\n");
	}

	dev_info(&client->dev, "glasshub ready\n");
	return 0;
}

static const struct of_device_id gh_of_match[] = {
	{ .compatible = "google,glasshub" },
	{ }
};
MODULE_DEVICE_TABLE(of, gh_of_match);

static const struct i2c_device_id gh_i2c_ids[] = {
	{ "glasshub", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gh_i2c_ids);

static struct i2c_driver gh_driver = {
	.probe		= gh_probe,
	.id_table	= gh_i2c_ids,
	.driver		= {
		.name		= "glasshub-notle",
		.of_match_table	= gh_of_match,
	},
};
module_i2c_driver(gh_driver);

MODULE_DESCRIPTION("Google Glass XE-C glasshub MCU handshake driver");
MODULE_LICENSE("GPL");
