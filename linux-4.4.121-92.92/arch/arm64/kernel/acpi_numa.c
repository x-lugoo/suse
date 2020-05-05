/*
 * ACPI 5.1 based NUMA setup for ARM64
 * Lots of code was borrowed from arch/x86/mm/srat.c
 *
 * Copyright 2004 Andi Kleen, SuSE Labs.
 * Copyright (C) 2013-2016, Linaro Ltd.
 *		Author: Hanjun Guo <hanjun.guo@linaro.org>
 *
 * Reads the ACPI SRAT table to figure out what memory belongs to which CPUs.
 *
 * Called from acpi_numa_init while reading the SRAT and SLIT tables.
 * Assumes all memory regions belonging to a single proximity domain
 * are in one chunk. Holes between them will be included in the node.
 */

#define pr_fmt(fmt) "ACPI: NUMA: " fmt

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/bootmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/topology.h>

#include <acpi/processor.h>
#include <asm/numa.h>

static int cpus_in_srat;
int acpi_numa __initdata;

struct __node_cpu_hwid {
	u32 node_id;    /* logical node containing this CPU */
	u64 cpu_hwid;   /* MPIDR for this CPU */
};

static struct __node_cpu_hwid early_node_cpu_hwid[NR_CPUS] = {
[0 ... NR_CPUS - 1] = {NUMA_NO_NODE, PHYS_CPUID_INVALID} };

void __init bad_srat(void)
{
	pr_err("SRAT: SRAT not used.\n");
	acpi_numa = -1;
}

int __init srat_disabled(void)
{
	return acpi_numa < 0;
}

/* Callback for parsing of the Proximity Domain <-> Memory Area mappings */
int __init
acpi_numa_memory_affinity_init(struct acpi_srat_mem_affinity *ma)
{
	u64 start, end;
	u32 hotpluggable;
	int node, pxm;

	if (srat_disabled())
		goto out_err;
	if (ma->header.length < sizeof(struct acpi_srat_mem_affinity)) {
		pr_err("SRAT: Unexpected header length: %d\n",
		       ma->header.length);
		goto out_err_bad_srat;
	}
	if ((ma->flags & ACPI_SRAT_MEM_ENABLED) == 0)
		goto out_err;
	hotpluggable = ma->flags & ACPI_SRAT_MEM_HOT_PLUGGABLE;
	if (hotpluggable && !IS_ENABLED(CONFIG_MEMORY_HOTPLUG))
		goto out_err;

	start = ma->base_address;
	end = start + ma->length;
	pxm = ma->proximity_domain;
	if (acpi_srat_revision <= 1)
		pxm &= 0xff;

	node = acpi_map_pxm_to_node(pxm);
	if (node == NUMA_NO_NODE || node >= MAX_NUMNODES) {
		pr_err("SRAT: Too many proximity domains.\n");
		goto out_err_bad_srat;
	}

	if (numa_add_memblk(node, start, end) < 0) {
		pr_err("SRAT: Failed to add memblk to node %u [mem %#010Lx-%#010Lx]\n",
		       node, (unsigned long long) start,
		       (unsigned long long) end - 1);
		goto out_err_bad_srat;
	}

	node_set(node, numa_nodes_parsed);

	pr_info("SRAT: Node %u PXM %u [mem %#010Lx-%#010Lx]%s%s\n",
		node, pxm,
		(unsigned long long) start, (unsigned long long) end - 1,
		hotpluggable ? " hotplug" : "",
		ma->flags & ACPI_SRAT_MEM_NON_VOLATILE ? " non-volatile" : "");

	/* Mark hotplug range in memblock. */
	if (hotpluggable && memblock_mark_hotplug(start, ma->length))
		pr_warn("SRAT: Failed to mark hotplug range [mem %#010Lx-%#010Lx] in memblock\n",
			(unsigned long long)start, (unsigned long long)end - 1);

	return 0;
out_err_bad_srat:
	bad_srat();
out_err:
	return -EINVAL;
}

/*
 * Callback for SLIT parsing.  pxm_to_node() returns NUMA_NO_NODE for
 * I/O localities since SRAT does not list them.  I/O localities are
 * not supported at this point.
 */
void __init acpi_numa_slit_init(struct acpi_table_slit *slit)
{
	int i, j;

	for (i = 0; i < slit->locality_count; i++) {
		const int from_node = pxm_to_node(i);

		if (from_node == NUMA_NO_NODE)
			continue;

		for (j = 0; j < slit->locality_count; j++) {
			const int to_node = pxm_to_node(j);

			if (to_node == NUMA_NO_NODE)
				continue;

			numa_set_distance(from_node, to_node,
				slit->entry[slit->locality_count * i + j]);
		}
	}
}

int acpi_numa_get_nid(unsigned int cpu, u64 hwid)
{
	int i;

	for (i = 0; i < cpus_in_srat; i++) {
		if (hwid == early_node_cpu_hwid[i].cpu_hwid)
			return early_node_cpu_hwid[i].node_id;
	}

	return NUMA_NO_NODE;
}

/* Callback for Proximity Domain -> ACPI processor UID mapping */
void __init acpi_numa_gicc_affinity_init(struct acpi_srat_gicc_affinity *pa)
{
	int pxm, node;
	phys_cpuid_t mpidr;

	if (srat_disabled())
		return;

	if (pa->header.length < sizeof(struct acpi_srat_gicc_affinity)) {
		pr_err("SRAT: Invalid SRAT header length: %d\n",
			pa->header.length);
		bad_srat();
		return;
	}

	if (!(pa->flags & ACPI_SRAT_GICC_ENABLED))
		return;

	if (cpus_in_srat >= NR_CPUS) {
		pr_warn_once("SRAT: cpu_to_node_map[%d] is too small, may not be able to use all cpus\n",
			     NR_CPUS);
		return;
	}

	pxm = pa->proximity_domain;
	node = acpi_map_pxm_to_node(pxm);

	if (node == NUMA_NO_NODE || node >= MAX_NUMNODES) {
		pr_err("SRAT: Too many proximity domains %d\n", pxm);
		bad_srat();
		return;
	}

	mpidr = acpi_map_madt_entry(pa->acpi_processor_uid);
	if (mpidr == PHYS_CPUID_INVALID) {
		pr_err("SRAT: PXM %d with ACPI ID %d has no valid MPIDR in MADT\n",
			pxm, pa->acpi_processor_uid);
		bad_srat();
		return;
	}

	early_node_cpu_hwid[cpus_in_srat].node_id = node;
	early_node_cpu_hwid[cpus_in_srat].cpu_hwid =  mpidr;
	node_set(node, numa_nodes_parsed);
	cpus_in_srat++;
	pr_info("SRAT: PXM %d -> MPIDR 0x%Lx -> Node %d\n",
		pxm, mpidr, node);
}

int __init arm64_acpi_numa_init(void)
{
	int ret;

	ret = acpi_numa_init();
	if (ret)
		return ret;

	return srat_disabled() ? -EINVAL : 0;
}
