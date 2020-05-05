/*
 * Cavium Thunder uncore PMU support. Derived from Intel and AMD uncore code.
 *
 * Copyright (C) 2015,2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/cpufeature.h>

#include "uncore_cavium.h"

int thunder_uncore_version;

struct thunder_uncore *event_to_thunder_uncore(struct perf_event *event)
{
	if (event->pmu->type == thunder_l2c_tad_pmu.type)
		return thunder_uncore_l2c_tad;
	else if (event->pmu->type == thunder_l2c_cbc_pmu.type)
		return thunder_uncore_l2c_cbc;
	else if (event->pmu->type == thunder_lmc_pmu.type)
		return thunder_uncore_lmc;
	else if (event->pmu->type == thunder_ocx_tlk_pmu.type)
		return thunder_uncore_ocx_tlk;
	else
		return NULL;
}

void thunder_uncore_read(struct perf_event *event)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;
	u64 prev, new = 0;
	s64 delta;

	node = get_node(hwc->config, uncore);

	/*
	 * No counter overflow interrupts so we do not
	 * have to worry about prev_count changing on us.
	 */
	prev = local64_read(&hwc->prev_count);

	/* read counter values from all units on the node */
	list_for_each_entry(unit, &node->unit_list, entry)
		new += readq(hwc->event_base + unit->map);

	local64_set(&hwc->prev_count, new);
	delta = new - prev;
	local64_add(delta, &event->count);
}

void thunder_uncore_del(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int i;

	event->pmu->stop(event, PERF_EF_UPDATE);

	/*
	 * For programmable counters we need to check where we installed it.
	 * To keep this function generic always test the more complicated
	 * case (free running counters won't need the loop).
	 */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < node->num_counters; i++) {
		if (cmpxchg(&node->events[i], event, NULL) == event)
			break;
	}
	hwc->idx = -1;
}

int thunder_uncore_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore *uncore;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* we do not support sampling */
	if (is_sampling_event(event))
		return -EINVAL;

	/* counters do not have these bits */
	if (event->attr.exclude_user	||
	    event->attr.exclude_kernel	||
	    event->attr.exclude_host	||
	    event->attr.exclude_guest	||
	    event->attr.exclude_hv	||
	    event->attr.exclude_idle)
		return -EINVAL;

	/* counters are 64 bit wide and without overflow interrupts */

	uncore = event_to_thunder_uncore(event);
	if (!uncore)
		return -ENODEV;
	if (!uncore->event_valid(event->attr.config & UNCORE_EVENT_ID_MASK))
		return -EINVAL;

	/* check NUMA node */
	node = get_node(event->attr.config, uncore);
	if (!node) {
		pr_debug("Invalid numa node selected\n");
		return -EINVAL;
	}

	hwc->config = event->attr.config;
	hwc->idx = -1;
	return 0;
}

/*
 * Thunder uncore events are independent from CPUs. Provide a cpumask
 * nevertheless to prevent perf from adding the event per-cpu and just
 * set the mask to one online CPU. Use the same cpumask for all uncore
 * devices.
 */
static cpumask_t thunder_active_mask;

static ssize_t thunder_uncore_attr_show_cpumask(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	return cpumap_print_to_pagebuf(true, buf, &thunder_active_mask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, thunder_uncore_attr_show_cpumask, NULL);

static struct attribute *thunder_uncore_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

struct attribute_group thunder_uncore_attr_group = {
	.attrs = thunder_uncore_attrs,
};

ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page)
{
	struct perf_pmu_events_attr *pmu_attr =
		container_of(attr, struct perf_pmu_events_attr, attr);

	if (pmu_attr->event_str)
		return sprintf(page, "%s", pmu_attr->event_str);

	return 0;
}

/* node attribute depending on number of numa nodes */
static ssize_t node_show(struct device *dev, struct device_attribute *attr, char *page)
{
	if (NODES_SHIFT)
		return sprintf(page, "config:16-%d\n", 16 + NODES_SHIFT - 1);
	else
		return sprintf(page, "config:16\n");
}

struct device_attribute format_attr_node = __ATTR_RO(node);

