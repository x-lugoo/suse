#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/kernel_stat.h>
#include <linux/seq_file.h>
#include <linux/pci.h>
#include <linux/msi.h>

int pcibios_enable_device(struct pci_dev *pdev, int mask)
{
	return -ENXIO;
}

void pcibios_disable_device(struct pci_dev *pdev)
{
}

resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				       resource_size_t size,
				       resource_size_t align)
{
	return 0;
}

int pci_domain_nr(struct pci_bus *bus)
{
	return 0;
}
EXPORT_SYMBOL_GPL(pci_domain_nr);

int pci_proc_domain(struct pci_bus *bus)
{
	return 0;
}
EXPORT_SYMBOL_GPL(pci_proc_domain);

void synchronize_irq(unsigned int irq)
{
}
EXPORT_SYMBOL_GPL(synchronize_irq);

void enable_irq(unsigned int irq)
{
}
EXPORT_SYMBOL_GPL(enable_irq);

void disable_irq(unsigned int irq)
{
}
EXPORT_SYMBOL_GPL(disable_irq);

void disable_irq_nosync(unsigned int irq)
{
}
EXPORT_SYMBOL_GPL(disable_irq_nosync);

unsigned long probe_irq_on(void)
{
	return 0;
}
EXPORT_SYMBOL_GPL(probe_irq_on);

int probe_irq_off(unsigned long val)
{
	return 0;
}
EXPORT_SYMBOL_GPL(probe_irq_off);

unsigned int probe_irq_mask(unsigned long val)
{
	return val;
}
EXPORT_SYMBOL_GPL(probe_irq_mask);

void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long max)
{
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_iomap);

void pci_iounmap(struct pci_dev *pdev, void __iomem *addr)
{
}
EXPORT_SYMBOL_GPL(pci_iounmap);

int request_irq(unsigned int irq, irq_handler_t handler,
                unsigned long irqflags, const char *devname, void *dev_id)
{
        return -ENODEV;
}
EXPORT_SYMBOL_GPL(request_irq);

void free_irq(unsigned int irq, void *dev_id)
{
}
EXPORT_SYMBOL_GPL(free_irq);

unsigned int pci_probe = 1;
EXPORT_SYMBOL_GPL(pci_probe);

static int __init pci_base_init(void)
{
	return -ENODEV;
}
subsys_initcall(pci_base_init);
