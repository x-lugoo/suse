/*
 * Cavium ThunderX memory controller kernel module
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright Cavium, Inc. (C) 2015. All rights reserved.
 *
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/stop_machine.h>

#include <asm/cacheflush.h>

#include "edac_core.h"
#include "edac_module.h"

#define PCI_DEVICE_ID_THUNDER_LMC 0xa022

#define LMC_FADR		0x20
#define LMC_FADR_FDIMM(x)	((x >> 37) & 0x1)
#define LMC_FADR_FBUNK(x)	((x >> 36) & 0x1)
#define LMC_FADR_FBANK(x)	((x >> 32) & 0xf)
#define LMC_FADR_FROW(x)	((x >> 17) & 0xfffe)
#define LMC_FADR_FCOL(x)	((x >> 17) & 0x1fff)

#define LMC_NXM_FADR		0x28
#define LMC_ECC_SYND		0x38

#define LMC_ECC_PARITY_TEST	0x108

#define LMC_INT_W1S		0x150
#define LMC_INT_ENA_W1C		0x158
#define LMC_INT_ENA_W1S		0x160

#define LMC_INT			0x1F0

#define LMC_INT_MACRAM_DED_ERR	(1 << 13)
#define LMC_INT_MACRAM_SEC_ERR	(1 << 12)
#define LMC_INT_DDR_ERR		(1 << 11)
#define LMC_INT_DLCRAM_DED_ERR	(1 << 10)
#define LMC_INT_DLCRAM_SEC_ERR	(1 << 9)
#define LMC_INT_DED_ERR		(0xf << 5)
#define LMC_INT_SEC_ERR		(0xf << 1)
#define LMC_INT_NXM_WR_MASK	(1 << 0)

#define LMC_INT_UNCORR		(LMC_INT_MACRAM_DED_ERR | LMC_INT_DDR_ERR | \
				 LMC_INT_DLCRAM_DED_ERR | LMC_INT_DED_ERR)

#define LMC_INT_CORR		(LMC_INT_MACRAM_SEC_ERR | \
				 LMC_INT_DLCRAM_SEC_ERR | LMC_INT_SEC_ERR)

#define LMC_INT_UNKNOWN		(~0ULL - (LMC_INT_UNCORR | LMC_INT_CORR))

#define LMC_INT_EN		0x1E8

#define LMC_INT_EN_DDR_ERROR_ALERT_ENA	(1 << 5)
#define LMC_INT_EN_DLCRAM_DED_ERR	(1 << 4)
#define LMC_INT_EN_DLCRAM_SEC_ERR	(1 << 3)
#define LMC_INT_INTR_DED_ENA		(1 << 2)
#define LMC_INT_INTR_SEC_ENA		(1 << 1)
#define LMC_INT_INTR_NXM_WR_ENA		(1 << 0)

#define LMC_INT_EN_ALL			((1 << 6) - 1)
#define LMC_INT_ENA_ALL			((1 << 14) - 1)

#define LMC_DDR_PLL_CTL		0x258
#define LMC_DDR_PLL_CTL_DDR4	(1 << 29)

#define LMC_CONTROL		0x190
#define LMC_CONTROL_RDIMM	(1 << 0)

#define LMC_SCRAM_FADR		0x330

#define LMC_CHAR_MASK0		0x228
#define LMC_CHAR_MASK2		0x238

struct thunderx_lmc {
	void __iomem *regs;
	struct pci_dev *pdev;
	struct msix_entry msix_ent;

	u64 mask0;
	u64 mask2;
	u64 parity_test;
	u64 *mem;
};

#define to_mci(k) container_of(k, struct mem_ctl_info, dev)

#define LMC_SYSFS_ATTR(_field)						    \
static ssize_t thunderx_lmc_ecc_##_field##_show(struct device *dev,	    \
					  struct device_attribute *mattr,   \
					  char *data)			    \
{									    \
	struct mem_ctl_info *mci = to_mci(dev);				    \
	struct thunderx_lmc *lmc = mci->pvt_info;			    \
									    \
	return sprintf(data, "0x%016llx", lmc->_field);			    \
}									    \
									    \
static ssize_t thunderx_lmc_ecc_##_field##_store(struct device *dev,	    \
					   struct device_attribute *mattr,  \
					   const char *data, size_t count)  \
{									    \
	struct mem_ctl_info *mci = to_mci(dev);				    \
	struct thunderx_lmc *lmc = mci->pvt_info;			    \
	int res = kstrtoull(data, 0, &lmc->_field);			    \
									    \
	return res ? res : count;					    \
}									    \
									    \
struct device_attribute dev_attr_ecc_##_field =				    \
__ATTR(_field, S_IRUGO | S_IWUSR,					    \
	thunderx_lmc_ecc_##_field##_show,				    \
	thunderx_lmc_ecc_##_field##_store)

/*
 * To get the ECC error injection, the following steps are needed:
 * - Setup the ECC injection by writing the appropriate parameters:
 *	echo <bit mask value> > /sys/devices/system/edac/mc/mc0/ecc_mask0
 *	echo <bit mask value> > /sys/devices/system/edac/mc/mc0/ecc_mask2
 *	echo 0x802 > /sys/devices/system/edac/mc/mc0/ecc_parity_test
 * - Do the actual injection:
 *	echo 1 > /sys/devices/system/edac/mc/mc0/inject_ecc
 */

