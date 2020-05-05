/*
 * Cavium Thunder uncore PMU support, OCX TLK counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/slab.h>
#include <linux/perf_event.h>

#include "uncore_cavium.h"

#ifndef PCI_DEVICE_ID_THUNDER_OCX
#define PCI_DEVICE_ID_THUNDER_OCX		0xa013
#endif

#define OCX_TLK_NR_UNITS			3
#define OCX_TLK_UNIT_OFFSET			0x2000
#define OCX_TLK_CONTROL_OFFSET			0x10040
#define OCX_TLK_COUNTER_OFFSET			0x10400

#define OCX_TLK_STAT_DISABLE			0
#define OCX_TLK_STAT_ENABLE			1

/* OCX TLK event list */
#define OCX_TLK_EVENT_STAT_IDLE_CNT		0x00
#define OCX_TLK_EVENT_STAT_DATA_CNT		0x01
#define OCX_TLK_EVENT_STAT_SYNC_CNT		0x02
#define OCX_TLK_EVENT_STAT_RETRY_CNT		0x03
#define OCX_TLK_EVENT_STAT_ERR_CNT		0x04

#define OCX_TLK_EVENT_STAT_MAT0_CNT		0x08
#define OCX_TLK_EVENT_STAT_MAT1_CNT		0x09
#define OCX_TLK_EVENT_STAT_MAT2_CNT		0x0a
#define OCX_TLK_EVENT_STAT_MAT3_CNT		0x0b

#define OCX_TLK_EVENT_STAT_VC0_CMD		0x10
#define OCX_TLK_EVENT_STAT_VC1_CMD		0x11
#define OCX_TLK_EVENT_STAT_VC2_CMD		0x12
#define OCX_TLK_EVENT_STAT_VC3_CMD		0x13
#define OCX_TLK_EVENT_STAT_VC4_CMD		0x14
#define OCX_TLK_EVENT_STAT_VC5_CMD		0x15

#define OCX_TLK_EVENT_STAT_VC0_PKT		0x20
#define OCX_TLK_EVENT_STAT_VC1_PKT		0x21
#define OCX_TLK_EVENT_STAT_VC2_PKT		0x22
#define OCX_TLK_EVENT_STAT_VC3_PKT		0x23
#define OCX_TLK_EVENT_STAT_VC4_PKT		0x24
#define OCX_TLK_EVENT_STAT_VC5_PKT		0x25
#define OCX_TLK_EVENT_STAT_VC6_PKT		0x26
#define OCX_TLK_EVENT_STAT_VC7_PKT		0x27
#define OCX_TLK_EVENT_STAT_VC8_PKT		0x28
#define OCX_TLK_EVENT_STAT_VC9_PKT		0x29
#define OCX_TLK_EVENT_STAT_VC10_PKT		0x2a
#define OCX_TLK_EVENT_STAT_VC11_PKT		0x2b
#define OCX_TLK_EVENT_STAT_VC12_PKT		0x2c
#define OCX_TLK_EVENT_STAT_VC13_PKT		0x2d

#define OCX_TLK_EVENT_STAT_VC0_CON		0x30
#define OCX_TLK_EVENT_STAT_VC1_CON		0x31
#define OCX_TLK_EVENT_STAT_VC2_CON		0x32
#define OCX_TLK_EVENT_STAT_VC3_CON		0x33
#define OCX_TLK_EVENT_STAT_VC4_CON		0x34
#define OCX_TLK_EVENT_STAT_VC5_CON		0x35
#define OCX_TLK_EVENT_STAT_VC6_CON		0x36
#define OCX_TLK_EVENT_STAT_VC7_CON		0x37
#define OCX_TLK_EVENT_STAT_VC8_CON		0x38
#define OCX_TLK_EVENT_STAT_VC9_CON		0x39
#define OCX_TLK_EVENT_STAT_VC10_CON		0x3a
#define OCX_TLK_EVENT_STAT_VC11_CON		0x3b
#define OCX_TLK_EVENT_STAT_VC12_CON		0x3c
#define OCX_TLK_EVENT_STAT_VC13_CON		0x3d

