// SPDX-License-Identifier: GPL-2.0
/*
 * Omnivision OV2710 CMOS Image Sensor driver
 *
 * Copyright (C) 2020 David Heidelberg
 *
 * Based on OV2680 Sensor Driver
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 * Copyright (C) 2018 Linaro Ltd
 */

#include <asm/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define OV2710_REG_STREAM_CTRL		0x3008
#define OV2710_REG_STREAM_CTRL_RESET	BIT(7)
#define OV2710_REG_STREAM_CTRL_SLEEP	BIT(6)

#define OV2710_REG_R_MANUAL		0x3503 // check
#define OV2710_REG_GAIN_PK		0x350a // could be, check
#define OV2680_REG_EXPOSURE_PK_HIGH	0x3500
#define OV2680_REG_TIMING_HTS		0x380c
#define OV2680_REG_TIMING_VTS		0x380e
#define OV2680_REG_FORMAT1		0x3820
#define OV2680_REG_FORMAT2		0x3821

#define OV2680_REG_ISP_CTRL00		0x5080

#define OV2710_FRAME_RATE		30

#define OV2680_REG_VALUE_8BIT		1
#define OV2680_REG_VALUE_16BIT		2
#define OV2680_REG_VALUE_24BIT		3

#define OV2710_WIDTH_MAX		1920
#define OV2710_HEIGHT_MAX		1080

enum ov2710_mode_id {
	OV2710_MODE_720P_1280_720,
	OV2710_MODE_HD_1920_1080,
	OV2710_MODE_MAX,
};

struct reg_value {
	u16 reg_addr;
	u8 val;
};

static const char * const ov2710_supply_name[] = {
	"DOVDD",
	"DVDD",
	"AVDD",
};

#define OV2710_NUM_SUPPLIES ARRAY_SIZE(ov2710_supply_name)

struct ov2710_mode_info {
	const char *name;
	enum ov2710_mode_id id;
	u32 width;
	u32 height;
	const struct reg_value *reg_data;
	u32 reg_data_size;
};

struct ov2710_ctrls {
	struct v4l2_ctrl_handler handler;
	struct {
		struct v4l2_ctrl *auto_exp;
		struct v4l2_ctrl *exposure;
	};
	struct {
		struct v4l2_ctrl *auto_gain;
		struct v4l2_ctrl *gain;
	};

	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *test_pattern;
};

struct ov2710_dev {
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		sd;

	struct media_pad		pad;
	struct clk			*xvclk;
	u32				xvclk_freq;
	struct regulator_bulk_data	supplies[OV2710_NUM_SUPPLIES];

	struct gpio_desc		*reset_gpio;
	struct mutex			lock; /* protect members */

	bool				mode_pending_changes;
	bool				is_enabled;
	bool				is_streaming;

	struct ov2710_ctrls		ctrls;
	struct v4l2_mbus_framefmt	fmt;
	struct v4l2_fract		frame_interval;

	const struct ov2710_mode_info	*current_mode;
};

static const char * const test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Random Data",
	"Square",
	"Black Image",
};

