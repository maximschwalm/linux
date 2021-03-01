// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2006-2010, Samsung Electronics Co., Ltd. All Right Reserved.
 * Copyright (C) 2010-2012, NVIDIA Corporation
 * Copyright (C) 2015, Red Hat
 * Copyright (C) 2015, Sony Mobile Communications Inc.
 * Author: Maxim Schwalm <maxim.schwalm@gmail.com>
 *
 * Based on Samsung CMC6230R panel driver by Robert Yang <decatf@gmail.com>
 * and Panasonic VVX10F034N00 panel driver by 
 * Werner Johansson <werner.johansson@sonymobile.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include "panel-asus-tf700t.h"

/* Maximum count for I2C access retries */
#define DISPLAY_MAX_RETRIES   3 

enum wuxga_gpios {
	TEGRA_GPIO_PI6,
	TEGRA_GPIO_PH3,
	TEGRA_GPIO_PU5,
	TEGRA_GPIO_PBB3,
	TEGRA_GPIO_PC6,
	TEGRA_GPIO_PX0,
	TEGRA_GPIO_PD2,
	NUM_GPIOS,
};

struct wuxga_data {
	struct i2c_client *client;
	struct gpio_desc *gpios[NUM_GPIOS];
	bool suspended;

	const struct wuxga_register_set *init_regs;
	int ninit_regs;

	const struct drm_display_mode *mode;
	struct drm_panel panel;
	int wuxga_type;

	struct regulator *supply;

	bool prepared;
	bool enabled;
};


static int wuxga_write_reg(struct i2c_client *client, u16 addr, u16 data)
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

static int wuxga_write_table(struct i2c_client *client,
	const struct wuxga_register_set *regs, int nregs)
{
	int err, i;

	for (i = 0; i < nregs; i++) {
		err = wuxga_write_reg(client, regs[i].addr, regs[i].data);
		if (err)
			break;

		if (regs[i].addr == DISPLAY_WAIT_MS) {
			msleep(regs[i].data);
			continue;
		}
	}

	return err;
}

static void wuxga_suspend(struct i2c_client *client)
{
	struct wuxga_data *data = i2c_get_clientdata(client);

	if (data->suspended)
		return;

	gpiod_set_value(data->gpios[TEGRA_GPIO_PD2], 0);
	gpiod_set_value(data->gpios[TEGRA_GPIO_PX0] , 0);
	gpiod_set_value(data->gpios[TEGRA_GPIO_PC6], 0);
	gpiod_set_value(data->gpios[TEGRA_GPIO_PBB3], 0);
	
//	if (data->wuxga_type == WUXGA_TYPE_HYDIS)
//		msleep(10);
//	else
		msleep(85);

	data->suspended = true;
}

static void wuxga_resume_gpios(struct i2c_client *client)
{
	struct wuxga_data *data = i2c_get_clientdata(client);

	gpiod_set_value(data->gpios[TEGRA_GPIO_PBB3], 1);
	gpiod_set_value(data->gpios[TEGRA_GPIO_PC6], 1);
	mdelay(10);

	gpiod_set_value(data->gpios[TEGRA_GPIO_PX0], 1);
	mdelay(10);

	gpiod_set_value(data->gpios[TEGRA_GPIO_PD2], 1);
	msleep(10);
}

static void wuxga_resume(struct i2c_client *client)
{
	struct wuxga_data *data = i2c_get_clientdata(client);
	struct i2c_msg msg[2];
	unsigned char buf[4] = {0, 0, 0, 0};

	if (!data->suspended)
		return;

	wuxga_resume_gpios(client);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = buf;

	/* High byte goes out first */
	buf[0] = 0;
	buf[1] = 0;

	msg[1].addr = client->addr;
	msg[1].flags = 1;
	msg[1].len = 2;
	msg[1].buf = buf + 2;
		
	i2c_transfer(client->adapter, msg, 2);
	wuxga_write_table(client, data->init_regs, data->ninit_regs);

//	if (data->wuxga_type == WUXGA_TYPE_HYDIS)
//		mdelay(70);
//	else
		mdelay(35);

	data->suspended = false;
}