static int thunder_uncore_pmu_cpu_notifier(struct notifier_block *nb,
					   unsigned long action, void *data)
{
	struct thunder_uncore *uncore = container_of(nb, struct thunder_uncore, cpu_nb);
	int new_cpu, old_cpu = (long) data;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DOWN_PREPARE:
		if (!cpumask_test_and_clear_cpu(old_cpu, &thunder_active_mask))
			break;
		new_cpu = cpumask_any_but(cpu_online_mask, old_cpu);
		if (new_cpu >= nr_cpu_ids)
			break;
		perf_pmu_migrate_context(uncore->pmu, old_cpu, new_cpu);
		cpumask_set_cpu(new_cpu, &thunder_active_mask);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct thunder_uncore_node *alloc_node(struct thunder_uncore *uncore, int node_id, int counters)
{
	struct thunder_uncore_node *node;

	node = kzalloc(sizeof(struct thunder_uncore_node), GFP_KERNEL);
	if (!node)
		return NULL;
	node->num_counters = counters;
	INIT_LIST_HEAD(&node->unit_list);
	return node;
}

int __init thunder_uncore_setup(struct thunder_uncore *uncore, int device_id,
			 unsigned long offset, unsigned long size,
			 struct pmu *pmu, int counters)
{
	struct thunder_uncore_unit  *unit, *tmp;
	struct thunder_uncore_node *node;
	struct pci_dev *pdev = NULL;
	int ret, node_id, found = 0;

	/* detect PCI devices */
	do {
		pdev = pci_get_device(PCI_VENDOR_ID_CAVIUM, device_id, pdev);
		if (!pdev)
			break;

		node_id = dev_to_node(&pdev->dev);
		/*
		 * -1 without NUMA, set to 0 because we always have at
		 *  least node 0.
		 */
		if (node_id < 0)
			node_id = 0;

		/* allocate node if necessary */
		if (!uncore->nodes[node_id])
			uncore->nodes[node_id] = alloc_node(uncore, node_id, counters);

		node = uncore->nodes[node_id];
		if (!node) {
			ret = -ENOMEM;
			goto fail;
		}

		unit = kzalloc(sizeof(struct thunder_uncore_unit), GFP_KERNEL);
		if (!unit) {
			ret = -ENOMEM;
			goto fail;
		}

		unit->pdev = pdev;
		unit->map = ioremap(pci_resource_start(pdev, 0) + offset, size);
		list_add(&unit->entry, &node->unit_list);
		node->nr_units++;
		found++;
	} while (1);

	if (!found)
		return -ENODEV;

	/*
	 * perf PMU is CPU dependent in difference to our uncore devices.
	 * Just pick a CPU and migrate away if it goes offline.
	 */
	cpumask_set_cpu(smp_processor_id(), &thunder_active_mask);

	uncore->cpu_nb.notifier_call = thunder_uncore_pmu_cpu_notifier;
	uncore->cpu_nb.priority = CPU_PRI_PERF + 1;
	ret = register_cpu_notifier(&uncore->cpu_nb);
	if (ret)
		goto fail;

	ret = perf_pmu_register(pmu, pmu->name, -1);
	if (ret)
		goto fail_pmu;

	uncore->pmu = pmu;
	return 0;

fail_pmu:
	unregister_cpu_notifier(&uncore->cpu_nb);
fail:
	node_id = 0;
	while (uncore->nodes[node_id]) {
		node = uncore->nodes[node_id];

		list_for_each_entry_safe(unit, tmp, &node->unit_list, entry) {
			if (unit->pdev) {
				if (unit->map)
					iounmap(unit->map);
				pci_dev_put(unit->pdev);
			}
			kfree(unit);
		}
		kfree(uncore->nodes[node_id]);
		node_id++;
	}
	return ret;
}

static int __init thunder_uncore_init(void)
{
	unsigned long implementor = read_cpuid_implementor();
	unsigned long part_number = read_cpuid_part_number();
	u32 variant;

	if (implementor != ARM_CPU_IMP_CAVIUM ||
	    part_number != CAVIUM_CPU_PART_THUNDERX)
		return -ENODEV;

	/* detect pass2 which contains different counters */
	variant = MIDR_VARIANT(read_cpuid_id());
	if (variant == 1)
		thunder_uncore_version = 1;
	pr_info("PMU version: %d\n", thunder_uncore_version);

	thunder_uncore_l2c_tad_setup();
	thunder_uncore_l2c_cbc_setup();
	thunder_uncore_lmc_setup();
	thunder_uncore_ocx_tlk_setup();
	return 0;
}
late_initcall(thunder_uncore_init);
