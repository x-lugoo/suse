/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2009-2015 Cavium, Inc.
 */

#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/phy.h>
#include <linux/io.h>
#include <linux/pci.h>

#ifdef CONFIG_CAVIUM_OCTEON_SOC
#include <asm/octeon/octeon.h>
#endif

#define DRV_VERSION "1.1"
#define DRV_DESCRIPTION "Cavium Networks Octeon/ThunderX SMI/MDIO driver"

#define SMI_CMD		0x0
#define SMI_WR_DAT	0x8
#define SMI_RD_DAT	0x10
#define SMI_CLK		0x18
#define SMI_EN		0x20

#ifdef __BIG_ENDIAN_BITFIELD
#define OCT_MDIO_BITFIELD_FIELD(field, more)	\
	field;					\
	more

#else
#define OCT_MDIO_BITFIELD_FIELD(field, more)	\
	more					\
	field;

#endif

union cvmx_smix_clk {
	u64 u64;
	struct cvmx_smix_clk_s {
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_25_63:39,
	  OCT_MDIO_BITFIELD_FIELD(u64 mode:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_21_23:3,
	  OCT_MDIO_BITFIELD_FIELD(u64 sample_hi:5,
	  OCT_MDIO_BITFIELD_FIELD(u64 sample_mode:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_14_14:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 clk_idle:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 preamble:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 sample:4,
	  OCT_MDIO_BITFIELD_FIELD(u64 phase:8,
	  ;))))))))))
	} s;
};

union cvmx_smix_cmd {
	u64 u64;
	struct cvmx_smix_cmd_s {
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_18_63:46,
	  OCT_MDIO_BITFIELD_FIELD(u64 phy_op:2,
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_13_15:3,
	  OCT_MDIO_BITFIELD_FIELD(u64 phy_adr:5,
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_5_7:3,
	  OCT_MDIO_BITFIELD_FIELD(u64 reg_adr:5,
	  ;))))))
	} s;
};

union cvmx_smix_en {
	u64 u64;
	struct cvmx_smix_en_s {
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_1_63:63,
	  OCT_MDIO_BITFIELD_FIELD(u64 en:1,
	  ;))
	} s;
};

union cvmx_smix_rd_dat {
	u64 u64;
	struct cvmx_smix_rd_dat_s {
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_18_63:46,
	  OCT_MDIO_BITFIELD_FIELD(u64 pending:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 val:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 dat:16,
	  ;))))
	} s;
};

union cvmx_smix_wr_dat {
	u64 u64;
	struct cvmx_smix_wr_dat_s {
	  OCT_MDIO_BITFIELD_FIELD(u64 reserved_18_63:46,
	  OCT_MDIO_BITFIELD_FIELD(u64 pending:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 val:1,
	  OCT_MDIO_BITFIELD_FIELD(u64 dat:16,
	  ;))))
	} s;
};

enum octeon_mdiobus_mode {
	UNINIT = 0,
	C22,
	C45
};

struct octeon_mdiobus {
	struct mii_bus *mii_bus;
	u64 register_base;
	enum octeon_mdiobus_mode mode;
	int phy_irq[PHY_MAX_ADDR];
};

#ifdef CONFIG_CAVIUM_OCTEON_SOC
static void oct_mdio_writeq(u64 val, u64 addr)
{
	cvmx_write_csr(addr, val);
}

static u64 oct_mdio_readq(u64 addr)
{
	return cvmx_read_csr(addr);
}
#else
#define oct_mdio_writeq(val, addr)	writeq(val, (void *)addr)
#define oct_mdio_readq(addr)		readq((void *)addr)
#endif

static void octeon_mdiobus_set_mode(struct octeon_mdiobus *p,
				    enum octeon_mdiobus_mode m)
{
	union cvmx_smix_clk smi_clk;

	if (m == p->mode)
		return;

	smi_clk.u64 = oct_mdio_readq(p->register_base + SMI_CLK);
	smi_clk.s.mode = (m == C45) ? 1 : 0;
	smi_clk.s.preamble = 1;
	oct_mdio_writeq(smi_clk.u64, p->register_base + SMI_CLK);
	p->mode = m;
}

