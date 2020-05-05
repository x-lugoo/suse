/*
 * Copyright IBM Corp. 2011
 * Author(s): Jan Glauber <jang@linux.vnet.ibm.com>
 */
#include <linux/hugetlb.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/page.h>

static inline unsigned long sske_frame(unsigned long addr, unsigned char skey)
{
	asm volatile(".insn rrf,0xb22b0000,%[skey],%[addr],9,0"
		     : [addr] "+a" (addr) : [skey] "d" (skey));
	return addr;
}

void storage_key_init_range(unsigned long start, unsigned long end)
{
	unsigned long boundary, size;

	while (start < end) {
		if (MACHINE_HAS_EDAT1) {
			/* set storage keys for a 1MB frame */
			size = 1UL << 20;
			boundary = (start + size) & ~(size - 1);
			if (boundary <= end) {
				do {
					start = sske_frame(start, PAGE_DEFAULT_KEY);
				} while (start < boundary);
				continue;
			}
		}
		page_set_storage_key(start, PAGE_DEFAULT_KEY, 0);
		start += PAGE_SIZE;
	}
}

static void change_page_attr(unsigned long addr, int numpages,
			     pte_t (*set) (pte_t))
{
	pte_t *ptep, pte;
	pmd_t *pmdp;
	pud_t *pudp;
	pgd_t *pgdp;
	int i;

	for (i = 0; i < numpages; i++) {
		pgdp = pgd_offset(&init_mm, addr);
		pudp = pud_offset(pgdp, addr);
		pmdp = pmd_offset(pudp, addr);
		if (pmd_huge(*pmdp)) {
			WARN_ON_ONCE(1);
			continue;
		}
		ptep = pte_offset_kernel(pmdp, addr);

		pte = *ptep;
		pte = set(pte);
		*ptep = pte;
		addr += PAGE_SIZE;
	}
	__tlb_flush_global();
}

int set_memory_ro(unsigned long addr, int numpages)
{
	change_page_attr(addr, numpages, pte_wrprotect);
	return 0;
}
EXPORT_SYMBOL_GPL(set_memory_ro);

int set_memory_rw(unsigned long addr, int numpages)
{
	change_page_attr(addr, numpages, pte_mkwrite);
	return 0;
}
EXPORT_SYMBOL_GPL(set_memory_rw);

/* not possible */
int set_memory_nx(unsigned long addr, int numpages)
{
	return 0;
}
EXPORT_SYMBOL_GPL(set_memory_nx);

int set_memory_x(unsigned long addr, int numpages)
{
	return 0;
}