static const int ov2710_hv_flip_bayer_order[] = { // verify
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

static const struct reg_value ov2710_setting_60fps_720P_1280_720[] = { // TABLE_WAIT_MS ?? FIXME
	{0x3103, 0x93},	{0x3008, 0x82},	{0x3008, 0x42},	{0x3017, 0x7f},
	{0x3018, 0xfc},	{0x3706, 0x61},	{0x3712, 0x0c},	{0x3630, 0x6d},
	{0x3801, 0xb4},	{0x3621, 0x04},	{0x3604, 0x60},	{0x3603, 0xa7},
	{0x3631, 0x26},	{0x3600, 0x04},	{0x3620, 0x37},	{0x3623, 0x00},
	{0x3702, 0x9e},	{0x3703, 0x5c},	{0x3704, 0x40},	{0x370d, 0x0f},
	{0x3713, 0x9f},	{0x3714, 0x4c},	{0x3710, 0x9e},	{0x3801, 0xc4},
	{0x3605, 0x05},	{0x3606, 0x3f},	{0x302d, 0x90},	{0x370b, 0x40},
	{0x3716, 0x31},	{0x3707, 0x52},	{0x380d, 0x74},	{0x5181, 0x20},
	{0x518f, 0x00},	{0x4301, 0xff},	{0x4303, 0x00},	{0x3a00, 0x78},
	{0x300f, 0x88},	{0x3011, 0x28},	{0x3a1a, 0x06},	{0x3a18, 0x00},
	{0x3a19, 0x7a},	{0x3a13, 0x54},	{0x382e, 0x0f},	{0x381a, 0x1a},
	{0x401d, 0x02},	{0x381c, 0x10},	{0x381d, 0xb0},	{0x381e, 0x02},
	{0x381f, 0xec},	{0x3800, 0x01},	{0x3820, 0x0a},	{0x3821, 0x2a},
	{0x3804, 0x05},	{0x3805, 0x10},	{0x3802, 0x00},	{0x3803, 0x04},
	{0x3806, 0x02},	{0x3807, 0xe0},	{0x3808, 0x05},	{0x3809, 0x10},
	{0x380a, 0x02},	{0x380b, 0xe0},	{0x380e, 0x02},	{0x380f, 0xf0},
	{0x380c, 0x07},	{0x380d, 0x00},	{0x3810, 0x10},	{0x3811, 0x06},
	{0x5688, 0x03},	{0x5684, 0x05},	{0x5685, 0x00},	{0x5686, 0x02},
	{0x5687, 0xd0},	{0x3a08, 0x1b},	{0x3a09, 0xe6},	{0x3a0a, 0x17},
	{0x3a0b, 0x40},	{0x3a0e, 0x01},	{0x3a0d, 0x02},	{0x3011, 0x0a},
	{0x300f, 0x8a},	{0x3017, 0x00},	{0x3018, 0x00},	{0x4800, 0x24},
	{0x300e, 0x04},	{0x4801, 0x0f},	{0x300f, 0xc3},	{0x3a0f, 0x40},
	{0x3a10, 0x38},	{0x3a1b, 0x48},	{0x3a1e, 0x30},	{0x3a11, 0x90},
	{0x3a1f, 0x10},	{0x3010, 0x10},	{0x3a0e, 0x02},	{0x3a0d, 0x03},
	{0x3a08, 0x0d},	{0x3a09, 0xf3},	{0x3a0a, 0x0b},	{0x3a0b, 0xa0},
	{0x300f, 0xc3},	{0x3011, 0x0e},	{0x3012, 0x02},	{0x380c, 0x07},
	{0x380d, 0x6a},	{0x3703, 0x5c},	{0x3704, 0x40},	{0x3801, 0xbc},
	{0x3503, 0x17},	{0x3500, 0x00},	{0x3501, 0x00},	{0x3502, 0x00},
	{0x350a, 0x00},	{0x350b, 0x00},	{0x5001, 0x4e},	{0x5000, 0x5f},
	{0x3008, 0x02},
};

static const struct reg_value ov2710_setting_30fps_HD_1920_1080[] = { // TABLE_WAIT_MS ?? FIXME
	{0x3103, 0x93},	{0x3008, 0x82},	{0x3008, 0x42},	{0x3017, 0x7f},
	{0x3018, 0xfc},	{0x3706, 0x61},	{0x3712, 0x0c},	{0x3630, 0x6d},
	{0x3801, 0xb4},	{0x3621, 0x04},	{0x3604, 0x60},	{0x3603, 0xa7},
	{0x3631, 0x26},	{0x3600, 0x04},	{0x3620, 0x37},	{0x3623, 0x00},
	{0x3702, 0x9e},	{0x3703, 0x5c},	{0x3704, 0x40},	{0x370d, 0x0f},
	{0x3713, 0x9f},	{0x3714, 0x4c},	{0x3710, 0x9e},	{0x3801, 0xc4},
	{0x3605, 0x05},	{0x3606, 0x3f},	{0x302d, 0x90},	{0x370b, 0x40},
	{0x3716, 0x31},	{0x3707, 0x52},	{0x380d, 0x74},	{0x5181, 0x20},
	{0x518f, 0x00},	{0x4301, 0xff},	{0x4303, 0x00},	{0x3a00, 0x78},
	{0x300f, 0x88},	{0x3011, 0x28},	{0x3a1a, 0x06},	{0x3a18, 0x00},
	{0x3a19, 0x7a},	{0x3a13, 0x54},	{0x382e, 0x0f},	{0x381a, 0x1a},
	{0x401d, 0x02},	{0x381c, 0x00},	{0x381d, 0x02},	{0x381e, 0x04},
	{0x381f, 0x38},	{0x3820, 0x00},	{0x3821, 0x98},	{0x3800, 0x01},
	{0x3802, 0x00},	{0x3803, 0x0a},	{0x3804, 0x07},	{0x3805, 0x90},
	{0x3806, 0x04},	{0x3807, 0x40},	{0x3808, 0x07},	{0x3809, 0x90},
	{0x380a, 0x04},	{0x380b, 0x40},	{0x380e, 0x04},	{0x380f, 0x50},
	{0x380c, 0x09},	{0x380d, 0x74},	{0x3810, 0x08},	{0x3811, 0x02},
	{0x5688, 0x03},	{0x5684, 0x07},	{0x5685, 0xa0},	{0x5686, 0x04},
	{0x5687, 0x43},	{0x3011, 0x0a},	{0x300f, 0x8a},	{0x3017, 0x00},
	{0x3018, 0x00},	{0x4800, 0x24},	{0x300e, 0x04},	{0x4801, 0x0f},
	{0x300f, 0xc3},	{0x3010, 0x00},	{0x3011, 0x0a},	{0x3012, 0x01},
	{0x3a0f, 0x40},	{0x3a10, 0x38},	{0x3a1b, 0x48},	{0x3a1e, 0x30},
	{0x3a11, 0x90},	{0x3a1f, 0x10},	{0x3a0e, 0x03},	{0x3a0d, 0x04},
	{0x3a08, 0x14},	{0x3a09, 0xc0},	{0x3a0a, 0x11},	{0x3a0b, 0x40},
	{0x300f, 0xc3},	{0x3010, 0x00},	{0x3011, 0x0e},	{0x3012, 0x02},
	{0x380c, 0x09},	{0x380d, 0xec},	{0x3703, 0x61},	{0x3704, 0x44},
	{0x3801, 0xd2},	{0x3503, 0x17},	{0x3500, 0x00},	{0x3501, 0x00},
	{0x3502, 0x00},	{0x350a, 0x00},	{0x350b, 0x00},	{0x5001, 0x4e},
	{0x5000, 0x5f},	{0x3008, 0x02},
};

static const struct ov2710_mode_info ov2710_mode_init_data = {
	"mode_hd_1920_1080", OV2710_MODE_HD_1920_1080, 1920, 1080,
	ov2710_setting_30fps_HD_1920_1080,
	ARRAY_SIZE(ov2710_setting_30fps_HD_1920_1080),
};

static const struct ov2710_mode_info ov2710_mode_data[OV2710_MODE_MAX] = {
	{"mode_720p_1280_720", OV2710_MODE_720P_1280_720,
	 1280, 720, ov2710_setting_60fps_720P_1280_720,
	 ARRAY_SIZE(ov2710_setting_60fps_720P_1280_720)},
	{"mode_hd_1920_1080", OV2710_MODE_HD_1920_1080,
	 1920, 1080, ov2710_setting_30fps_HD_1920_1090,
	 ARRAY_SIZE(ov2710_setting_30fps_HD_1920_1090)},
};

static struct ov2710_dev *to_ov2710_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov2710_dev, sd);
}