#define OCX_TLK_MAX_COUNTER			OCX_TLK_EVENT_STAT_VC13_CON
#define OCX_TLK_NR_COUNTERS			OCX_TLK_MAX_COUNTER

struct thunder_uncore *thunder_uncore_ocx_tlk;

/*
 * The OCX devices have a single device per node, therefore picking the
 * first device from the list is correct.
 */
static inline void __iomem *map_offset(struct thunder_uncore_node *node,
				       unsigned long addr, int offset, int nr)
{
	struct thunder_uncore_unit *unit;

	unit = list_first_entry(&node->unit_list, struct thunder_uncore_unit,
				entry);
	return (void __iomem *) (addr + unit->map + nr * offset);
}

static void __iomem *map_offset_ocx_tlk(struct thunder_uncore_node *node,
					unsigned long addr, int nr)
{
	return (void __iomem *) map_offset(node, addr, nr,
					   OCX_TLK_UNIT_OFFSET);
}

/*
 * Summarize counters across all TLK's. Different from the other uncore
 * PMUs because all TLK's are on one PCI device.
 */
static void thunder_uncore_read_ocx_tlk(struct perf_event *event)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	u64 prev, new = 0;
	s64 delta;
	int i;

	/*
	 * No counter overflow interrupts so we do not
	 * have to worry about prev_count changing on us.
	 */

	prev = local64_read(&hwc->prev_count);

	/* read counter values from all units */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < OCX_TLK_NR_UNITS; i++)
		new += readq(map_offset_ocx_tlk(node, hwc->event_base, i));

	local64_set(&hwc->prev_count, new);
	delta = new - prev;
	local64_add(delta, &event->count);
}

static void thunder_uncore_start(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int i;

	hwc->state = 0;

	/* enable counters on all units */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < OCX_TLK_NR_UNITS; i++)
		writeb(OCX_TLK_STAT_ENABLE,
		       map_offset_ocx_tlk(node, hwc->config_base, i));

	perf_event_update_userpage(event);
}

