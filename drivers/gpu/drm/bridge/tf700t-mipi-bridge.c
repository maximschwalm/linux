// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021, Maxim Schwalm and Svyatoslav Ryhel
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_bridge.h>
#include <drm/drm_panel.h>

#define DISPLAY_MAX_RETRIES 3 /* max counter for retry I2C access */

struct bridge_register_set {
	uint16_t addr;
	uint16_t data;
};

static const struct bridge_register_set display_table[] = {
    /* Software Reset */
	{0x0002, 0x0001}, // SYSctl, S/W Reset
	{0x0000, 0x0005}, // Delay time
	{0x0002, 0x0000}, // SYSctl, S/W Reset release

	/* PLL, Clock Setting */
	{0x0016, 0x309F}, // PLL Control Register 0 (PLL_PRD,PLL_FBD)
	{0x0018, 0x0203}, // PLL_FRS,PLL_LBWS, PLL oscillation enable
	{0x0000, 0x0005}, // Delay time
	{0x0018, 0x0213}, // PLL_FRS,PLL_LBWS, PLL clock out enable

	/* DPI Input Control */
	{0x0006, 0x012C}, // FIFO Control Register

	/* D-PHY Setting */
	{0x0140, 0x0000}, // D-PHY Clock Lane enable
	{0x0142, 0x0000},
	{0x0144, 0x0000}, // D-PHY Data Lane0 enable
	{0x0146, 0x0000},
	{0x0148, 0x0000}, // D-PHY Data Lane1 enable
	{0x014A, 0x0000},
	{0x014C, 0x0000}, // D-PHY Data Lane2 enable
	{0x014E, 0x0000},
	{0x0150, 0x0000}, // D-PHY Data Lane3 enable
	{0x0152, 0x0000},

	{0x0100, 0x0203}, // D-PHY Clock Lane Control TX
	{0x0102, 0x0000},
	{0x0104, 0x0203}, // D-PHY Data Lane0 Control TX
	{0x0106, 0x0000},
	{0x0108, 0x0203}, // D-PHY Data Lane1 Control TX
	{0x010A, 0x0000},
	{0x010C, 0x0203}, // D-PHY Data Lane2 Control TX
	{0x010E, 0x0000},
	{0x0110, 0x0203}, // D-PHY Data Lane3 Control TX
	{0x0112, 0x0000},

	/* DSI-TX PPI Control */
	{0x0210, 0x1964}, // LINEINITCNT
	{0x0212, 0x0000},
	{0x0214, 0x0005}, // LPTXTIMECNT
	{0x0216, 0x0000},
	{0x0218, 0x2801}, // TCLK_HEADERCNT
	{0x021A, 0x0000},
	{0x021C, 0x0000}, // TCLK_TRAILCNT
	{0x021E, 0x0000},
	{0x0220, 0x0C06}, // THS_HEADERCNT
	{0x0222, 0x0000},
	{0x0224, 0x4E20}, // TWAKEUPCNT
	{0x0226, 0x0000},
	{0x0228, 0x000B}, // TCLK_POSTCNT
	{0x022A, 0x0000},
	{0x022C, 0x0005}, // THS_TRAILCNT
	{0x022E, 0x0000},
	{0x0230, 0x0005}, // HSTXVREGCNT
	{0x0232, 0x0000},
	{0x0234, 0x001F}, // HSTXVREGEN enable
	{0x0236, 0x0000},
	{0x0238, 0x0001}, // DSI clock enable/disable during LP
	{0x023A, 0x0000},
	{0x023C, 0x0005}, // BTACNTRL1
	{0x023E, 0x0005},
	{0x0204, 0x0001}, // STARTCNTRL
	{0x0206, 0x0000},

	/* DSI-TX Timing Control */
	{0x0620, 0x0001}, // Sync Pulse/Sync Event mode setting
	{0x0622, 0x0020}, // V Control Register1
	{0x0624, 0x001A}, // V Control Register2
	{0x0626, 0x04B0}, // V Control Register3
	{0x0628, 0x015E}, // H Control Register1
	{0x062A, 0x00FA}, // H Control Register2
	{0x062C, 0x1680}, // H Control Register3

	{0x0518, 0x0001}, // DSI Start
	{0x051A, 0x0000},

	/* Set to HS mode */
	{0x0500, 0x0086}, // DSI lane setting, DSI mode=HS
	{0x0502, 0xA300}, // bit set
	{0x0500, 0x8000}, // Switch to DSI mode
	{0x0502, 0xC300},

	/* Host: RGB(DPI) input start */
	{0x0008, 0x0037}, // DSI-TX Format setting
	{0x0050, 0x003E}, // DSI-TX Pixel Stream packet Data Type setting
	{0x0032, 0x0001}, // HSYNC polarity
	{0x0004, 0x0064}, // Configuration Control Register
};