static struct device *ov2710_to_dev(struct ov2710_dev *sensor)
{
	return &sensor->i2c_client->dev;
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct ov2710_dev,
			     ctrls.handler)->sd;
}

static int __ov2710_write_reg(struct ov2710_dev *sensor, u16 reg,
			      unsigned int len, u32 val)
{
	struct i2c_client *client = sensor->i2c_client;
	u8 buf[6];
	int ret;

	if (len > 4)
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);
	ret = i2c_master_send(client, buf, len + 2);
	if (ret != len + 2) {
		dev_err(&client->dev, "write error: reg=0x%4x: %d\n", reg, ret);
		return -EIO;
	}

	return 0;
}

#define ov2710_write_reg(s, r, v) \
	__ov2710_write_reg(s, r, OV2710_REG_VALUE_8BIT, v)

#define ov2710_write_reg16(s, r, v) \
	__ov2710_write_reg(s, r, OV2710_REG_VALUE_16BIT, v)

#define ov2710_write_reg24(s, r, v) \
	__ov2710_write_reg(s, r, OV2710_REG_VALUE_24BIT, v)

static int __ov2710_read_reg(struct ov2710_dev *sensor, u16 reg,
			     unsigned int len, u32 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msgs[2];
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0, };
	int ret;

	if (len > 4)
		return -EINVAL;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(&client->dev, "read error: reg=0x%4x: %d\n", reg, ret);
		return -EIO;
	}

	*val = get_unaligned_be32(data_buf);

	return 0;
}