static ssize_t thunderx_lmc_inject_int_store(struct device *dev,
					     struct device_attribute *mattr,
					     const char *data, size_t count)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct thunderx_lmc *lmc = mci->pvt_info;
	u64 val;

	int res = kstrtoull(data, 0, &val);

	if (!res) {
		/* Trigger the interrupt */
		writeq(val, lmc->regs + LMC_INT_W1S);
		res = count;
	}

	return res;
}

#define TEST_PATTERN 0xa5

int inject_ecc_fn(void *arg)
{
	struct thunderx_lmc *lmc = arg;
	uintptr_t addr;
	unsigned int cline_size = cache_line_size();
	const unsigned int lines = PAGE_SIZE / cline_size;
	unsigned int i;

	addr = (uintptr_t)lmc->mem;

	writeq(lmc->mask0, lmc->regs + LMC_CHAR_MASK0);
	writeq(lmc->mask2, lmc->regs + LMC_CHAR_MASK2);
	writeq(lmc->parity_test, lmc->regs + LMC_ECC_PARITY_TEST);

	for (i = 0; i < lines; i++) {
		memset((void *)addr, TEST_PATTERN, cline_size);
		barrier();
		/* Do a cacheline PoU flush followed by invalidation
		 * This should cause a DRAM write. Next load should
		 * generate an error interrupt
		 */
		asm volatile("dc cvau, %0\n"
			     "dsb sy\n"
			     "dc civac, %0\n"
			     "dsb sy\n"
				: : "r"(addr));
		addr += cline_size;
	}

	return 0;
}

static ssize_t thunderx_lmc_inject_ecc_store(struct device *dev,
					     struct device_attribute *mattr,
					     const char *data, size_t count)
{
	struct mem_ctl_info *mci = to_mci(dev);
	struct thunderx_lmc *lmc = mci->pvt_info;

	u64 tmp[2];
	uintptr_t addr;
	unsigned int i;

	lmc->mem = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!lmc->mem)
		return -ENOMEM;

	stop_machine(inject_ecc_fn, lmc, NULL);

	addr = (uintptr_t)lmc->mem;

	for (i = 0; i < PAGE_SIZE; i += 2 * sizeof(u64)) {
		/* Do a load from the previously rigged location
		 * This should generate an error interrupt
		 */
		asm volatile("ldp %0, %1, [%2]\n"
			     "dsb ld\n"
				: "=r"(tmp[0]), "=r"(tmp[1])
				: "r"(addr + i));
	}

	kfree(lmc->mem);

	return count;
}

LMC_SYSFS_ATTR(mask0);
LMC_SYSFS_ATTR(mask2);
LMC_SYSFS_ATTR(parity_test);

DEVICE_ATTR(inject_int, S_IWUSR, NULL, thunderx_lmc_inject_int_store);
DEVICE_ATTR(inject_ecc, S_IWUSR, NULL, thunderx_lmc_inject_ecc_store);