static const struct drm_display_mode asus_tf700t_mode = {
	.clock = 154000,
	.hdisplay = 1920,
	.hsync_start = 1920 + 48,
	.hsync_end = 1920 + 48 + 32,
	.htotal = 1920 + 48 + 32 + 80,
	.vdisplay = 1200,
	.vsync_start = 1200 + 3,
	.vsync_end = 1200 + 3 + 6,
	.vtotal = 1200 + 3 + 6 + 26,
};

static inline struct wuxga_data *panel_to_wuxga(struct drm_panel *panel)
{
	return container_of(panel, struct wuxga_data, panel);
}

static int wuxga_drm_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct wuxga_data *data = panel_to_wuxga(panel);
	const struct drm_display_mode *panel_mode = data->mode;
	struct drm_display_mode *mode;
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA;

	mode = drm_mode_duplicate(connector->dev, panel_mode);
	if (!mode) {
		DRM_ERROR("failed to add mode %ux%u\n",
			panel_mode->hdisplay, panel_mode->vdisplay);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 217;
	connector->display_info.height_mm = 136;
	drm_display_info_set_bus_formats(&connector->display_info,
					&bus_format, 1);

	return 1;
}

static int wuxga_drm_disable(struct drm_panel *panel)
{
	struct wuxga_data *data = panel_to_wuxga(panel);
	struct i2c_client *client = data->client;

	if (!data->enabled)
		return 0;

	wuxga_suspend(client);

	data->enabled = false;

	return 0;
}

static int wuxga_drm_unprepare(struct drm_panel *panel)
{
	struct wuxga_data *data = panel_to_wuxga(panel);

    if (!data->prepared)
		return 0;

	if (data->supply)
		regulator_disable(data->supply);

	gpiod_set_value(data->gpios[TEGRA_GPIO_PU5], 0);

    data->prepared = false;

	return 0;
}

static int wuxga_drm_prepare(struct drm_panel *panel)
{
	struct wuxga_data *data = panel_to_wuxga(panel);
	int err;

    if (data->prepared)
		return 0;

//	if (data->wuxga_type == WUXGA_TYPE_HYDIS) {
//		gpiod_set_value(data->gpios[TEGRA_GPIO_PH3], 0);
//		err = gpiod_direction_output(data->gpios[TEGRA_GPIO_PU5], 0);
//		if (err < 0)
//			return err;
//	} else {
		err = gpiod_direction_output(data->gpios[TEGRA_GPIO_PU5], 1);
		if (err < 0)
			return err;
//	}
	
	mdelay(5);

	err = regulator_enable(data->supply);
	if (err < 0)
		return err;
	
	msleep(20);

    data->prepared = true;

	return 0;
}

static int wuxga_drm_enable(struct drm_panel *panel)
{
	struct wuxga_data *data = panel_to_wuxga(panel);
	struct i2c_client *client = data->client;

	if (data->enabled)
		return 0;

	wuxga_resume(client);

	data->enabled = true;

	return 0;
}

static const struct drm_panel_funcs wuxga_panel_funcs = {
	.disable = wuxga_drm_disable,
	.unprepare = wuxga_drm_unprepare,
	.prepare = wuxga_drm_prepare,
	.enable = wuxga_drm_enable,
	.get_modes = wuxga_drm_get_modes,
};