#define ov2710_read_reg(s, r, v) \
	__ov2710_read_reg(s, r, OV2710_REG_VALUE_8BIT, v)

#define ov2710_read_reg16(s, r, v) \
	__ov2710_read_reg(s, r, OV2710_REG_VALUE_16BIT, v)

#define ov2710_read_reg24(s, r, v) \
	__ov2710_read_reg(s, r, OV2710_REG_VALUE_24BIT, v)

static int ov2710_mod_reg(struct ov2710_dev *sensor, u16 reg, u8 mask, u8 val)
{
	u32 readval;
	int ret;

	ret = ov2710_read_reg(sensor, reg, &readval);
	if (ret < 0)
		return ret;

	readval &= ~mask;
	val &= mask;
	val |= readval;

	return ov2710_write_reg(sensor, reg, val);
}

static int ov2710_load_regs(struct ov2710_dev *sensor,
			    const struct ov2710_mode_info *mode)
{
	const struct reg_value *regs = mode->reg_data;
	unsigned int i;
	int ret = 0;
	u16 reg_addr;
	u8 val;

	for (i = 0; i < mode->reg_data_size; ++i, ++regs) {
		reg_addr = regs->reg_addr;
		val = regs->val;

		ret = ov2710_write_reg(sensor, reg_addr, val);
		if (ret)
			break;
	}

	return ret;
}

static void ov2710_power_up(struct ov2710_dev *sensor)
{
	if (!sensor->reset_gpio)
		return;

	gpiod_set_value(sensor->reset_gpio, 0);
	usleep_range(5000, 10000);
}

static void ov2710_power_down(struct ov2710_dev *sensor)
{
	if (!sensor->reset_gpio)
		return;

	gpiod_set_value(sensor->reset_gpio, 1);
	usleep_range(5000, 10000);
}

static int ov2710_bayer_order(struct ov2710_dev *sensor)
{
	u32 format1;
	u32 format2;
	u32 hv_flip;
	int ret;

	ret = ov2710_read_reg(sensor, OV2710_REG_FORMAT1, &format1);
	if (ret < 0)
		return ret;

	ret = ov2710_read_reg(sensor, OV2710_REG_FORMAT2, &format2);
	if (ret < 0)
		return ret;

	hv_flip = (format2 & BIT(2)  << 1) | (format1 & BIT(2));

	sensor->fmt.code = ov2710_hv_flip_bayer_order[hv_flip];

	return 0;
}

static int ov2710_vflip_enable(struct ov2710_dev *sensor)
{
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_FORMAT1, BIT(2), BIT(2));
	if (ret < 0)
		return ret;

	return ov2710_bayer_order(sensor);
}

static int ov2710_vflip_disable(struct ov2710_dev *sensor)
{
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_FORMAT1, BIT(2), BIT(0));
	if (ret < 0)
		return ret;

	return ov2710_bayer_order(sensor);
}

static int ov2710_hflip_enable(struct ov2710_dev *sensor)
{
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_FORMAT2, BIT(2), BIT(2));
	if (ret < 0)
		return ret;

	return ov2710_bayer_order(sensor);
}

static int ov2710_hflip_disable(struct ov2710_dev *sensor)
{
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_FORMAT2, BIT(2), BIT(0));
	if (ret < 0)
		return ret;

	return ov2710_bayer_order(sensor);
}

static int ov2710_test_pattern_set(struct ov2710_dev *sensor, int value)
{
	int ret;

	if (!value)
		return ov2710_mod_reg(sensor, OV2710_REG_ISP_CTRL00, BIT(7), 0);

	ret = ov2710_mod_reg(sensor, OV2710_REG_ISP_CTRL00, 0x03, value - 1);
	if (ret < 0)
		return ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_ISP_CTRL00, BIT(7), BIT(7));
	if (ret < 0)
		return ret;

	return 0;
}

