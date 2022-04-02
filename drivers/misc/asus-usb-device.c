// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS simple USB device driver
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct asus_usb_device_data {
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power_gpio;
};

static void asus_usb_device_reset(struct asus_usb_device_data *priv)
{
	gpiod_set_value(priv->reset_gpio, 1);
	udelay(1);
	gpiod_set_value(priv->reset_gpio, 0);
	udelay(100);
}

static void asus_usb_device_power(struct asus_usb_device_data *priv, bool state)
{
	if (priv->reset_gpio)
		asus_usb_device_reset(priv);

	gpiod_set_value(priv->power_gpio, state);
}

static int asus_usb_device_probe(struct platform_device *pdev)
{
	struct asus_usb_device_data *priv;
	struct device *dev = &pdev->dev;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
					 "Failed to get reset GPIO\n");

	priv->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(priv->power_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->power_gpio),
					 "Failed to get power GPIO\n");

	asus_usb_device_power(priv, true);

	return 0;
}

static int __maybe_unused asus_usb_device_suspend(struct device *dev)
{
	struct asus_usb_device_data *priv = dev_get_drvdata(dev);

	asus_usb_device_power(priv, false);

	return 0;
}

static int __maybe_unused asus_usb_device_resume(struct device *dev)
{
	struct asus_usb_device_data *priv = dev_get_drvdata(dev);

	asus_usb_device_power(priv, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(asus_usb_device_pm_ops,
			 asus_usb_device_suspend, asus_usb_device_resume);

static const struct of_device_id asus_usb_device_match[] = {
	{ .compatible = "asus,usb-device" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_usb_device_match);

static struct platform_driver asus_usb_device_driver = {
	.driver = {
		.name = "asus-usb-device",
		.pm = &asus_usb_device_pm_ops,
		.of_match_table = asus_usb_device_match,
	},
	.probe = asus_usb_device_probe,
};
module_platform_driver(asus_usb_device_driver);

MODULE_AUTHOR("Maxim Schwalm <maxim.schwalm@gmail.com>");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS simple USB device driver");
MODULE_LICENSE("GPL");