struct device_attribute *lmc_devattr[] = {
	&dev_attr_ecc_mask0,
	&dev_attr_ecc_mask2,
	&dev_attr_ecc_parity_test,
	&dev_attr_inject_ecc,
	&dev_attr_inject_int,
};

static int thunderx_create_sysfs_attrs(struct device *dev,
				       struct device_attribute *attrs[],
				       size_t num)
{
	int rc, i;

	for (i = 0; i < num; i++) {
		rc = device_create_file(dev, attrs[i]);
		if (rc < 0)
			return rc;
	}


	return 0;
}

static void thunderx_remove_sysfs_attrs(struct device *dev,
					struct device_attribute *attrs[],
					size_t num)
{
	int i;

	for (i = num - 1; i >= 0; --i)
		device_remove_file(dev, attrs[i]);
}

static irqreturn_t thunderx_lmc_err_isr(int irq, void *dev_id)
{
	struct mem_ctl_info *mci = dev_id;
	struct thunderx_lmc *lmc = mci->pvt_info;
	char msg[64], other[64];

	u64 lmc_int, lmc_fadr, lmc_nxm_fadr,
	    lmc_scram_fadr, lmc_ecc_synd;

	writeq(0, lmc->regs + LMC_CHAR_MASK0);
	writeq(0, lmc->regs + LMC_CHAR_MASK2);
	writeq(0x2, lmc->regs + LMC_ECC_PARITY_TEST);

	lmc_int = readq(lmc->regs + LMC_INT);
	lmc_fadr = readq(lmc->regs + LMC_FADR);
	lmc_nxm_fadr = readq(lmc->regs + LMC_NXM_FADR);
	lmc_scram_fadr = readq(lmc->regs + LMC_SCRAM_FADR);
	lmc_ecc_synd = readq(lmc->regs + LMC_ECC_SYND);

	/* Clear the interrupt */
	writeq(lmc_int, lmc->regs + LMC_INT);

	dev_dbg(&lmc->pdev->dev, "LMC_INT: %016llx\n", lmc_int);
	dev_dbg(&lmc->pdev->dev, "LMC_FADR: %016llx\n", lmc_fadr);
	dev_dbg(&lmc->pdev->dev, "LMC_NXM_FADR: %016llx\n", lmc_nxm_fadr);
	dev_dbg(&lmc->pdev->dev, "LMC_SCRAM_FADR: %016llx\n", lmc_scram_fadr);
	dev_dbg(&lmc->pdev->dev, "LMC_ECC_SYND: %016llx\n", lmc_ecc_synd);

	snprintf(msg, sizeof(msg),
		 "DIMM %lld rank %lld bank %lld row %lld col %lld",
		 LMC_FADR_FDIMM(lmc_fadr), LMC_FADR_FBUNK(lmc_fadr),
		 LMC_FADR_FBANK(lmc_fadr), LMC_FADR_FROW(lmc_fadr),
		 LMC_FADR_FCOL(lmc_fadr));

	snprintf(other, sizeof(other),
		 "%s%s%s%s",
		 lmc_int & (LMC_INT_DLCRAM_DED_ERR | LMC_INT_DLCRAM_SEC_ERR) ?
			"DLC " : "",
		 lmc_int & (LMC_INT_MACRAM_DED_ERR | LMC_INT_MACRAM_SEC_ERR) ?
			"MAC " : "",
		 lmc_int & (LMC_INT_DDR_ERR) ?
			"DDR " : "",
		 lmc_int & (LMC_INT_UNKNOWN) ?
			"Unknown" : "");

	if (lmc_int & LMC_INT_CORR)
		edac_mc_handle_error(HW_EVENT_ERR_CORRECTED, mci, 1, 0, 0, 0,
				     -1, -1, -1, msg, other);

	if (lmc_int & LMC_INT_UNCORR)
		edac_mc_handle_error(HW_EVENT_ERR_UNCORRECTED, mci, 1, 0, 0, 0,
				     -1, -1, -1, msg, other);

	if (lmc_int & LMC_INT_UNKNOWN)
		edac_mc_handle_error(HW_EVENT_ERR_INFO, mci, 1, 0, 0, 0,
				     -1, -1, -1, msg, other);

	return IRQ_HANDLED;
}