static int ov2710_gain_set(struct ov2710_dev *sensor, bool auto_gain)
{
	struct ov2710_ctrls *ctrls = &sensor->ctrls;
	u32 gain;
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_R_MANUAL, BIT(1),
			     auto_gain ? 0 : BIT(1));
	if (ret < 0)
		return ret;

	if (auto_gain || !ctrls->gain->is_new)
		return 0;

	gain = ctrls->gain->val;

	ret = ov2710_write_reg16(sensor, OV2710_REG_GAIN_PK, gain);

	return 0;
}

static int ov2710_gain_get(struct ov2710_dev *sensor)
{
	u32 gain;
	int ret;

	ret = ov2710_read_reg16(sensor, OV2710_REG_GAIN_PK, &gain);
	if (ret)
		return ret;

	return gain;
}

static int ov2710_exposure_set(struct ov2710_dev *sensor, bool auto_exp)
{
	struct ov2710_ctrls *ctrls = &sensor->ctrls;
	u32 exp;
	int ret;

	ret = ov2710_mod_reg(sensor, OV2710_REG_R_MANUAL, BIT(0),
			     auto_exp ? 0 : BIT(0));
	if (ret < 0)
		return ret;

	if (auto_exp || !ctrls->exposure->is_new)
		return 0;

	exp = (u32)ctrls->exposure->val;
	exp <<= 4;

	return ov2710_write_reg24(sensor, OV2710_REG_EXPOSURE_PK_HIGH, exp);
}

static int ov2710_exposure_get(struct ov2710_dev *sensor)
{
	int ret;
	u32 exp;

	ret = ov2710_read_reg24(sensor, OV2710_REG_EXPOSURE_PK_HIGH, &exp);
	if (ret)
		return ret;

	return exp >> 4;
}

static int ov2710_stream_enable(struct ov2710_dev *sensor)
{
	return ov2710_write_reg(sensor, OV2680_REG_STREAM_CTRL, ~OV2710_REG_STREAM_CTRL_SLEEP);
}

static int ov2710_stream_disable(struct ov2710_dev *sensor)
{
	return ov2710_write_reg(sensor, OV2680_REG_STREAM_CTRL, OV2710_REG_STREAM_CTRL_SLEEP);
}

static int ov2710_mode_set(struct ov2710_dev *sensor)
{
	struct ov2710_ctrls *ctrls = &sensor->ctrls;
	int ret;

	ret = ov2710_gain_set(sensor, false);
	if (ret < 0)
		return ret;

	ret = ov2710_exposure_set(sensor, false);
	if (ret < 0)
		return ret;

	ret = ov2710_load_regs(sensor, sensor->current_mode);
	if (ret < 0)
		return ret;

	if (ctrls->auto_gain->val) {
		ret = ov2710_gain_set(sensor, true);
		if (ret < 0)
			return ret;
	}

	if (ctrls->auto_exp->val == V4L2_EXPOSURE_AUTO) {
		ret = ov2710_exposure_set(sensor, true);
		if (ret < 0)
			return ret;
	}

	sensor->mode_pending_changes = false;

	return 0;
}

static int ov2710_mode_restore(struct ov2710_dev *sensor)
{
	int ret;

	ret = ov2710_load_regs(sensor, &ov2710_mode_init_data);
	if (ret < 0)
		return ret;

	return ov2710_mode_set(sensor);
}

static int ov2710_power_off(struct ov2710_dev *sensor)
{
	if (!sensor->is_enabled)
		return 0;

	clk_disable_unprepare(sensor->xvclk);
	ov2710_power_down(sensor);
	regulator_bulk_disable(OV2710_NUM_SUPPLIES, sensor->supplies);
	sensor->is_enabled = false;

	return 0;
}

static int ov2710_power_on(struct ov2710_dev *sensor)
{
	struct device *dev = ov2710_to_dev(sensor);
	int ret;

	if (sensor->is_enabled)
		return 0;

	ret = regulator_bulk_enable(OV2710_NUM_SUPPLIES, sensor->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	if (!sensor->reset_gpio) {
		ret = ov2710_write_reg(sensor, OV2710_REG_STREAM_CTRL, OV2710_REG_STREAM_CTRL_RESET);
		if (ret != 0) {
			dev_err(dev, "sensor soft reset failed\n");
			return ret;
		}
		usleep_range(1000, 2000);
	} else {
		ov2710_power_down(sensor);
		ov2710_power_up(sensor);
	}

	ret = clk_prepare_enable(sensor->xvclk);
	if (ret < 0)
		return ret;

	sensor->is_enabled = true;

	/* Set clock lane into LP-11 state */
	ov2710_stream_enable(sensor);
	usleep_range(1000, 2000);
	ov2710_stream_disable(sensor);

	return 0;
}

static int ov2710_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (on)
		ret = ov2710_power_on(sensor);
	else
		ret = ov2710_power_off(sensor);

	mutex_unlock(&sensor->lock);

	if (on && ret == 0) {
		ret = v4l2_ctrl_handler_setup(&sensor->ctrls.handler);
		if (ret < 0)
			return ret;

		ret = ov2710_mode_restore(sensor);
	}

	return ret;
}

