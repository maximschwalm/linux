// SPDX-License-Identifier: GPL-2.0+
/*
 * rtc/regmap.c -- regmap-based RTC helpers
 *
 * Author: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 */

#include <linux/bcd.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

enum {
	REG_SEC,
	REG_MIN,
	REG_HOUR,
	REG_DAY,
	REG_MONTH,
	REG_YEAR,
	REG_WDAY,	/* week day; optional */

	NUM_TIME_REGS
};

static int rtc_val_to_hour(uint8_t v, unsigned flags)
{
	uint8_t hour, mask, mask_ampm, mask_12hr;

	/* support 12-hour clocks with AM/PM on bit 5 */
	/* or with no 12-hour indication bit */

	mask = 0x3f;
	mask_12hr = FIELD_GET(RTC_12HR_MASK, flags) << 5;
	mask_ampm = FIELD_GET(RTC_AMPM_MASK, flags) << 5;

	if ((flags & RTC_AMPM_BIT5) || (v & mask_12hr))
		mask &= ~(mask_12hr | mask_ampm);

	hour = bcd2bin(v & mask);
	if (v & mask_ampm)
		hour += 12;

	return hour;
}

static uint8_t rtc_val_from_hour(uint8_t hour, unsigned flags)
{
	uint8_t mask_ampm, mask_12hr;

	if (~flags & RTC_SET_12HR)
		return bin2bcd(hour);

	mask_12hr = FIELD_GET(RTC_12HR_MASK, flags) << 5;
	mask_ampm = FIELD_GET(RTC_AMPM_MASK, flags) << 5;

	if (hour > 12)
		hour -= 12;
	else
		mask_ampm = 0;

	return bin2bcd(hour) | mask_12hr | mask_ampm;
}

/**
 * rtc_regmap_read_time - read RTC time
 * @rtc: regmapped RTC device
 * @tm: time structure to be filled from device
 * @reg_base: register offset for SECONDS
 * @flags: register set format flags
 *
 * @return 0 on success, -error value otherwise
 *
 * Reads time from a device using common register set: SEC, MIN, HOUR, DAY, MONTH, YEAR,
 * with WDAY register optionally at end or between time and date registers; all in BCD.
 */
int rtc_regmap_read_time(struct rtc_device *rtc, struct rtc_time *tm,
			 unsigned reg_base, unsigned flags)
{
	struct regmap *regmap = rtc_get_regmap(rtc);
	uint8_t rtc_data[NUM_TIME_REGS], *data;
	int nregs, ret;

	nregs = NUM_TIME_REGS - 1;
	if ((flags & (RTC_HAS_WDAY|RTC_HAS_WDAY_MIDDLE)) == (RTC_HAS_WDAY|RTC_HAS_WDAY_MIDDLE))
		return -EINVAL;
	if (flags & (RTC_HAS_WDAY|RTC_HAS_WDAY_MIDDLE))
		++nregs;

	ret = regmap_bulk_read(regmap, reg_base, rtc_data, nregs);
	if (ret < 0)
		return ret;

	memset(tm, -1, sizeof(*tm));

	data = rtc_data;
	tm->tm_sec = bcd2bin(*data++ & 0x7f);
	tm->tm_min = bcd2bin(*data++ & 0x7f);
	tm->tm_hour = rtc_val_to_hour(*data++, flags);
	if (flags & RTC_HAS_WDAY_MIDDLE)
		tm->tm_wday = *data++ & 0x07;
	tm->tm_mday = bcd2bin(*data++ & 0x3f);
	tm->tm_mon = bcd2bin(*data++ & 0x1f) - 1;
	tm->tm_year = bcd2bin(*data++) + 100;
	if (flags & RTC_HAS_WDAY)
		tm->tm_wday = *data++ & 0x07;

	return 0;
}
EXPORT_SYMBOL_GPL(rtc_regmap_read_time);

/**
 * rtc_regmap_set_time - set RTC time
 * @rtc: regmapped RTC device
 * @tm: time to be written to device
 * @reg_base: register offset for SECONDS
 * @flags: register set format flags
 *
 * @return 0 on success, -error value otherwise
 *
 * Sets time to a device using common register set: SEC, MIN, HOUR, DAY, MONTH, YEAR,
 * with WDAY register optionally at end or between time and date registers; all in BCD.
 */
int rtc_regmap_set_time(struct rtc_device *rtc, const struct rtc_time *tm,
			unsigned reg_base, unsigned flags)
{
	uint8_t rtc_data[NUM_TIME_REGS], *data = rtc_data;
	struct regmap *regmap = rtc_get_regmap(rtc);

	if ((flags & (RTC_HAS_WDAY|RTC_HAS_WDAY_MIDDLE)) == (RTC_HAS_WDAY|RTC_HAS_WDAY_MIDDLE))
		return -EINVAL;

	*data++ = bin2bcd(tm->tm_sec);
	*data++ = bin2bcd(tm->tm_min);
	*data++ = rtc_val_from_hour(tm->tm_hour, flags);
	if (flags & RTC_HAS_WDAY_MIDDLE)
		*data++ = bin2bcd(tm->tm_wday);
	*data++ = bin2bcd(tm->tm_mday);
	*data++ = bin2bcd(tm->tm_mon + 1);
	*data++ = bin2bcd(tm->tm_year - 100);
	if (flags & RTC_HAS_WDAY)
		*data++ = bin2bcd(tm->tm_wday);

	return regmap_bulk_write(regmap, reg_base, rtc_data, data - rtc_data);
}
EXPORT_SYMBOL_GPL(rtc_regmap_set_time);

/**
 * devm_rtc_regmap_allocate_device - allocate and prepare regmapped RTC device
 * @dev: parent device
 * @regmap: parent device
 *
 * @return a struct rtc on success, ERR_PTR otherwise
 *
 * Managed allocation for regmap-based RTC devices. Requires 1-byte regmap.
 */
struct rtc_device *devm_rtc_regmap_allocate_device(struct device *dev,
						   struct regmap *regmap)
{
	struct rtc_device *rtc;

	if (WARN_ON_ONCE(regmap_get_val_bytes(regmap) != 1))
		return ERR_PTR(-EINVAL);

	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc))
		return rtc;

	dev_set_drvdata(&rtc->dev, regmap);
	rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	rtc->range_max = RTC_TIMESTAMP_END_2099;

	return rtc;
}
EXPORT_SYMBOL_GPL(devm_rtc_regmap_allocate_device);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_LICENSE("GPL");