#ifdef CONFIG_PM
static int thunderx_lmc_suspend(struct pci_dev *pdev, pm_message_t state)
{
	pci_save_state(pdev);
	pci_disable_device(pdev);

	pci_set_power_state(pdev, pci_choose_state(pdev, state));

	return 0;
}

static int thunderx_lmc_resume(struct pci_dev *pdev)
{
	pci_set_power_state(pdev, PCI_D0);
	pci_enable_wake(pdev, PCI_D0, 0);
	pci_restore_state(pdev);

	return 0;
}
#endif

static const struct pci_device_id thunderx_lmc_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_LMC) },
	{ 0, },
};

/*
 * Per domain mappings (up to 4 domains):
 *
 *  domain 1: 0..3
 *  domain 4: 4..7
 */
#define pci_dev_to_mc_idx(dev)	\
	((pci_domain_nr((dev)->bus) & 6) | PCI_FUNC((dev)->devfn))

static int thunderx_lmc_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct thunderx_lmc *lmc;
	struct edac_mc_layer layer;
	struct mem_ctl_info *mci;
	u64 lmc_control, lmc_ddr_pll_ctl;
	int err;
	u64 lmc_int;

	layer.type = EDAC_MC_LAYER_SLOT;
	layer.size = 2;
	layer.is_virt_csrow = false;

	mci = edac_mc_alloc(pci_dev_to_mc_idx(pdev), 1, &layer,
			    sizeof(struct thunderx_lmc));
	if (!mci)
		return -ENOMEM;

	mci->pdev = &pdev->dev;
	lmc = mci->pvt_info;

	pci_set_drvdata(pdev, mci);

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable PCI device: %d\n", err);
		goto err_kfree;
	}

	err = pcim_iomap_regions(pdev, 1 << 0, "thunderx_lmc");
	if (err) {
		dev_err(&pdev->dev, "Cannot map PCI resources: %d\n", err);
		goto err_kfree;
	}

	lmc->regs = pcim_iomap_table(pdev)[0];

	lmc_control = readq(lmc->regs + LMC_CONTROL);
	lmc_ddr_pll_ctl = readq(lmc->regs + LMC_DDR_PLL_CTL);

	if (lmc_control & LMC_CONTROL_RDIMM) {
		mci->mtype_cap = (lmc_ddr_pll_ctl & LMC_DDR_PLL_CTL_DDR4) ?
				MEM_RDDR4 : MEM_RDDR3;
	} else {
		mci->mtype_cap = (lmc_ddr_pll_ctl & LMC_DDR_PLL_CTL_DDR4) ?
				MEM_DDR4 : MEM_DDR3;
	}

	mci->edac_ctl_cap = EDAC_FLAG_NONE | EDAC_FLAG_SECDED;
	mci->edac_cap = EDAC_FLAG_SECDED;

	mci->mod_name = "thunderx-lmc";
	mci->mod_ver = "1";
	mci->ctl_name = "thunderx-lmc-err";
	mci->dev_name = dev_name(&pdev->dev);
	mci->scrub_mode = SCRUB_NONE;

	err = edac_mc_add_mc(mci);
	if (err) {
		dev_err(&pdev->dev, "Cannot add the MC: %d\n", err);
		goto err_kfree;
	}

	err = thunderx_create_sysfs_attrs(&mci->dev, lmc_devattr,
					  ARRAY_SIZE(lmc_devattr));
	if (err) {
		dev_err(&pdev->dev, "Cannot add device attrs: %d\n", err);
		goto err_del_mc;
	}

	lmc->pdev = pdev;
	lmc->msix_ent.entry = 0;

	err = pci_enable_msix_exact(pdev, &lmc->msix_ent, 1);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable interrupt: %d\n", err);
		goto err_del_attrs;
	}

	err = devm_request_irq(&pdev->dev, lmc->msix_ent.vector,
			       thunderx_lmc_err_isr, 0,
			       "[EDAC] ThunderX LMC", mci);
	if (err) {
		dev_err(&pdev->dev, "Cannot set ISR: %d\n", err);
		goto err_del_attrs;
	}

	lmc_int = readq(lmc->regs + LMC_INT);
	writeq(lmc_int, lmc->regs + LMC_INT);

	writeq(LMC_INT_EN_ALL, lmc->regs + LMC_INT_EN);
	writeq(LMC_INT_ENA_ALL, lmc->regs + LMC_INT_ENA_W1S);

	return 0;