static int ov2710_s_g_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_frame_interval *fi)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);

	mutex_lock(&sensor->lock);
	fi->interval = sensor->frame_interval;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int ov2710_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (sensor->is_streaming == !!enable)
		goto unlock;

	if (enable && sensor->mode_pending_changes) {
		ret = ov2710_mode_set(sensor);
		if (ret < 0)
			goto unlock;
	}

	if (enable)
		ret = ov2710_stream_enable(sensor);
	else
		ret = ov2710_stream_disable(sensor);

	sensor->is_streaming = !!enable;

unlock:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int ov2710_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);

	if (code->pad != 0 || code->index != 0)
		return -EINVAL;

	code->code = sensor->fmt.code;

	return 0;
}

static int ov2710_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	struct v4l2_mbus_framefmt *fmt = NULL;
	int ret = 0;

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt = v4l2_subdev_get_try_format(&sensor->sd, cfg, format->pad);
#else
		ret = -EINVAL;
#endif
	} else {
		fmt = &sensor->fmt;
	}

	if (fmt)
		format->format = *fmt;

	mutex_unlock(&sensor->lock);

	return ret;
}

static int ov2710_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	struct v4l2_mbus_framefmt *try_fmt;
#endif
	const struct ov2710_mode_info *mode;
	int ret = 0;

	if (format->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	if (sensor->is_streaming) {
		ret = -EBUSY;
		goto unlock;
	}

	mode = v4l2_find_nearest_size(ov2710_mode_data,
				      ARRAY_SIZE(ov2710_mode_data), width,
				      height, fmt->width, fmt->height);
	if (!mode) {
		ret = -EINVAL;
		goto unlock;
	}

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		try_fmt = v4l2_subdev_get_try_format(sd, cfg, 0);
		format->format = *try_fmt;
#endif
		goto unlock;
	}

	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = sensor->fmt.code;
	fmt->colorspace = sensor->fmt.colorspace;

	sensor->current_mode = mode;
	sensor->fmt = format->format;
	sensor->mode_pending_changes = true;

unlock:
	mutex_unlock(&sensor->lock);

	return ret;
}

static int ov2710_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg)
{
	struct v4l2_subdev_format fmt = {
		.which = cfg ? V4L2_SUBDEV_FORMAT_TRY
				: V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.width = 800,
			.height = 600,
		}
	};

	return ov2710_set_fmt(sd, cfg, &fmt);
}

static int ov2710_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;

	if (index >= OV2710_MODE_MAX || index < 0)
		return -EINVAL;

	fse->min_width = ov2710_mode_data[index].width;
	fse->min_height = ov2710_mode_data[index].height;
	fse->max_width = ov2710_mode_data[index].width;
	fse->max_height = ov2710_mode_data[index].height;

	return 0;
}

static int ov2710_enum_frame_interval(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_frame_interval_enum *fie)
{
	struct v4l2_fract tpf;

	if (fie->index >= OV2710_MODE_MAX || fie->width > OV2710_WIDTH_MAX ||
	    fie->height > OV2710_HEIGHT_MAX ||
	    fie->which > V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	tpf.denominator = OV2710_FRAME_RATE;
	tpf.numerator = 1;

	fie->interval = tpf;

	return 0;
}

static int ov2710_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	struct ov2710_ctrls *ctrls = &sensor->ctrls;
	int val;

	if (!sensor->is_enabled)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		val = ov2710_gain_get(sensor);
		if (val < 0)
			return val;
		ctrls->gain->val = val;
		break;
	case V4L2_CID_EXPOSURE:
		val = ov2710_exposure_get(sensor);
		if (val < 0)
			return val;
		ctrls->exposure->val = val;
		break;
	}

	return 0;
}