static int octeon_mdiobus_c45_addr(struct octeon_mdiobus *p,
				   int phy_id, int regnum)
{
	union cvmx_smix_cmd smi_cmd;
	union cvmx_smix_wr_dat smi_wr;
	int timeout = 1000;

	octeon_mdiobus_set_mode(p, C45);

	smi_wr.u64 = 0;
	smi_wr.s.dat = regnum & 0xffff;
	oct_mdio_writeq(smi_wr.u64, p->register_base + SMI_WR_DAT);

	regnum = (regnum >> 16) & 0x1f;

	smi_cmd.u64 = 0;
	smi_cmd.s.phy_op = 0; /* MDIO_CLAUSE_45_ADDRESS */
	smi_cmd.s.phy_adr = phy_id;
	smi_cmd.s.reg_adr = regnum;
	oct_mdio_writeq(smi_cmd.u64, p->register_base + SMI_CMD);

	do {
		/* Wait 1000 clocks so we don't saturate the RSL bus
		 * doing reads.
		 */
		__delay(1000);
		smi_wr.u64 = oct_mdio_readq(p->register_base + SMI_WR_DAT);
	} while (smi_wr.s.pending && --timeout);

	if (timeout <= 0)
		return -EIO;
	return 0;
}

static int octeon_mdiobus_read(struct mii_bus *bus, int phy_id, int regnum)
{
	struct octeon_mdiobus *p = bus->priv;
	union cvmx_smix_cmd smi_cmd;
	union cvmx_smix_rd_dat smi_rd;
	unsigned int op = 1; /* MDIO_CLAUSE_22_READ */
	int timeout = 1000;

	if (regnum & MII_ADDR_C45) {
		int r = octeon_mdiobus_c45_addr(p, phy_id, regnum);
		if (r < 0)
			return r;

		regnum = (regnum >> 16) & 0x1f;
		op = 3; /* MDIO_CLAUSE_45_READ */
	} else {
		octeon_mdiobus_set_mode(p, C22);
	}


	smi_cmd.u64 = 0;
	smi_cmd.s.phy_op = op;
	smi_cmd.s.phy_adr = phy_id;
	smi_cmd.s.reg_adr = regnum;
	oct_mdio_writeq(smi_cmd.u64, p->register_base + SMI_CMD);

	do {
		/* Wait 1000 clocks so we don't saturate the RSL bus
		 * doing reads.
		 */
		__delay(1000);
		smi_rd.u64 = oct_mdio_readq(p->register_base + SMI_RD_DAT);
	} while (smi_rd.s.pending && --timeout);

	if (smi_rd.s.val)
		return smi_rd.s.dat;
	else
		return -EIO;
}

static int octeon_mdiobus_write(struct mii_bus *bus, int phy_id,
				int regnum, u16 val)
{
	struct octeon_mdiobus *p = bus->priv;
	union cvmx_smix_cmd smi_cmd;
	union cvmx_smix_wr_dat smi_wr;
	unsigned int op = 0; /* MDIO_CLAUSE_22_WRITE */
	int timeout = 1000;


	if (regnum & MII_ADDR_C45) {
		int r = octeon_mdiobus_c45_addr(p, phy_id, regnum);
		if (r < 0)
			return r;

		regnum = (regnum >> 16) & 0x1f;
		op = 1; /* MDIO_CLAUSE_45_WRITE */
	} else {
		octeon_mdiobus_set_mode(p, C22);
	}

	smi_wr.u64 = 0;
	smi_wr.s.dat = val;
	oct_mdio_writeq(smi_wr.u64, p->register_base + SMI_WR_DAT);

	smi_cmd.u64 = 0;
	smi_cmd.s.phy_op = op;
	smi_cmd.s.phy_adr = phy_id;
	smi_cmd.s.reg_adr = regnum;
	oct_mdio_writeq(smi_cmd.u64, p->register_base + SMI_CMD);

	do {
		/* Wait 1000 clocks so we don't saturate the RSL bus
		 * doing reads.
		 */
		__delay(1000);
		smi_wr.u64 = oct_mdio_readq(p->register_base + SMI_WR_DAT);
	} while (smi_wr.s.pending && --timeout);

	if (timeout <= 0)
		return -EIO;

	return 0;
}