struct wuxga_gpio_init {
	enum wuxga_gpios id;
	const char *name;
	enum gpiod_flags flags;
} static const wuxga_gpio_init_table[] = {
//	{ .id = TEGRA_GPIO_PI6, .name = "panel-type", .flags = GPIOD_IN },
//	{ .id = TEGRA_GPIO_PH3, .name = "en-vdd-bl", .flags = GPIOD_ASIS },
	{ .id = TEGRA_GPIO_PU5, .name = "ldo-en", .flags = GPIOD_IN },
	{ .id = TEGRA_GPIO_PBB3, .name = "mipi-1v2", .flags = GPIOD_OUT_HIGH },
	{ .id = TEGRA_GPIO_PC6, .name = "mipi-1v8", .flags = GPIOD_OUT_HIGH },
	{ .id = TEGRA_GPIO_PX0, .name = "i2c-switch", .flags = GPIOD_OUT_HIGH },
	{ .id = TEGRA_GPIO_PD2, .name = "osc-gate", .flags = GPIOD_OUT_HIGH },
};

static int wuxga_init_gpios(struct i2c_client *client,
	struct wuxga_data *data)
{
	struct gpio_desc *desc;
	int i, err = 0;

	for (i = 0; i < ARRAY_SIZE(wuxga_gpio_init_table); i++) {
		const struct wuxga_gpio_init *item = &wuxga_gpio_init_table[i];

		desc = devm_gpiod_get(&client->dev, item->name, item->flags);
		if (IS_ERR(desc)) {
			err = PTR_ERR(desc);
			dev_err(&client->dev, "could not get %s gpio. err = %d\n",
				item->name, err);
			return err;
		}

		data->gpios[item->id] = desc;
	}

	return err;
}

static int wuxga_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct wuxga_data *data;
	int err;

	data = devm_kzalloc(&client->dev, sizeof(struct wuxga_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		return err;
	}

	data->supply = devm_regulator_get(&client->dev, "power");
	if (IS_ERR(data->supply))
		return PTR_ERR(data->supply);

	err = wuxga_init_gpios(client, data);
	if (err)
		return err;

//	if (gpiod_get_value(data->gpios[TEGRA_GPIO_PI6])) {
//		pr_info("%s: panel type is HYDIS HV101WU1-1E\n", __func__);
//		data->wuxga_type = WUXGA_TYPE_HYDIS;
//	} else {
		pr_info("%s: panel type is Panasonic VVX10F004B00\n", __func__);
		data->wuxga_type = WUXGA_TYPE_PANASONIC;
//	}

	data->init_regs = display_table;
	data->ninit_regs = ARRAY_SIZE(display_table);
	data->mode = &asus_tf700t_mode;

	data->client = client;
	data->suspended = false;
	i2c_set_clientdata(client, data);

	drm_panel_init(&data->panel, &client->dev, &wuxga_panel_funcs,
		       DRM_MODE_CONNECTOR_LVDS);

	err = drm_panel_of_backlight(&data->panel);
	if (err)
		return err;

	drm_panel_add(&data->panel);
	
	return 0;
}

static int wuxga_i2c_remove(struct i2c_client *client)
{
	struct wuxga_data *data = i2c_get_clientdata(client);

	drm_panel_disable(&data->panel);

	drm_panel_remove(&data->panel);

	return 0;
}

static const struct i2c_device_id wuxga_id[] = {
	{ "wuxga", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, wuxga_id);

static const struct of_device_id wuxga_dt_match[] = {
	{ .compatible = "asus,tf700t-panel" },
	{ },
};
MODULE_DEVICE_TABLE(of, wuxga_dt_match);

struct i2c_driver wuxga_i2c_driver = {
	.driver	= {
		.name	= "panel-asus-tf700t",
		.of_match_table = wuxga_dt_match,
	},
	.probe		= wuxga_i2c_probe,
	.remove		= wuxga_i2c_remove,
	.id_table	= wuxga_id,
};
module_i2c_driver(wuxga_i2c_driver);

MODULE_AUTHOR("Maxim Schwalm <maxim.schwalm@gmail.com>");
MODULE_DESCRIPTION("Asus TF700T WUXGA LCD panel driver");
MODULE_LICENSE("GPL v2");