static int ov2710_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	struct ov2710_ctrls *ctrls = &sensor->ctrls;

	if (!sensor->is_enabled)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		return ov2710_gain_set(sensor, !!ctrl->val);
	case V4L2_CID_GAIN:
		return ov2710_gain_set(sensor, !!ctrls->auto_gain->val);
	case V4L2_CID_EXPOSURE_AUTO:
		return ov2710_exposure_set(sensor, !!ctrl->val);
	case V4L2_CID_EXPOSURE:
		return ov2710_exposure_set(sensor, !!ctrls->auto_exp->val);
	case V4L2_CID_VFLIP:
		if (sensor->is_streaming)
			return -EBUSY;
		if (ctrl->val)
			return ov2710_vflip_enable(sensor);
		else
			return ov2710_vflip_disable(sensor);
	case V4L2_CID_HFLIP:
		if (sensor->is_streaming)
			return -EBUSY;
		if (ctrl->val)
			return ov2710_hflip_enable(sensor);
		else
			return ov2710_hflip_disable(sensor);
	case V4L2_CID_TEST_PATTERN:
		return ov2710_test_pattern_set(sensor, ctrl->val);
	default:
		break;
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops ov2710_ctrl_ops = {
	.g_volatile_ctrl = ov2710_g_volatile_ctrl,
	.s_ctrl = ov2710_s_ctrl,
};

static const struct v4l2_subdev_core_ops ov2710_core_ops = {
	.s_power = ov2710_s_power,
};

static const struct v4l2_subdev_video_ops ov2710_video_ops = {
	.g_frame_interval	= ov2710_s_g_frame_interval,
	.s_frame_interval	= ov2710_s_g_frame_interval,
	.s_stream		= ov2710_s_stream,
};

static const struct v4l2_subdev_pad_ops ov2710_pad_ops = {
	.init_cfg		= ov2710_init_cfg,
	.enum_mbus_code		= ov2710_enum_mbus_code,
	.get_fmt		= ov2710_get_fmt,
	.set_fmt		= ov2710_set_fmt,
	.enum_frame_size	= ov2710_enum_frame_size,
	.enum_frame_interval	= ov2710_enum_frame_interval,
};

static const struct v4l2_subdev_ops ov2710_subdev_ops = {
	.core	= &ov2710_core_ops,
	.video	= &ov2710_video_ops,
	.pad	= &ov2710_pad_ops,
};

static int ov2710_mode_init(struct ov2710_dev *sensor)
{
	const struct ov2710_mode_info *init_mode;

	/* set initial mode */
	sensor->fmt.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	sensor->fmt.width = 1920;
	sensor->fmt.height = 1080;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->fmt.colorspace = V4L2_COLORSPACE_SRGB;

	sensor->frame_interval.denominator = OV2710_FRAME_RATE;
	sensor->frame_interval.numerator = 1;

	init_mode = &ov2710_mode_init_data;

	sensor->current_mode = init_mode;

	sensor->mode_pending_changes = true;

	return 0;
}

static int ov2710_v4l2_register(struct ov2710_dev *sensor)
{
	const struct v4l2_ctrl_ops *ops = &ov2710_ctrl_ops;
	struct ov2710_ctrls *ctrls = &sensor->ctrls;
	struct v4l2_ctrl_handler *hdl = &ctrls->handler;
	int ret = 0;

	v4l2_i2c_subdev_init(&sensor->sd, sensor->i2c_client,
			     &ov2710_subdev_ops);

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sensor->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret < 0)
		return ret;

	v4l2_ctrl_handler_init(hdl, 7);

	hdl->lock = &sensor->lock;

	ctrls->vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	ctrls->hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP, 0, 1, 1, 0);

	ctrls->test_pattern = v4l2_ctrl_new_std_menu_items(hdl,
					&ov2710_ctrl_ops, V4L2_CID_TEST_PATTERN,
					ARRAY_SIZE(test_pattern_menu) - 1,
					0, 0, test_pattern_menu);

	ctrls->auto_exp = v4l2_ctrl_new_std_menu(hdl, ops,
						 V4L2_CID_EXPOSURE_AUTO,
						 V4L2_EXPOSURE_MANUAL, 0,
						 V4L2_EXPOSURE_AUTO);

	ctrls->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
					    0, 32767, 1, 0);

	ctrls->auto_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTOGAIN,
					     0, 1, 1, 1);
	ctrls->gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_GAIN, 0, 2047, 1, 0);

	if (hdl->error) {
		ret = hdl->error;
		goto cleanup_entity;
	}

	ctrls->gain->flags |= V4L2_CTRL_FLAG_VOLATILE;
	ctrls->exposure->flags |= V4L2_CTRL_FLAG_VOLATILE;

	v4l2_ctrl_auto_cluster(2, &ctrls->auto_gain, 0, true);
	v4l2_ctrl_auto_cluster(2, &ctrls->auto_exp, 1, true);

	sensor->sd.ctrl_handler = hdl;

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret < 0)
		goto cleanup_entity;

	return 0;

