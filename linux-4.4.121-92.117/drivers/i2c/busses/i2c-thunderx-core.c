/*
 * Cavium ThunderX i2c driver.
 *
 * Copyright (C) 2015,2016 Cavium Inc.
 * Authors: Fred Martin <fmartin@caviumnetworks.com>
 *	    Jan Glauber <jglauber@cavium.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/of_irq.h>

#include "i2c-cavium.h"

#define DRV_NAME "i2c-thunderx"

#define PCI_CFG_REG_BAR_NUM		0
#define PCI_DEVICE_ID_THUNDER_TWSI	0xa012

#define TWSI_DFL_RATE			100000
#define SYS_FREQ_DEFAULT		800000000

#define TWSI_INT_ENA_W1C		0x1028
#define TWSI_INT_ENA_W1S		0x1030

/*
 * Enable the CORE interrupt.
 * The interrupt will be asserted when there is non-STAT_IDLE state in the
 * SW_TWSI_EOP_TWSI_STAT register.
 */
static void thunder_i2c_int_enable(struct octeon_i2c *i2c)
{
	__raw_writeq(TWSI_INT_CORE_INT, i2c->twsi_base + TWSI_INT_ENA_W1S);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1S);
}

/*
 * Disable the CORE interrupt.
 */
static void thunder_i2c_int_disable(struct octeon_i2c *i2c)
{
	__raw_writeq(TWSI_INT_CORE_INT, i2c->twsi_base + TWSI_INT_ENA_W1C);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1C);
}

static void thunder_i2c_hlc_int_enable(struct octeon_i2c *i2c)
{
	__raw_writeq(TWSI_INT_ST_INT | TWSI_INT_TS_INT,
		     i2c->twsi_base + TWSI_INT_ENA_W1S);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1S);
}

static void thunder_i2c_hlc_int_disable(struct octeon_i2c *i2c)
{
	__raw_writeq(TWSI_INT_ST_INT | TWSI_INT_TS_INT,
		     i2c->twsi_base + TWSI_INT_ENA_W1C);
	__raw_readq(i2c->twsi_base + TWSI_INT_ENA_W1C);
}

static u32 thunderx_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm thunderx_i2c_algo = {
	.master_xfer = octeon_i2c_xfer,
	.functionality = thunderx_i2c_functionality,
};

static struct i2c_adapter thunderx_i2c_ops = {
	.owner	= THIS_MODULE,
	.name	= "ThunderX adapter",
	.algo	= &thunderx_i2c_algo,
};

static void thunder_i2c_clock_enable(struct device *dev, struct octeon_i2c *i2c)
{
	int ret;

	i2c->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(i2c->clk)) {
		i2c->clk = NULL;
		goto skip;
	}

	ret = clk_prepare_enable(i2c->clk);
	if (ret)
		goto skip;
	i2c->sys_freq = clk_get_rate(i2c->clk);

skip:
	if (!i2c->sys_freq)
		i2c->sys_freq = SYS_FREQ_DEFAULT;

	dev_info(dev, "Set system clock to %u\n", i2c->sys_freq);
}

static void thunder_i2c_clock_disable(struct device *dev, struct clk *clk)
{
	if (!clk)
		return;
	clk_disable_unprepare(clk);
	devm_clk_put(dev, clk);
}

static int thunder_i2c_smbus_setup(struct octeon_i2c *i2c,
				   struct device_node *node)
{
#if IS_ENABLED(CONFIG_I2C_SMBUS)
	u32 type;

	i2c->alert_data.irq = irq_of_parse_and_map(node, 0);
	if (!i2c->alert_data.irq)
		return -EINVAL;

	type = irqd_get_trigger_type(irq_get_irq_data(i2c->alert_data.irq));
	i2c->alert_data.alert_edge_triggered =
		(type & IRQ_TYPE_LEVEL_MASK) ? 1 : 0;

	i2c->ara = i2c_setup_smbus_alert(&i2c->adap, &i2c->alert_data);
	if (!i2c->ara)
		return -ENODEV;
#endif
	return 0;
}

static void thunder_i2c_smbus_remove(struct octeon_i2c *i2c)
{
#if IS_ENABLED(CONFIG_I2C_SMBUS)
	if (i2c->ara)
		i2c_unregister_device(i2c->ara);
#endif
}

static void thunder_i2c_set_name(struct pci_dev *pdev, struct octeon_i2c *i2c,
				 char *name)
{
	u8 i2c_bus_id, soc_node;
	resource_size_t start;

	start = pci_resource_start(pdev, PCI_CFG_REG_BAR_NUM);
	soc_node = (start >> 44) & 0x3;
	i2c_bus_id = (start >> 24) & 0x7;
	snprintf(name, 10, "i2c%d", soc_node * 6 + i2c_bus_id);

	snprintf(i2c->adap.name, sizeof(i2c->adap.name), "thunderx-i2c-%d.%d",
		 soc_node, i2c_bus_id);
}

