// SPDX-License-Identifier: GPL-2.0-only

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct asus_usb_cam_power_data {
	struct device *dev;
	struct gpio_desc *power_gpio;
};

static int asus_usb_cam_power_probe(struct platform_device *pdev)
{
	struct asus_usb_cam_power_data *data;
	struct device *dev = &pdev->dev;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(data->power_gpio))
		return dev_err_probe(dev, PTR_ERR(data->power_gpio),
					 "Failed to get power GPIO\n");

	gpiod_set_value(data->power_gpio, 1);

	return 0;
}

static int __maybe_unused asus_usb_cam_power_suspend(struct device *dev)
{
	struct asus_usb_cam_power_data *data = dev_get_drvdata(dev);

	gpiod_set_value(data->power_gpio, 0);

	return 0;
}

static int __maybe_unused asus_usb_cam_power_resume(struct device *dev)
{
	struct asus_usb_cam_power_data *data = dev_get_drvdata(dev);

	gpiod_set_value(data->power_gpio, 1);

	return 0;
}

static SIMPLE_DEV_PM_OPS(asus_usb_cam_power_pm_ops,
			 asus_usb_cam_power_suspend, asus_usb_cam_power_resume);

static const struct of_device_id asus_usb_cam_power_of_match[] = {
	{ .compatible = "asus,usb-cam-power", },
	{},
};
MODULE_DEVICE_TABLE(of, asus_usb_cam_power_of_match);

static struct platform_driver asus_usb_cam_power_driver = {
	.probe = asus_usb_cam_power_probe,
	.driver = {
		.name = "asus-usb-cam-power",
		.pm = &asus_usb_cam_power_pm_ops,
		.of_match_table = asus_usb_cam_power_of_match,
	},
};
module_platform_driver(asus_usb_cam_power_driver);

MODULE_LICENSE("GPL");