struct bridge_data {
	struct i2c_client *client;
	struct device *dev;

	struct drm_bridge bridge;
	struct drm_bridge *panel_bridge;

	struct regulator *vdd;
	struct regulator *vddio;

	struct gpio_desc *power_gpio;
	struct gpio_desc *lvds_gpio;
	struct gpio_desc *ldo_gpio;

	const struct drm_display_mode *mode;
	const struct bridge_register_set *init_regs;

	int ninit_regs;
};

static inline struct bridge_data *to_bridge_data(struct drm_bridge *bridge)
{
	return container_of(bridge, struct bridge_data, bridge);
}

static int bridge_write_reg(struct i2c_client *client, u16 addr, u16 data)
{
	int err;
	struct i2c_msg msg[1];
	unsigned char buf[4];
	int retry = 0;

	if (!client->adapter)
		return -ENODEV;

	buf[0] = (addr >> 8);
	buf[1] = (addr & 0xFF);
	buf[2] = (data >> 8);
	buf[3] = (data & 0xFF);

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = ARRAY_SIZE(buf);
	msg->buf = buf;

	do {
		err = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
		if (err < 0) {
			dev_err(&client->dev, "i2c_transfer failed. err = %d, addr = %x, "
				"data = %x\n", err, addr, data);
			return err;
		}
		retry++;
	} while (retry <= DISPLAY_MAX_RETRIES);

	return 0;
}

static int bridge_write_table(struct i2c_client *client,
	const struct bridge_register_set *regs, int nregs)
{
	int err, i;

	for (i = 0; i < nregs; i++) {
		if (!regs[i].addr)
			msleep(regs[i].data);

		err = bridge_write_reg(client, regs[i].addr, regs[i].data);
		if (err)
			break;
	}

	return err;
}

static int mipi_bridge_attach(struct drm_bridge *bridge,
			     enum drm_bridge_attach_flags flags)
{
	struct bridge_data *data = to_bridge_data(bridge);

	return drm_bridge_attach(bridge->encoder, data->panel_bridge,
				 bridge, flags);
}

static void mipi_bridge_enable(struct drm_bridge *bridge)
{
	struct bridge_data *data = to_bridge_data(bridge);
	struct i2c_client *client = data->client;
	struct i2c_msg msg[2];
	unsigned char buf[4] = {0, 0, 0, 0};
	int ret;

	/* Used by Panasonic panel */
	if (data->ldo_gpio)
		gpiod_set_value_cansleep(data->ldo_gpio, 1);

	mdelay(20);

	/* Check power on/off for bridge IC */
	ret = regulator_enable(data->vdd);
	if (ret) {
		dev_err(data->dev,
			"Failed to enable regulator \"vdd\": %d\n", ret);
		return;
	}

	ret = regulator_enable(data->vddio);
	if (ret) {
		dev_err(data->dev,
			"Failed to enable regulator \"vddio\": %d\n", ret);
		return;
	}

	mdelay(10);

	if (data->lvds_gpio)
		gpiod_set_value_cansleep(data->lvds_gpio, 1);

	if (data->power_gpio)
		gpiod_set_value_cansleep(data->power_gpio, 1);

	mdelay(10);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = buf;

	/* high byte goes out first */
	buf[0] = 0;
	buf[1] = 0;

	msg[1].addr = client->addr;
	msg[1].flags = 1;
	msg[1].len = 2;
	msg[1].buf = buf + 2;

	i2c_transfer(client->adapter, msg, 2);
	bridge_write_table(client, data->init_regs, data->ninit_regs);

//	if (gpio_get_value(TEGRA_GPIO_PI6))
//		mdelay(70);
//	else
		mdelay(35);
}

