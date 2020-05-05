/*
 *  Copyright (C) 2013 Craig McGeachie
 *  Copyright (C) 2014,2015 Lubomir Rintel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device presents the BCM2835 SoC temperature sensor as a thermal
 * device.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define VC_TAG_GET_TEMP 0x00030006
#define VC_TAG_GET_MAX_TEMP 0x0003000A
#define VC_SUCCESS 0x80000000

struct prop {
	u32 id;
	u32 val;
} __packed;

struct bcm2835_therm {
	struct device *dev;
	struct thermal_zone_device *thermal_dev;
	struct rpi_firmware *fw;
};

static int bcm2835_get_temp_common(struct thermal_zone_device *thermal_dev,
						int *temp, u32 temp_type)
{
	struct bcm2835_therm *therm = thermal_dev->devdata;
	struct device *dev = therm->dev;
	struct prop msg = {
		.id = 0,
		.val = 0
	};
	int ret;

	ret = rpi_firmware_property(therm->fw, temp_type, &msg, sizeof(msg));
	if (ret) {
		dev_err(dev, "VC temperature request failed\n");
		goto exit;
	}

	*temp = msg.val;

exit:
	return ret;
}

static int bcm2835_get_temp(struct thermal_zone_device *thermal_dev, int *temp)
{
	return bcm2835_get_temp_common(thermal_dev, temp, VC_TAG_GET_TEMP);
}

static int bcm2835_get_max_temp(struct thermal_zone_device *thermal_dev,
						int trip_num, int *temp)
{
	return bcm2835_get_temp_common(thermal_dev, temp, VC_TAG_GET_MAX_TEMP);
}

static int bcm2835_get_trip_type(struct thermal_zone_device *thermal_dev,
			int trip_num, enum thermal_trip_type *trip_type)
{
	*trip_type = THERMAL_TRIP_HOT;

	return 0;
}

static int bcm2835_get_mode(struct thermal_zone_device *thermal_dev,
		enum thermal_device_mode *dev_mode)
{
	*dev_mode = THERMAL_DEVICE_ENABLED;

	return 0;
}

static struct thermal_zone_device_ops ops  = {
	.get_temp = bcm2835_get_temp,
	.get_trip_temp = bcm2835_get_max_temp,
	.get_trip_type = bcm2835_get_trip_type,
	.get_mode = bcm2835_get_mode,
};

static int bcm2835_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcm2835_therm *therm;
	struct device_node *fw;

	therm = devm_kzalloc(dev, sizeof(*therm), GFP_KERNEL);
	if (!therm)
		return -ENOMEM;

	therm->dev = dev;
	dev_set_drvdata(dev, therm);

	fw = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
	if (!fw) {
		dev_err(dev, "no firmware node");
		return -ENODEV;
	}
	therm->fw = rpi_firmware_get(fw);
	if (!therm->fw)
		return -EPROBE_DEFER;

	therm->thermal_dev = thermal_zone_device_register("bcm2835_thermal",
					1, 0, therm, &ops, NULL, 0, 0);
	if (IS_ERR(therm->thermal_dev)) {
		dev_err(dev, "Unable to register the thermal device");
		return PTR_ERR(therm->thermal_dev);
	}

	dev_info(dev, "Broadcom BCM2835 thermal sensor\n");

	return 0;
}

static int bcm2835_thermal_remove(struct platform_device *pdev)
{
	struct bcm2835_therm *therm = dev_get_drvdata(&pdev->dev);

	thermal_zone_device_unregister(therm->thermal_dev);

	return 0;
}

static const struct of_device_id bcm2835_thermal_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-thermal", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_thermal_of_match);

static struct platform_driver bcm2835_thermal_driver = {
	.driver = {
		.name = "bcm2835_thermal",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_thermal_of_match,
	},
	.probe = bcm2835_thermal_probe,
	.remove = bcm2835_thermal_remove,
};

module_platform_driver(bcm2835_thermal_driver);

MODULE_AUTHOR("Craig McGeachie");
MODULE_AUTHOR("Lubomir Rintel");
MODULE_DESCRIPTION("Raspberry Pi BCM2835 thermal driver");
MODULE_LICENSE("GPL v2");
