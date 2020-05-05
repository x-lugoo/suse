/*
 * Cavium Thunder uncore PMU support, LMC counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/slab.h>
#include <linux/perf_event.h>

#include "uncore_cavium.h"

#ifndef PCI_DEVICE_ID_THUNDER_LMC
#define PCI_DEVICE_ID_THUNDER_LMC	0xa022
#endif

#define LMC_NR_COUNTERS			3
#define LMC_PASS2_NR_COUNTERS		5
#define LMC_MAX_NR_COUNTERS		LMC_PASS2_NR_COUNTERS

/* LMC event list */
#define LMC_EVENT_IFB_CNT		0
#define LMC_EVENT_OPS_CNT		1
#define LMC_EVENT_DCLK_CNT		2

/* pass 2 added counters */
#define LMC_EVENT_BANK_CONFLICT1	3
#define LMC_EVENT_BANK_CONFLICT2	4

#define LMC_COUNTER_START		LMC_EVENT_IFB_CNT
#define LMC_COUNTER_END			(LMC_EVENT_BANK_CONFLICT2 + 8)

struct thunder_uncore *thunder_uncore_lmc;

int lmc_events[LMC_MAX_NR_COUNTERS] = { 0x1d0, 0x1d8, 0x1e0, 0x360, 0x368 };

static void thunder_uncore_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	hwc->state = 0;
	perf_event_update_userpage(event);
}

static void thunder_uncore_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunder_uncore_read(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int thunder_uncore_add(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int id, i;

	WARN_ON_ONCE(!uncore);
	node = get_node(hwc->config, uncore);
	id = get_id(hwc->config);

	/* are we already assigned? */
	if (hwc->idx != -1 && node->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < node->num_counters; i++) {
		if (node->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	/* these counters are self-sustained so idx must match the counter! */
	hwc->idx = -1;
	if (cmpxchg(&node->events[id], NULL, event) == NULL)
		hwc->idx = i;

out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->event_base = lmc_events[id];
	hwc->state = PERF_HES_UPTODATE;

	/* counters are read-only, so avoid PERF_EF_RELOAD */
	if (flags & PERF_EF_START)
		thunder_uncore_start(event, 0);

	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-2");

static struct attribute *thunder_lmc_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_lmc_format_group = {
	.name = "format",
	.attrs = thunder_lmc_format_attr,
};

EVENT_ATTR(ifb_cnt,		LMC_EVENT_IFB_CNT);
EVENT_ATTR(ops_cnt,		LMC_EVENT_OPS_CNT);
EVENT_ATTR(dclk_cnt,		LMC_EVENT_DCLK_CNT);
EVENT_ATTR(bank_conflict1,	LMC_EVENT_BANK_CONFLICT1);
EVENT_ATTR(bank_conflict2,	LMC_EVENT_BANK_CONFLICT2);

static struct attribute *thunder_lmc_events_attr[] = {
	EVENT_PTR(ifb_cnt),
	EVENT_PTR(ops_cnt),
	EVENT_PTR(dclk_cnt),
	NULL,
};

static struct attribute *thunder_lmc_pass2_events_attr[] = {
	EVENT_PTR(ifb_cnt),
	EVENT_PTR(ops_cnt),
	EVENT_PTR(dclk_cnt),
	EVENT_PTR(bank_conflict1),
	EVENT_PTR(bank_conflict2),
	NULL,
};

static struct attribute_group thunder_lmc_events_group = {
	.name = "events",
	.attrs = NULL,
};

static const struct attribute_group *thunder_lmc_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_lmc_format_group,
	&thunder_lmc_events_group,
	NULL,
};

struct pmu thunder_lmc_pmu = {
	.attr_groups	= thunder_lmc_attr_groups,
	.name		= "thunder_lmc",
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read,
};

static int event_valid(u64 config)
{
	if (config <= LMC_EVENT_DCLK_CNT)
		return 1;

	if (thunder_uncore_version == 1)
		if (config == LMC_EVENT_BANK_CONFLICT1 ||
		    config == LMC_EVENT_BANK_CONFLICT2)
			return 1;
	return 0;
}

int __init thunder_uncore_lmc_setup(void)
{
	int ret = -ENOMEM;

	thunder_uncore_lmc = kzalloc(sizeof(struct thunder_uncore), GFP_KERNEL);
	if (!thunder_uncore_lmc)
		goto fail_nomem;

	/* pass2 is default */
	thunder_lmc_events_group.attrs = (thunder_uncore_version == 0) ?
		thunder_lmc_events_attr : thunder_lmc_pass2_events_attr;

	ret = thunder_uncore_setup(thunder_uncore_lmc,
				   PCI_DEVICE_ID_THUNDER_LMC,
				   LMC_COUNTER_START,
				   LMC_COUNTER_END - LMC_COUNTER_START,
				   &thunder_lmc_pmu,
				   (thunder_uncore_version == 1) ?
					LMC_PASS2_NR_COUNTERS : LMC_NR_COUNTERS);
	if (ret)
		goto fail;

	thunder_uncore_lmc->type = LMC_TYPE;
	thunder_uncore_lmc->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_lmc);
fail_nomem:
	return ret;
}
