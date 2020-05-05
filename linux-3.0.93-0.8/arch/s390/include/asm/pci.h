#ifndef __ASM_S390_PCI_H
#define __ASM_S390_PCI_H

/* must be set before including asm-generic/pci.h */
#define PCI_DMA_BUS_IS_PHYS (0)
/* must be set before including pci_clp.h */
#define PCI_BAR_COUNT	6

#include <asm-generic/pci.h>
#include <asm-generic/pci-dma-compat.h>

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0x10000000

#define pcibios_assign_all_busses()	(0)

#define pcibios_set_master(pdev)	do { } while (0)
#define pcibios_fixup_bus(bus)		do { } while (0)
#define pcibios_setup(str)		(NULL)

void __iomem *pci_iomap(struct pci_dev *, int, unsigned long);
void pci_iounmap(struct pci_dev *, void __iomem *);
int pci_domain_nr(struct pci_bus *);
int pci_proc_domain(struct pci_bus *);

/* MSI arch hooks */
#define arch_setup_msi_irqs(dev,nvec,flags)	(-ENODEV)
#define arch_teardown_msi_irqs(dev)		do { } while (0)

struct msi_map {
	unsigned long irq;
	struct msi_desc *msi;
	struct hlist_node msi_chain;
};

extern unsigned int pci_probe;

#endif