err_del_attrs:
	thunderx_remove_sysfs_attrs(&mci->dev, lmc_devattr,
				    ARRAY_SIZE(lmc_devattr));
err_del_mc:
	edac_mc_del_mc(mci->pdev);
err_kfree:
	kfree(mci);

	return err;
}

static void thunderx_lmc_remove(struct pci_dev *pdev)
{
	struct mem_ctl_info *mci = pci_get_drvdata(pdev);
	struct thunderx_lmc *lmc = mci->pvt_info;

	writeq(0, lmc->regs + LMC_INT_EN);
	writeq(LMC_INT_ENA_ALL, lmc->regs + LMC_INT_ENA_W1C);

	edac_mc_del_mc(&pdev->dev);
	thunderx_remove_sysfs_attrs(&mci->dev, lmc_devattr,
				    ARRAY_SIZE(lmc_devattr));

	edac_mc_free(mci);
}

MODULE_DEVICE_TABLE(pci, thunderx_lmc_pci_tbl);

static struct pci_driver thunderx_lmc_driver = {
	.name     = "thunderx_lmc_edac",
	.probe    = thunderx_lmc_probe,
	.remove   = thunderx_lmc_remove,
#ifdef CONFIG_PM
	.suspend  = thunderx_lmc_suspend,
	.resume   = thunderx_lmc_resume,
#endif
	.id_table = thunderx_lmc_pci_tbl,
};

/*---------------------- CCPI driver ---------------------------------*/

#define PCI_DEVICE_ID_THUNDER_OCX 0xa013

#define OCX_INTS		4

#define OCX_COM_INT		0x100
#define OCX_COM_INT_W1S		0x108
#define OCX_COM_INT_ENA_W1S	0x110
#define OCX_COM_INT_ENA_W1C	0x118

#define OCX_COM_LINKX_INT(x)		(0x120 + (x) * 8)
#define OCX_COM_LINKX_INT_W1S(x)	(0x140 + (x) * 8)
#define OCX_COM_LINKX_INT_ENA_W1S(x)	(0x160 + (x) * 8)
#define OCX_COM_LINKX_INT_ENA_W1C(x)	(0x180 + (x) * 8)

#define OCX_COM_INT_ENA_ALL	((0x1fULL << 50) | (0xffffffULL))
#define OCX_COM_LINKX_INT_ENA_ALL	((3 << 12) | (7 << 7) | (0x3f))

#define OCX_TLKX_ECC_CTL(x)		(0x10018 + (x) * 8)
#define OCX_RLKX_ECC_CTL(x)		(0x18018 + (x) * 8)

struct thunderx_ocx {
	void __iomem *regs;
	int com_link;
	struct pci_dev *pdev;
	struct edac_device_ctl_info *edac_dev;

	struct msix_entry msix_ent[OCX_INTS];
};

static irqreturn_t thunderx_ocx_com_isr(int irq, void *irq_id)
{
	struct msix_entry *msix = irq_id;
	struct thunderx_ocx *ocx = container_of(msix, struct thunderx_ocx,
						msix_ent[msix->entry]);

	u64 ocx_com_int = readq(ocx->regs + OCX_COM_INT);

	dev_info(&ocx->pdev->dev, "OCX_COM_INT: %016llx\n", ocx_com_int);

	writeq(ocx_com_int, ocx->regs + OCX_COM_INT);

	edac_device_handle_ue(ocx->edac_dev, 0, 0, ocx->edac_dev->ctl_name);

	return IRQ_HANDLED;
}

static irqreturn_t thunderx_ocx_lnk_isr(int irq, void *irq_id)
{
	struct msix_entry *msix = irq_id;
	struct thunderx_ocx *ocx = container_of(msix, struct thunderx_ocx,
						msix_ent[msix->entry]);

	u64 ocx_com_link_int = readq(ocx->regs +
				     OCX_COM_LINKX_INT(msix->entry - 1));

	dev_info(&ocx->pdev->dev, "OCX_COM_LINK_INT[%d]: %016llx\n",
		 msix->entry - 1, ocx_com_link_int);

	writeq(ocx_com_link_int, ocx->regs +
	       OCX_COM_LINKX_INT(msix->entry - 1));

	edac_device_handle_ue(ocx->edac_dev, 0, 0, ocx->edac_dev->ctl_name);

	return IRQ_HANDLED;
}