cleanup_entity:
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(hdl);

	return ret;
}

static int ov2710_get_regulators(struct ov2710_dev *sensor)
{
	int i;

	for (i = 0; i < OV2710_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = ov2710_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       OV2710_NUM_SUPPLIES,
				       sensor->supplies);
}

static int ov2710_check_id(struct ov2710_dev *sensor)
{
	struct device *dev = ov2710_to_dev(sensor);
	u32 chip_id;
	int ret;

	ov2710_power_on(sensor);

	return 0;
}

static int ov2710_parse_dt(struct ov2710_dev *sensor)
{
	struct device *dev = ov2710_to_dev(sensor);
	int ret;

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	ret = PTR_ERR_OR_ZERO(sensor->reset_gpio);
	if (ret < 0) {
		dev_dbg(dev, "error while getting reset gpio: %d\n", ret);
		return ret;
	}

	sensor->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(sensor->xvclk)) {
		dev_err(dev, "xvclk clock missing or invalid\n");
		return PTR_ERR(sensor->xvclk);
	}

	sensor->xvclk_freq = clk_get_rate(sensor->xvclk);
	if (sensor->xvclk_freq != OV2680_XVCLK_VALUE) {
		dev_err(dev, "wrong xvclk frequency %d HZ, expected: %d Hz\n",
			sensor->xvclk_freq, OV2680_XVCLK_VALUE);
		return -EINVAL;
	}

	return 0;
}

static int ov2710_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov2710_dev *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	ret = ov2710_parse_dt(sensor);
	if (ret < 0)
		return -EINVAL;

	ret = ov2710_mode_init(sensor);
	if (ret < 0)
		return ret;

	ret = ov2710_get_regulators(sensor);
	if (ret < 0) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	mutex_init(&sensor->lock);

	ret = ov2710_check_id(sensor);
	if (ret < 0)
		goto lock_destroy;

	ret = ov2710_v4l2_register(sensor);
	if (ret < 0)
		goto lock_destroy;

	dev_info(dev, "ov2710 init correctly\n");

	return 0;

lock_destroy:
	dev_err(dev, "ov2710 init fail: %d\n", ret);
	mutex_destroy(&sensor->lock);

	return ret;
}

static int ov2710_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov2710_dev *sensor = to_ov2710_dev(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);

	return 0;
}

static int __maybe_unused ov2710_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov2710_dev *sensor = to_ov2710_dev(sd);

	if (sensor->is_streaming)
		ov2710_stream_disable(sensor);

	return 0;
}

static int __maybe_unused ov2710_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov2710_dev *sensor = to_ov2710_dev(sd);
	int ret;

	if (sensor->is_streaming) {
		ret = ov2710_stream_enable(sensor);
		if (ret < 0)
			goto stream_disable;
	}

	return 0;

stream_disable:
	ov2710_stream_disable(sensor);
	sensor->is_streaming = false;

	return ret;
}

static const struct dev_pm_ops ov2710_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ov2710_suspend, ov2710_resume)
};

static const struct of_device_id ov2710_dt_ids[] = {
	{ .compatible = "ovti,ov2710" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ov2710_dt_ids);

static struct i2c_driver ov2710_i2c_driver = {
	.driver = {
		.name  = "ov2710",
		.pm = &ov2710_pm_ops,
		.of_match_table	= of_match_ptr(ov2710_dt_ids),
	},
	.probe_new	= ov2710_probe,
	.remove		= ov2710_remove,
};
module_i2c_driver(ov2710_i2c_driver);

MODULE_AUTHOR("Rui Miguel Silva <rui.silva@linaro.org>");
MODULE_DESCRIPTION("OV2710 CMOS Image Sensor driver");
MODULE_LICENSE("GPL v2");
