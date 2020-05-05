/*
 * This driver dynamically manages the CPU Frequency of the ARM processor.
 * Messages are sent to Videocore either setting or requesting the
 * frequency of the ARM in order to match an appropriate frequency to the
 * current usage of the processor. The policy which selects the frequency
 * to use is defined in the kernel .config file, but can be changed during
 * runtime.
 *
 * Copyright 2011 Broadcom Corporation.  All rights reserved.
 * Copyright 2013,2015 Lubomir Rintel
 *
 * Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2, available at
 * http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
 *
 * Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a
 * license other than the GPL, without Broadcom's express prior written
 * consent.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define VCMSG_SET_CLOCK_RATE	0x00038002
#define VCMSG_GET_CLOCK_RATE	0x00030002
#define VCMSG_GET_MIN_CLOCK	0x00030007
#define VCMSG_GET_MAX_CLOCK	0x00030004
#define VCMSG_ID_ARM_CLOCK	0x00000003	/* Clock/Voltage ID's */

struct rpi_firmware *fw;

/* tag part of the message */
struct prop {
	u32 dev_id;		/* the ID of the clock/voltage to get or set */
	u32 val;		/* the value (e.g. rate (in Hz)) to set */
} __packed;

static u32 bcm2835_cpufreq_set_clock(int cur_rate, int arm_rate)
{
	int ret = 0;
	struct prop msg = {
		.dev_id = VCMSG_ID_ARM_CLOCK,
		.val = arm_rate * 1000,
	};

	/* send the message */
	ret = rpi_firmware_property(fw, VCMSG_SET_CLOCK_RATE, &msg,
							sizeof(msg));

	/* check if it was all ok and return the rate in KHz */
	if (ret) {
		pr_err("bcm2835_cpufreq: could not set clock rate\n");
		return 0;
	}

	return msg.val / 1000;
}

static u32 bcm2835_cpufreq_get_clock(int tag)
{
	int ret = 0;
	struct prop msg = {
		.dev_id = VCMSG_ID_ARM_CLOCK,
		.val = 0,
	};

	/* send the message */
	ret = rpi_firmware_property(fw, tag, &msg, sizeof(msg));

	/* check if it was all ok and return the rate in KHz */
	if (ret) {
		pr_err("bcm2835_cpufreq: could not get clock %d\n", tag);
		return 0;
	}

	return msg.val / 1000;
}

static int bcm2835_cpufreq_init(struct cpufreq_policy *policy)
{
	/* measured value of how long it takes to change frequency */
	policy->cpuinfo.transition_latency = 355000; /* ns */

	/* now find out what the maximum and minimum frequencies are */
	policy->min = bcm2835_cpufreq_get_clock(VCMSG_GET_MIN_CLOCK);
	policy->max = bcm2835_cpufreq_get_clock(VCMSG_GET_MAX_CLOCK);
	policy->cur = bcm2835_cpufreq_get_clock(VCMSG_GET_CLOCK_RATE);

	policy->cpuinfo.min_freq = policy->min;
	policy->cpuinfo.max_freq = policy->max;

	return 0;
}

static int bcm2835_cpufreq_target(struct cpufreq_policy *policy,
						unsigned int target_freq,
						unsigned int relation)
{
	unsigned int target = target_freq;
	u32 cur;

	/* if we are above min and using ondemand, then just use max */
	if (strcmp("ondemand", policy->governor->name) == 0 &&
					target > policy->min)
		target = policy->max;

	/* if the frequency is the same, just quit */
	if (target == policy->cur)
		return 0;

	/* otherwise were good to set the clock frequency */
	policy->cur = bcm2835_cpufreq_set_clock(policy->cur, target);

	cur = bcm2835_cpufreq_set_clock(policy->cur, target);
	if (!cur)
		return -EINVAL;

	policy->cur = cur;
	return 0;
}

static unsigned int bcm2835_cpufreq_get(unsigned int cpu)
{
	return bcm2835_cpufreq_get_clock(VCMSG_GET_CLOCK_RATE);
}

static int bcm2835_cpufreq_verify(struct cpufreq_policy *policy)
{
	return 0;
}

static struct cpufreq_driver bcm2835_cpufreq = {
	.name = "bcm2835-cpufreq",
	.init = bcm2835_cpufreq_init,
	.verify = bcm2835_cpufreq_verify,
	.target = bcm2835_cpufreq_target,
	.get = bcm2835_cpufreq_get
};

static int bcm2835_cpufreq_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct device_node *fw_node;

	fw_node = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
	if (!fw_node) {
		dev_err(dev, "no firmware node");
		return -ENODEV;
	}

	fw = rpi_firmware_get(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	ret = cpufreq_register_driver(&bcm2835_cpufreq);
	if (ret) {
		dev_err(dev, "Could not register cpufreq driver\n");
		return ret;
	}

	dev_info(dev, "Broadcom BCM2835 CPU frequency control\n");
	return 0;
}

static int bcm2835_cpufreq_remove(struct platform_device *dev)
{
	cpufreq_unregister_driver(&bcm2835_cpufreq);
	return 0;
}

static const struct of_device_id bcm2835_cpufreq_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-cpufreq", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_cpufreq_of_match);

static struct platform_driver bcm2835_cpufreq_driver = {
	.probe = bcm2835_cpufreq_probe,
	.remove = bcm2835_cpufreq_remove,
	.driver = {
		.name = "bcm2835-cpufreq",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_cpufreq_of_match,
	},
};
module_platform_driver(bcm2835_cpufreq_driver);

MODULE_AUTHOR("Dorian Peake and Dom Cobley and Lubomir Rintel");
MODULE_DESCRIPTION("BCM2835 CPU frequency control driver");
MODULE_LICENSE("GPL v2");