static int thunder_i2c_probe_pci(struct pci_dev *pdev,
				 const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = NULL;
	struct octeon_i2c *i2c;
	char i2c_name[10];
	int ret = 0;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->dev = dev;
	pci_set_drvdata(pdev, i2c);
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(dev, "Failed to enable PCI device\n");
		goto out_free_i2c;
	}

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret) {
		dev_err(dev, "PCI request regions failed 0x%x\n", ret);
		goto out_disable_device;
	}

	i2c->twsi_base = pci_ioremap_bar(pdev, PCI_CFG_REG_BAR_NUM);
	if (!i2c->twsi_base) {
		dev_err(dev, "Cannot map CSR memory space\n");
		ret = -EINVAL;
		goto out_release_regions;
	}

	thunder_i2c_clock_enable(dev, i2c);

	thunder_i2c_set_name(pdev, i2c, i2c_name);
	node = of_find_node_by_name(NULL, i2c_name);
	if (!node || of_property_read_u32(node, "clock-frequency",
	    &i2c->twsi_freq))
		i2c->twsi_freq = TWSI_DFL_RATE;

	init_waitqueue_head(&i2c->queue);

	i2c->int_en = thunder_i2c_int_enable;
	i2c->int_dis = thunder_i2c_int_disable;
	i2c->hlc_int_en = thunder_i2c_hlc_int_enable;
	i2c->hlc_int_dis = thunder_i2c_hlc_int_disable;

	ret = pci_enable_msix(pdev, &i2c->i2c_msix, 1);
	if (ret) {
		dev_err(dev, "Unable to enable MSI-X\n");
		goto out_unmap;
	}

	ret = devm_request_irq(dev, i2c->i2c_msix.vector, octeon_i2c_isr, 0,
			       DRV_NAME, i2c);
	if (ret < 0) {
		dev_err(dev, "Failed to attach i2c interrupt\n");
		goto out_msix;
	}

	ret = octeon_i2c_initlowlevel(i2c);
	if (ret) {
		dev_err(dev, "Init low level failed\n");
		goto out_msix;
	}

	octeon_i2c_setclock(i2c);

	i2c->adap = thunderx_i2c_ops;
	i2c->adap.class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	i2c->adap.timeout = HZ / 50;
	i2c->adap.dev.parent = dev;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c_set_adapdata(&i2c->adap, i2c);

	ret = i2c_add_adapter(&i2c->adap);
	if (ret < 0) {
		dev_err(dev, "Failed to add i2c adapter\n");
		goto out_irq;
	}

	ret = thunder_i2c_smbus_setup(i2c, node);
	if (ret < 0)
		dev_err(dev, "Failed to setup smbus alert\n");
	dev_info(i2c->dev, "probed\n");
	return 0;

out_irq:
	devm_free_irq(dev, i2c->i2c_msix.vector, i2c);
out_msix:
	pci_disable_msix(pdev);
out_unmap:
	iounmap(i2c->twsi_base);
	thunder_i2c_clock_disable(dev, i2c->clk);
out_release_regions:
	pci_release_regions(pdev);
out_disable_device:
	pci_disable_device(pdev);
out_free_i2c:
	pci_set_drvdata(pdev, NULL);
	devm_kfree(dev, i2c);
	return ret;
}

static void thunder_i2c_remove_pci(struct pci_dev *pdev)
{
	struct octeon_i2c *i2c = pci_get_drvdata(pdev);
	struct device *dev;

	if (!i2c)
		return;

	dev = i2c->dev;
	thunder_i2c_clock_disable(dev, i2c->clk);
	thunder_i2c_smbus_remove(i2c);
	i2c_del_adapter(&i2c->adap);
	devm_free_irq(dev, i2c->i2c_msix.vector, i2c);
	pci_disable_msix(pdev);
	iounmap(i2c->twsi_base);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	devm_kfree(dev, i2c);
}

static const struct pci_device_id thunder_i2c_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_TWSI) },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, thunder_i2c_pci_id_table);

static struct pci_driver thunder_i2c_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= thunder_i2c_pci_id_table,
	.probe		= thunder_i2c_probe_pci,
	.remove		= thunder_i2c_remove_pci,
};

module_pci_driver(thunder_i2c_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fred Martin <fmartin@caviumnetworks.com>");
MODULE_DESCRIPTION("I2C-Bus adapter for Cavium ThunderX SOC");