static void thunder_uncore_stop(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	int i;

	/* disable counters on all units */
	node = get_node(hwc->config, uncore);
	for (i = 0; i < OCX_TLK_NR_UNITS; i++)
		writeb(OCX_TLK_STAT_DISABLE,
		       map_offset_ocx_tlk(node, hwc->config_base, i));
	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunder_uncore_read_ocx_tlk(event);
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

	/* counters are 1:1 */
	hwc->idx = -1;
	if (cmpxchg(&node->events[id], NULL, event) == NULL)
		hwc->idx = id;

out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = 0;
	hwc->event_base = OCX_TLK_COUNTER_OFFSET - OCX_TLK_CONTROL_OFFSET +
			hwc->idx * sizeof(unsigned long long);
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		thunder_uncore_start(event, PERF_EF_RELOAD);
	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-5");

static struct attribute *thunder_ocx_tlk_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_ocx_tlk_format_group = {
	.name = "format",
	.attrs = thunder_ocx_tlk_format_attr,
};

EVENT_ATTR(idle_cnt,	OCX_TLK_EVENT_STAT_IDLE_CNT);
EVENT_ATTR(data_cnt,	OCX_TLK_EVENT_STAT_DATA_CNT);
EVENT_ATTR(sync_cnt,	OCX_TLK_EVENT_STAT_SYNC_CNT);
EVENT_ATTR(retry_cnt,	OCX_TLK_EVENT_STAT_RETRY_CNT);
EVENT_ATTR(err_cnt,	OCX_TLK_EVENT_STAT_ERR_CNT);
EVENT_ATTR(mat0_cnt,	OCX_TLK_EVENT_STAT_MAT0_CNT);
EVENT_ATTR(mat1_cnt,	OCX_TLK_EVENT_STAT_MAT1_CNT);
EVENT_ATTR(mat2_cnt,	OCX_TLK_EVENT_STAT_MAT2_CNT);
EVENT_ATTR(mat3_cnt,	OCX_TLK_EVENT_STAT_MAT3_CNT);
EVENT_ATTR(vc0_cmd,	OCX_TLK_EVENT_STAT_VC0_CMD);
EVENT_ATTR(vc1_cmd,	OCX_TLK_EVENT_STAT_VC1_CMD);
EVENT_ATTR(vc2_cmd,	OCX_TLK_EVENT_STAT_VC2_CMD);
EVENT_ATTR(vc3_cmd,	OCX_TLK_EVENT_STAT_VC3_CMD);
EVENT_ATTR(vc4_cmd,	OCX_TLK_EVENT_STAT_VC4_CMD);
EVENT_ATTR(vc5_cmd,	OCX_TLK_EVENT_STAT_VC5_CMD);
EVENT_ATTR(vc0_pkt,	OCX_TLK_EVENT_STAT_VC0_PKT);
EVENT_ATTR(vc1_pkt,	OCX_TLK_EVENT_STAT_VC1_PKT);
EVENT_ATTR(vc2_pkt,	OCX_TLK_EVENT_STAT_VC2_PKT);
EVENT_ATTR(vc3_pkt,	OCX_TLK_EVENT_STAT_VC3_PKT);
EVENT_ATTR(vc4_pkt,	OCX_TLK_EVENT_STAT_VC4_PKT);
EVENT_ATTR(vc5_pkt,	OCX_TLK_EVENT_STAT_VC5_PKT);
EVENT_ATTR(vc6_pkt,	OCX_TLK_EVENT_STAT_VC6_PKT);
EVENT_ATTR(vc7_pkt,	OCX_TLK_EVENT_STAT_VC7_PKT);
EVENT_ATTR(vc8_pkt,	OCX_TLK_EVENT_STAT_VC8_PKT);
EVENT_ATTR(vc9_pkt,	OCX_TLK_EVENT_STAT_VC9_PKT);
EVENT_ATTR(vc10_pkt,	OCX_TLK_EVENT_STAT_VC10_PKT);
EVENT_ATTR(vc11_pkt,	OCX_TLK_EVENT_STAT_VC11_PKT);
EVENT_ATTR(vc12_pkt,	OCX_TLK_EVENT_STAT_VC12_PKT);
EVENT_ATTR(vc13_pkt,	OCX_TLK_EVENT_STAT_VC13_PKT);
EVENT_ATTR(vc0_con,	OCX_TLK_EVENT_STAT_VC0_CON);
EVENT_ATTR(vc1_con,	OCX_TLK_EVENT_STAT_VC1_CON);
EVENT_ATTR(vc2_con,	OCX_TLK_EVENT_STAT_VC2_CON);
EVENT_ATTR(vc3_con,	OCX_TLK_EVENT_STAT_VC3_CON);
EVENT_ATTR(vc4_con,	OCX_TLK_EVENT_STAT_VC4_CON);
EVENT_ATTR(vc5_con,	OCX_TLK_EVENT_STAT_VC5_CON);
EVENT_ATTR(vc6_con,	OCX_TLK_EVENT_STAT_VC6_CON);
EVENT_ATTR(vc7_con,	OCX_TLK_EVENT_STAT_VC7_CON);
EVENT_ATTR(vc8_con,	OCX_TLK_EVENT_STAT_VC8_CON);
EVENT_ATTR(vc9_con,	OCX_TLK_EVENT_STAT_VC9_CON);
EVENT_ATTR(vc10_con,	OCX_TLK_EVENT_STAT_VC10_CON);
EVENT_ATTR(vc11_con,	OCX_TLK_EVENT_STAT_VC11_CON);
EVENT_ATTR(vc12_con,	OCX_TLK_EVENT_STAT_VC12_CON);
EVENT_ATTR(vc13_con,	OCX_TLK_EVENT_STAT_VC13_CON);

static struct attribute *thunder_ocx_tlk_events_attr[] = {
	EVENT_PTR(idle_cnt),
	EVENT_PTR(data_cnt),
	EVENT_PTR(sync_cnt),
	EVENT_PTR(retry_cnt),
	EVENT_PTR(err_cnt),
	EVENT_PTR(mat0_cnt),
	EVENT_PTR(mat1_cnt),
	EVENT_PTR(mat2_cnt),
	EVENT_PTR(mat3_cnt),
	EVENT_PTR(vc0_cmd),
	EVENT_PTR(vc1_cmd),
	EVENT_PTR(vc2_cmd),
	EVENT_PTR(vc3_cmd),
	EVENT_PTR(vc4_cmd),
	EVENT_PTR(vc5_cmd),
	EVENT_PTR(vc0_pkt),
	EVENT_PTR(vc1_pkt),
	EVENT_PTR(vc2_pkt),
	EVENT_PTR(vc3_pkt),
	EVENT_PTR(vc4_pkt),
	EVENT_PTR(vc5_pkt),
	EVENT_PTR(vc6_pkt),
	EVENT_PTR(vc7_pkt),
	EVENT_PTR(vc8_pkt),
	EVENT_PTR(vc9_pkt),
	EVENT_PTR(vc10_pkt),
	EVENT_PTR(vc11_pkt),
	EVENT_PTR(vc12_pkt),
	EVENT_PTR(vc13_pkt),
	EVENT_PTR(vc0_con),
	EVENT_PTR(vc1_con),
	EVENT_PTR(vc2_con),
	EVENT_PTR(vc3_con),
	EVENT_PTR(vc4_con),
	EVENT_PTR(vc5_con),
	EVENT_PTR(vc6_con),
	EVENT_PTR(vc7_con),
	EVENT_PTR(vc8_con),
	EVENT_PTR(vc9_con),
	EVENT_PTR(vc10_con),
	EVENT_PTR(vc11_con),
	EVENT_PTR(vc12_con),
	EVENT_PTR(vc13_con),
	NULL,
};

static struct attribute_group thunder_ocx_tlk_events_group = {
	.name = "events",
	.attrs = thunder_ocx_tlk_events_attr,
};

static const struct attribute_group *thunder_ocx_tlk_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_ocx_tlk_format_group,
	&thunder_ocx_tlk_events_group,
	NULL,
};