static int octeon_mdiobus_probe(struct platform_device *pdev)
{
	struct octeon_mdiobus *bus;
	struct resource *res_mem;
	resource_size_t mdio_phys;
	resource_size_t regsize;
	union cvmx_smix_en smi_en;
	int err = -ENOENT;

	bus = devm_kzalloc(&pdev->dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	res_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res_mem == NULL) {
		dev_err(&pdev->dev, "found no memory resource\n");
		return -ENXIO;
	}

	mdio_phys = res_mem->start;
	regsize = resource_size(res_mem);

	if (!devm_request_mem_region(&pdev->dev, mdio_phys, regsize,
				     res_mem->name)) {
		dev_err(&pdev->dev, "request_mem_region failed\n");
		return -ENXIO;
	}

	bus->register_base =
		(u64)devm_ioremap(&pdev->dev, mdio_phys, regsize);
	if (!bus->register_base) {
		dev_err(&pdev->dev, "dev_ioremap failed\n");
		return -ENOMEM;
	}

	bus->mii_bus = mdiobus_alloc();
	if (!bus->mii_bus)
		goto fail;

	smi_en.u64 = 0;
	smi_en.s.en = 1;
	oct_mdio_writeq(smi_en.u64, bus->register_base + SMI_EN);

	bus->mii_bus->priv = bus;
	bus->mii_bus->irq = bus->phy_irq;
	bus->mii_bus->name = KBUILD_MODNAME;
	snprintf(bus->mii_bus->id, MII_BUS_ID_SIZE, "%llx", bus->register_base);
	bus->mii_bus->parent = &pdev->dev;

	bus->mii_bus->read = octeon_mdiobus_read;
	bus->mii_bus->write = octeon_mdiobus_write;

	platform_set_drvdata(pdev, bus);

	err = of_mdiobus_register(bus->mii_bus, pdev->dev.of_node);
	if (err)
		goto fail_register;

	dev_info(&pdev->dev, "Version " DRV_VERSION "\n");

	return 0;
fail_register:
	mdiobus_free(bus->mii_bus);
fail:
	smi_en.u64 = 0;
	oct_mdio_writeq(smi_en.u64, bus->register_base + SMI_EN);
	return err;
}

static int octeon_mdiobus_remove(struct platform_device *pdev)
{
	struct octeon_mdiobus *bus;
	union cvmx_smix_en smi_en;

	bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus->mii_bus);
	mdiobus_free(bus->mii_bus);
	smi_en.u64 = 0;
	oct_mdio_writeq(smi_en.u64, bus->register_base + SMI_EN);
	return 0;
}

static const struct of_device_id octeon_mdiobus_match[] = {
	{
		.compatible = "cavium,octeon-3860-mdio",
	},
	{},
};
MODULE_DEVICE_TABLE(of, octeon_mdiobus_match);

static struct platform_driver octeon_mdiobus_driver = {
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table = octeon_mdiobus_match,
	},
	.probe		= octeon_mdiobus_probe,
	.remove		= octeon_mdiobus_remove,
};

void octeon_mdiobus_force_mod_depencency(void)
{
	/* Let ethernet drivers force us to be loaded.  */
}
EXPORT_SYMBOL(octeon_mdiobus_force_mod_depencency);

#ifdef CONFIG_PCI

struct thunder_mdiobus_nexus {
	void __iomem *bar0;
	struct octeon_mdiobus *buses[4];
};