#define OCX_SYSFS_ATTR(_name, _reg)					    \
static ssize_t thunderx_ocx_##_name##_show(struct device *dev,		    \
					   struct device_attribute *mattr,  \
					   char *data)			    \
{									    \
	struct pci_dev *pdev = to_pci_dev(dev);				    \
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);		    \
									    \
	return sprintf(data, "0x%016llx",				    \
		       readq(ocx->regs + _reg));			    \
}									    \
									    \
static ssize_t thunderx_ocx_##_name##_store(struct device *dev,		    \
					    struct device_attribute *mattr, \
					    const char *data, size_t count) \
{									    \
	struct pci_dev *pdev = to_pci_dev(dev);				    \
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);		    \
	u64 val;							    \
	int res;							    \
									    \
	res = kstrtoull(data, 0, &val);					    \
									    \
	if (!res) {							    \
		writeq(val, ocx->regs + _reg);				    \
		res = count;						    \
	}								    \
									    \
	return res;							    \
}									    \
									    \
DEVICE_ATTR(_name, S_IRUGO | S_IWUSR,					    \
	    thunderx_ocx_##_name##_show, thunderx_ocx_##_name##_store)

OCX_SYSFS_ATTR(tlk0_ecc_ctl, OCX_TLKX_ECC_CTL(0));
OCX_SYSFS_ATTR(tlk1_ecc_ctl, OCX_TLKX_ECC_CTL(1));
OCX_SYSFS_ATTR(tlk2_ecc_ctl, OCX_TLKX_ECC_CTL(2));

OCX_SYSFS_ATTR(rlk0_ecc_ctl, OCX_RLKX_ECC_CTL(0));
OCX_SYSFS_ATTR(rlk1_ecc_ctl, OCX_RLKX_ECC_CTL(1));
OCX_SYSFS_ATTR(rlk2_ecc_ctl, OCX_RLKX_ECC_CTL(2));

OCX_SYSFS_ATTR(com_link0_int, OCX_COM_LINKX_INT(0));
OCX_SYSFS_ATTR(com_link1_int, OCX_COM_LINKX_INT(1));
OCX_SYSFS_ATTR(com_link2_int, OCX_COM_LINKX_INT(2));

OCX_SYSFS_ATTR(com_int, OCX_COM_INT);

struct device_attribute *ocx_devattr[] = {
	&dev_attr_tlk0_ecc_ctl,
	&dev_attr_tlk1_ecc_ctl,
	&dev_attr_tlk2_ecc_ctl,

	&dev_attr_rlk0_ecc_ctl,
	&dev_attr_rlk1_ecc_ctl,
	&dev_attr_rlk2_ecc_ctl,

	&dev_attr_com_link0_int,
	&dev_attr_com_link1_int,
	&dev_attr_com_link2_int,

	&dev_attr_com_int,
};



static const struct pci_device_id thunderx_ocx_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_OCX) },
	{ 0, },
};