struct pmu thunder_ocx_tlk_pmu = {
	.attr_groups	= thunder_ocx_tlk_attr_groups,
	.name		= "thunder_ocx_tlk",
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read_ocx_tlk,
};

static int event_valid(u64 config)
{
	if (config <= OCX_TLK_EVENT_STAT_ERR_CNT ||
	    (config >= OCX_TLK_EVENT_STAT_MAT0_CNT &&
	     config <= OCX_TLK_EVENT_STAT_MAT3_CNT) ||
	    (config >= OCX_TLK_EVENT_STAT_VC0_CMD &&
	     config <= OCX_TLK_EVENT_STAT_VC5_CMD) ||
	    (config >= OCX_TLK_EVENT_STAT_VC0_PKT &&
	     config <= OCX_TLK_EVENT_STAT_VC13_PKT) ||
	    (config >= OCX_TLK_EVENT_STAT_VC0_CON &&
	     config <= OCX_TLK_EVENT_STAT_VC13_CON))
		return 1;
	else
		return 0;
}

int __init thunder_uncore_ocx_tlk_setup(void)
{
	int ret;

	thunder_uncore_ocx_tlk = kzalloc(sizeof(struct thunder_uncore),
					 GFP_KERNEL);
	if (!thunder_uncore_ocx_tlk) {
		ret = -ENOMEM;
		goto fail_nomem;
	}

	ret = thunder_uncore_setup(thunder_uncore_ocx_tlk,
				   PCI_DEVICE_ID_THUNDER_OCX,
				   OCX_TLK_CONTROL_OFFSET,
				   OCX_TLK_UNIT_OFFSET * OCX_TLK_NR_UNITS,
				   &thunder_ocx_tlk_pmu,
				   OCX_TLK_NR_COUNTERS);
	if (ret)
		goto fail;

	thunder_uncore_ocx_tlk->type = OCX_TLK_TYPE;
	thunder_uncore_ocx_tlk->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_ocx_tlk);
fail_nomem:
	return ret;
}