static int thunder_mdiobus_pci_probe(struct pci_dev *pdev,
				     const struct pci_device_id *ent)
{
	struct device_node *node;
	struct fwnode_handle *fwn;
	struct thunder_mdiobus_nexus *nexus;
	int err;
	int i;

	nexus = devm_kzalloc(&pdev->dev, sizeof(*nexus), GFP_KERNEL);
	if (!nexus)
		return -ENOMEM;

	pci_set_drvdata(pdev, nexus);

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		pci_set_drvdata(pdev, NULL);
		return err;
	}

	err = pci_request_regions(pdev, KBUILD_MODNAME);
	if (err) {
		dev_err(&pdev->dev, "pci_request_regions failed\n");
		goto err_disable_device;
	}

	nexus->bar0 = pcim_iomap(pdev, 0, pci_resource_len(pdev, 0));
	if (!nexus->bar0) {
		err = -ENOMEM;
		goto err_release_regions;
	}

	i = 0;
	device_for_each_child_node(&pdev->dev, fwn) {
		struct resource r;
		struct octeon_mdiobus *bus;
		union cvmx_smix_en smi_en;

		/* If it is not an OF node we cannot handle it yet, so
		 * exit the loop.
		 */
		node = to_of_node(fwn);
		if (!node)
			break;

		err = of_address_to_resource(node, 0, &r);
		if (err) {
			dev_err(&pdev->dev,
				"Couldn't translate address for \"%s\"\n",
				node->name);
			break;
		}
		bus = devm_kzalloc(&pdev->dev, sizeof(struct octeon_mdiobus),
				   GFP_KERNEL);

		if (!bus)
			break;

		nexus->buses[i] = bus;
		i++;

		bus->register_base = (u64)nexus->bar0 +
			r.start - pci_resource_start(pdev, 0);

		bus->mii_bus = mdiobus_alloc();
		if (!bus->mii_bus)
			break;

		smi_en.u64 = 0;
		smi_en.s.en = 1;
		oct_mdio_writeq(smi_en.u64, bus->register_base + SMI_EN);
		bus->mii_bus->priv = bus;
		bus->mii_bus->irq = bus->phy_irq;
		bus->mii_bus->name = KBUILD_MODNAME;
		snprintf(bus->mii_bus->id, MII_BUS_ID_SIZE, "%llx", r.start);
		bus->mii_bus->parent = &pdev->dev;
		bus->mii_bus->read = octeon_mdiobus_read;
		bus->mii_bus->write = octeon_mdiobus_write;

		err = of_mdiobus_register(bus->mii_bus, node);
		if (err)
			dev_err(&pdev->dev, "of_mdiobus_register failed\n");

		dev_info(&pdev->dev, "Added bus at %llx\n", r.start);
		if (i >= ARRAY_SIZE(nexus->buses))
			break;
	}
	return 0;

err_release_regions:
	pci_release_regions(pdev);

err_disable_device:
	pci_set_drvdata(pdev, NULL);
	return err;
}

static void thunder_mdiobus_pci_remove(struct pci_dev *pdev)
{
	int i;
	union cvmx_smix_en smi_en;
	struct thunder_mdiobus_nexus *nexus = pci_get_drvdata(pdev);

	for (i = 0; i < ARRAY_SIZE(nexus->buses); i++) {
		struct octeon_mdiobus *bus = nexus->buses[i];

		if (!bus)
			continue;

		mdiobus_unregister(bus->mii_bus);
		mdiobus_free(bus->mii_bus);
		smi_en.u64 = 0;
		oct_mdio_writeq(smi_en.u64, bus->register_base + SMI_EN);
	}
	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id thunder_mdiobus_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, 0xa02b) },
	{ 0, } /* End of table. */
};
MODULE_DEVICE_TABLE(pci, thunder_mdiobus_id_table);

static struct pci_driver thunder_mdiobus_driver = {
	.name = KBUILD_MODNAME,
	.id_table = thunder_mdiobus_id_table,
	.probe = thunder_mdiobus_pci_probe,
	.remove = thunder_mdiobus_pci_remove,
};
#endif /* CONFIG_PCI */

static int __init octeon_mdiobus_driver_init(void)
{
	int r = platform_driver_register(&octeon_mdiobus_driver);

#ifdef CONFIG_PCI
	if (r)
		return r;

	r = pci_register_driver(&thunder_mdiobus_driver);
#endif
	return r;
}
module_init(octeon_mdiobus_driver_init);

static void __exit octeon_mdiobus_driver_exit(void)
{
	platform_driver_unregister(&octeon_mdiobus_driver);
#ifdef CONFIG_PCI
	pci_unregister_driver(&thunder_mdiobus_driver);
#endif
}
module_exit(octeon_mdiobus_driver_exit);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_VERSION(DRV_VERSION);
MODULE_AUTHOR("David Daney");
MODULE_LICENSE("GPL");