static void mipi_bridge_disable(struct drm_bridge *bridge)
{
	struct bridge_data *data = to_bridge_data(bridge);
	int ret;

	if (data->lvds_gpio)
		gpiod_set_value_cansleep(data->lvds_gpio, 0);

	if (data->power_gpio)
		gpiod_set_value_cansleep(data->power_gpio, 0);

	ret = regulator_disable(data->vddio);
	if (ret)
		dev_err(data->dev,
			"Failed to disable regulator \"vddio\": %d\n", ret);

	ret = regulator_disable(data->vdd);
	if (ret)
		dev_err(data->dev,
			"Failed to disable regulator \"vdd\": %d\n", ret);

//	if (gpio_get_value(TEGRA_GPIO_PI6))
//		msleep(10);
//	else
		msleep(85);

	if (data->ldo_gpio)
		gpiod_set_value_cansleep(data->ldo_gpio, 0);
}

static const struct drm_bridge_funcs mipi_bridge_funcs = {
	.attach = mipi_bridge_attach,
	.enable = mipi_bridge_enable,
	.disable = mipi_bridge_disable,
};

static int bridge_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *panel_node;
	struct drm_panel *panel;
	struct bridge_data *data;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->client = client;

	data->vdd = devm_regulator_get(data->dev, "vdd");
	if (IS_ERR(data->vdd))
		return dev_err_probe(dev, PTR_ERR(data->vdd),
				     "Unable to get \"vdd\" supply\n");

	data->vddio = devm_regulator_get(data->dev, "vddio");
	if (IS_ERR(data->vddio))
		return dev_err_probe(dev, PTR_ERR(data->vddio),
				     "Unable to get \"vddio\" supply\n");

	data->ldo_gpio = devm_gpiod_get_optional(dev, "ldo",
							     GPIOD_OUT_LOW);
	if (IS_ERR(data->ldo_gpio))
		return dev_err_probe(dev, PTR_ERR(data->ldo_gpio),
				     "ldo GPIO failure\n");

	data->power_gpio = devm_gpiod_get_optional(dev, "power",
							     GPIOD_OUT_LOW);
	if (IS_ERR(data->power_gpio))
		return dev_err_probe(dev, PTR_ERR(data->power_gpio),
				     "power GPIO failure\n");

	data->lvds_gpio = devm_gpiod_get_optional(dev, "lvds",
							     GPIOD_OUT_LOW);
	if (IS_ERR(data->lvds_gpio))
		return dev_err_probe(dev, PTR_ERR(data->lvds_gpio),
				     "lvds GPIO failure\n");

	data->init_regs = display_table;
	data->ninit_regs = ARRAY_SIZE(display_table);

	/* Locate the panel DT node. */
	panel_node = of_graph_get_remote_node(dev->of_node, 1, 0);
	if (!panel_node) {
		dev_dbg(dev, "panel DT node not found\n");
		return -ENXIO;
	}

	panel = of_drm_find_panel(panel_node);
	of_node_put(panel_node);
	if (IS_ERR(panel)) {
		dev_dbg(dev, "panel not found, deferring probe\n");
		return PTR_ERR(panel);
	}

	data->panel_bridge =
		devm_drm_panel_bridge_add_typed(dev, panel,
						DRM_MODE_CONNECTOR_LVDS);
	if (IS_ERR(data->panel_bridge))
		return PTR_ERR(data->panel_bridge);

	data->bridge.funcs = &mipi_bridge_funcs;
	data->bridge.of_node = dev->of_node;

	drm_bridge_add(&data->bridge);
	i2c_set_clientdata(client, data);

	dev_info(data->dev, "probed\n");

	return 0;
}

static int bridge_i2c_remove(struct i2c_client *client)
{
	struct bridge_data *data = i2c_get_clientdata(client);

	drm_bridge_remove(&data->bridge);

	return 0;
}

static const struct i2c_device_id bridge_i2c_id[] = {
	{ "mipi-bridge", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, bridge_i2c_id);

static const struct of_device_id bridge_dt_match[] = {
	{ .compatible = "tf700t,mipi-bridge" },
	{ },
};
MODULE_DEVICE_TABLE(of, bridge_dt_match);

struct i2c_driver bridge_i2c_driver = {
	.driver	= {
		.name	= "tf700t-mipi-bridge",
		.of_match_table = bridge_dt_match,
	},
	.id_table	= bridge_i2c_id,
	.probe		= bridge_i2c_probe,
	.remove		= bridge_i2c_remove,
};
module_i2c_driver(bridge_i2c_driver);

MODULE_AUTHOR("Maxim Schwalm <maxim.schwalm@gmail.com>");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Asus TF700T MIPI bridge driver");
MODULE_LICENSE("GPL");