static int thunderx_ocx_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct thunderx_ocx *ocx;
	struct edac_device_ctl_info *edac_dev;
	char name[6];
	int idx;
	int i;
	int err = -ENOMEM;

	idx = edac_device_alloc_index();
	snprintf(name, sizeof(name), "OCX%d", idx);
	edac_dev = edac_device_alloc_ctl_info(sizeof(struct thunderx_ocx),
					name, 1, "CCPI", 1, 0, NULL, 0, idx);
	if (!edac_dev) {
		dev_err(&pdev->dev, "Cannot allocate EDAC device: %d\n", err);
		return err;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable PCI device: %d\n", err);
		goto err_kfree;
	}

	err = pcim_iomap_regions(pdev, 1 << 0, "thunderx_ocx");
	if (err) {
		dev_err(&pdev->dev, "Cannot map PCI resources: %d\n", err);
		goto err_kfree;
	}

	ocx = edac_dev->pvt_info;
	ocx->edac_dev = edac_dev;

	ocx->regs = pcim_iomap_table(pdev)[0];

	if (!ocx->regs) {
		dev_err(&pdev->dev, "Cannot map PCI resources: %d\n", err);
		err = -ENODEV;
		goto err_kfree;
	}

	ocx->pdev = pdev;

	for (i = 0; i < OCX_INTS; i++) {
		ocx->msix_ent[i].entry = i;
		ocx->msix_ent[i].vector = 0;
	}

	err = pci_enable_msix_exact(pdev, ocx->msix_ent, OCX_INTS);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable interrupt: %d\n", err);
		goto err_kfree;
	}

	for (i = 0; i < OCX_INTS; i++) {
		err = devm_request_irq(&pdev->dev, ocx->msix_ent[i].vector,
				       (i == 0) ? thunderx_ocx_com_isr :
						  thunderx_ocx_lnk_isr,
				       0, "[EDAC] ThunderX OCX",
				       &ocx->msix_ent[i]);
		if (err)
			goto err_kfree;
	}

	edac_dev->dev = &pdev->dev;
	edac_dev->dev_name = dev_name(&pdev->dev);
	edac_dev->mod_name = "thunderx-ocx";
	edac_dev->ctl_name = "thunderx-ocx-err";

	err = edac_device_add_device(edac_dev);
	if (err) {
		dev_err(&pdev->dev, "Cannot add EDAC device: %d\n", err);
		goto err_kfree;
	}

	err = thunderx_create_sysfs_attrs(&pdev->dev, ocx_devattr,
					  ARRAY_SIZE(ocx_devattr));
	if (err) {
		dev_err(&pdev->dev, "Cannot add device attrs: %d\n", err);
		goto err_del_dev;
	}

	pci_set_drvdata(pdev, edac_dev);

	writeq(OCX_COM_INT_ENA_ALL, ocx->regs + OCX_COM_INT_ENA_W1S);

	for (i = 0; i < OCX_INTS; i++) {
		writeq(OCX_COM_LINKX_INT_ENA_ALL,
		       ocx->regs + OCX_COM_LINKX_INT_ENA_W1S(i));
	}

	return 0;

err_del_dev:
	edac_device_del_device(&pdev->dev);
err_kfree:
	edac_device_free_ctl_info(edac_dev);

	return err;
}


static void thunderx_ocx_remove(struct pci_dev *pdev)
{
	struct edac_device_ctl_info *edac_dev = pci_get_drvdata(pdev);
	struct thunderx_ocx *ocx = edac_dev->pvt_info;
	int i;

	writeq(OCX_COM_INT_ENA_ALL, ocx->regs + OCX_COM_INT_ENA_W1C);

	for (i = 0; i < OCX_INTS; i++) {
		writeq(OCX_COM_LINKX_INT_ENA_ALL,
		       ocx->regs + OCX_COM_LINKX_INT_ENA_W1C(i));
	}

	edac_device_del_device(&pdev->dev);
	thunderx_remove_sysfs_attrs(&pdev->dev, ocx_devattr,
				    ARRAY_SIZE(ocx_devattr));

	edac_device_free_ctl_info(edac_dev);
}

MODULE_DEVICE_TABLE(pci, thunderx_ocx_pci_tbl);

static struct pci_driver thunderx_ocx_driver = {
	.name     = "thunderx_ocx_edac",
	.probe    = thunderx_ocx_probe,
	.remove   = thunderx_ocx_remove,
	.id_table = thunderx_ocx_pci_tbl,
};

static int __init thunderx_edac_init(void)
{
	int rc = 0;

	rc = pci_register_driver(&thunderx_lmc_driver);

	if (rc)
		return rc;

	rc = pci_register_driver(&thunderx_ocx_driver);

	if (rc) {
		pci_unregister_driver(&thunderx_lmc_driver);
		return rc;
	}

	return rc;
}

static void __exit thunderx_edac_exit(void)
{
	pci_unregister_driver(&thunderx_ocx_driver);
	pci_unregister_driver(&thunderx_lmc_driver);

}

module_init(thunderx_edac_init);
module_exit(thunderx_edac_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Cavium, Inc.");
MODULE_DESCRIPTION("EDAC Driver for Cavium ThunderX");
