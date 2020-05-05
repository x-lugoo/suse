/*
 * Cavium Thunder uncore PMU support, L2C TAD counters.
 *
 * Copyright 2016 Cavium Inc.
 * Author: Jan Glauber <jan.glauber@cavium.com>
 */

#include <linux/slab.h>
#include <linux/perf_event.h>

#include "uncore_cavium.h"

#ifndef PCI_DEVICE_ID_THUNDER_L2C_TAD
#define PCI_DEVICE_ID_THUNDER_L2C_TAD	0xa02e
#endif

#define L2C_TAD_NR_COUNTERS             4
#define L2C_TAD_CONTROL_OFFSET		0x10000
#define L2C_TAD_COUNTER_OFFSET		0x100

/* L2C TAD event list */
#define L2C_TAD_EVENTS_DISABLED		0x00

#define L2C_TAD_EVENT_L2T_HIT		0x01
#define L2C_TAD_EVENT_L2T_MISS		0x02
#define L2C_TAD_EVENT_L2T_NOALLOC	0x03
#define L2C_TAD_EVENT_L2_VIC		0x04
#define L2C_TAD_EVENT_SC_FAIL		0x05
#define L2C_TAD_EVENT_SC_PASS		0x06
#define L2C_TAD_EVENT_LFB_OCC		0x07
#define L2C_TAD_EVENT_WAIT_LFB		0x08
#define L2C_TAD_EVENT_WAIT_VAB		0x09

#define L2C_TAD_EVENT_RTG_HIT		0x41
#define L2C_TAD_EVENT_RTG_MISS		0x42
#define L2C_TAD_EVENT_L2_RTG_VIC	0x44
#define L2C_TAD_EVENT_L2_OPEN_OCI	0x48

#define L2C_TAD_EVENT_QD0_IDX		0x80
#define L2C_TAD_EVENT_QD0_RDAT		0x81
#define L2C_TAD_EVENT_QD0_BNKS		0x82
#define L2C_TAD_EVENT_QD0_WDAT		0x83

#define L2C_TAD_EVENT_QD1_IDX		0x90
#define L2C_TAD_EVENT_QD1_RDAT		0x91
#define L2C_TAD_EVENT_QD1_BNKS		0x92
#define L2C_TAD_EVENT_QD1_WDAT		0x93

#define L2C_TAD_EVENT_QD2_IDX		0xa0
#define L2C_TAD_EVENT_QD2_RDAT		0xa1
#define L2C_TAD_EVENT_QD2_BNKS		0xa2
#define L2C_TAD_EVENT_QD2_WDAT		0xa3

#define L2C_TAD_EVENT_QD3_IDX		0xb0
#define L2C_TAD_EVENT_QD3_RDAT		0xb1
#define L2C_TAD_EVENT_QD3_BNKS		0xb2
#define L2C_TAD_EVENT_QD3_WDAT		0xb3

#define L2C_TAD_EVENT_QD4_IDX		0xc0
#define L2C_TAD_EVENT_QD4_RDAT		0xc1
#define L2C_TAD_EVENT_QD4_BNKS		0xc2
#define L2C_TAD_EVENT_QD4_WDAT		0xc3

#define L2C_TAD_EVENT_QD5_IDX		0xd0
#define L2C_TAD_EVENT_QD5_RDAT		0xd1
#define L2C_TAD_EVENT_QD5_BNKS		0xd2
#define L2C_TAD_EVENT_QD5_WDAT		0xd3

#define L2C_TAD_EVENT_QD6_IDX		0xe0
#define L2C_TAD_EVENT_QD6_RDAT		0xe1
#define L2C_TAD_EVENT_QD6_BNKS		0xe2
#define L2C_TAD_EVENT_QD6_WDAT		0xe3

#define L2C_TAD_EVENT_QD7_IDX		0xf0
#define L2C_TAD_EVENT_QD7_RDAT		0xf1
#define L2C_TAD_EVENT_QD7_BNKS		0xf2
#define L2C_TAD_EVENT_QD7_WDAT		0xf3

/* pass2 added/changed event list */
#define L2C_TAD_EVENT_OPEN_CCPI			0x0a
#define L2C_TAD_EVENT_LOOKUP			0x40
#define L2C_TAD_EVENT_LOOKUP_XMC_LCL		0x41
#define L2C_TAD_EVENT_LOOKUP_XMC_RMT		0x42
#define L2C_TAD_EVENT_LOOKUP_MIB		0x43
#define L2C_TAD_EVENT_LOOKUP_ALL		0x44
#define L2C_TAD_EVENT_TAG_ALC_HIT		0x48
#define L2C_TAD_EVENT_TAG_ALC_MISS		0x49
#define L2C_TAD_EVENT_TAG_ALC_NALC		0x4a
#define L2C_TAD_EVENT_TAG_NALC_HIT		0x4b
#define L2C_TAD_EVENT_TAG_NALC_MISS		0x4c
#define L2C_TAD_EVENT_LMC_WR			0x4e
#define L2C_TAD_EVENT_LMC_SBLKDTY		0x4f
#define L2C_TAD_EVENT_TAG_ALC_RTG_HIT		0x50
#define L2C_TAD_EVENT_TAG_ALC_RTG_HITE		0x51
#define L2C_TAD_EVENT_TAG_ALC_RTG_HITS		0x52
#define L2C_TAD_EVENT_TAG_ALC_RTG_MISS		0x53
#define L2C_TAD_EVENT_TAG_NALC_RTG_HIT		0x54
#define L2C_TAD_EVENT_TAG_NALC_RTG_MISS		0x55
#define L2C_TAD_EVENT_TAG_NALC_RTG_HITE		0x56
#define L2C_TAD_EVENT_TAG_NALC_RTG_HITS		0x57
#define L2C_TAD_EVENT_TAG_ALC_LCL_EVICT		0x58
#define L2C_TAD_EVENT_TAG_ALC_LCL_CLNVIC	0x59
#define L2C_TAD_EVENT_TAG_ALC_LCL_DTYVIC	0x5a
#define L2C_TAD_EVENT_TAG_ALC_RMT_EVICT		0x5b
#define L2C_TAD_EVENT_TAG_ALC_RMT_VIC		0x5c
#define L2C_TAD_EVENT_RTG_ALC			0x5d
#define L2C_TAD_EVENT_RTG_ALC_HIT		0x5e
#define L2C_TAD_EVENT_RTG_ALC_HITWB		0x5f
#define L2C_TAD_EVENT_STC_TOTAL			0x60
#define L2C_TAD_EVENT_STC_TOTAL_FAIL		0x61
#define L2C_TAD_EVENT_STC_RMT			0x62
#define L2C_TAD_EVENT_STC_RMT_FAIL		0x63
#define L2C_TAD_EVENT_STC_LCL			0x64
#define L2C_TAD_EVENT_STC_LCL_FAIL		0x65
#define L2C_TAD_EVENT_OCI_RTG_WAIT		0x68
#define L2C_TAD_EVENT_OCI_FWD_CYC_HIT		0x69
#define L2C_TAD_EVENT_OCI_FWD_RACE		0x6a
#define L2C_TAD_EVENT_OCI_HAKS			0x6b
#define L2C_TAD_EVENT_OCI_FLDX_TAG_E_NODAT	0x6c
#define L2C_TAD_EVENT_OCI_FLDX_TAG_E_DAT	0x6d
#define L2C_TAD_EVENT_OCI_RLDD			0x6e
#define L2C_TAD_EVENT_OCI_RLDD_PEMD		0x6f
#define L2C_TAD_EVENT_OCI_RRQ_DAT_CNT		0x70
#define L2C_TAD_EVENT_OCI_RRQ_DAT_DMASK		0x71
#define L2C_TAD_EVENT_OCI_RSP_DAT_CNT		0x72
#define L2C_TAD_EVENT_OCI_RSP_DAT_DMASK		0x73
#define L2C_TAD_EVENT_OCI_RSP_DAT_VICD_CNT	0x74
#define L2C_TAD_EVENT_OCI_RSP_DAT_VICD_DMASK	0x75
#define L2C_TAD_EVENT_OCI_RTG_ALC_EVICT		0x76
#define L2C_TAD_EVENT_OCI_RTG_ALC_VIC		0x77

struct thunder_uncore *thunder_uncore_l2c_tad;

static void thunder_uncore_start(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;
	u64 prev;
	int id;

	node = get_node(hwc->config, uncore);
	id = get_id(hwc->config);

	/* restore counter value divided by units into all counters */
	if (flags & PERF_EF_RELOAD) {
		prev = local64_read(&hwc->prev_count);
		prev = prev / node->nr_units;

		list_for_each_entry(unit, &node->unit_list, entry)
			writeq(prev, hwc->event_base + unit->map);
	}

	hwc->state = 0;

	/* write byte in control registers for all units on the node */
	list_for_each_entry(unit, &node->unit_list, entry)
		writeb(id, hwc->config_base + unit->map);

	perf_event_update_userpage(event);
}

static void thunder_uncore_stop(struct perf_event *event, int flags)
{
	struct thunder_uncore *uncore = event_to_thunder_uncore(event);
	struct hw_perf_event *hwc = &event->hw;
	struct thunder_uncore_node *node;
	struct thunder_uncore_unit *unit;

	/* reset selection value for all units on the node */
	node = get_node(hwc->config, uncore);

	list_for_each_entry(unit, &node->unit_list, entry)
		writeb(L2C_TAD_EVENTS_DISABLED, hwc->config_base + unit->map);
	hwc->state |= PERF_HES_STOPPED;

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
	int i;

	WARN_ON_ONCE(!uncore);
	node = get_node(hwc->config, uncore);

	/* are we already assigned? */
	if (hwc->idx != -1 && node->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < node->num_counters; i++) {
		if (node->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	/* if not take the first available counter */
	hwc->idx = -1;
	for (i = 0; i < node->num_counters; i++) {
		if (cmpxchg(&node->events[i], NULL, event) == NULL) {
			hwc->idx = i;
			break;
		}
	}
out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = hwc->idx;
	hwc->event_base = L2C_TAD_COUNTER_OFFSET +
			  hwc->idx * sizeof(unsigned long long);
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		thunder_uncore_start(event, PERF_EF_RELOAD);
	return 0;
}

PMU_FORMAT_ATTR(event, "config:0-7");

static struct attribute *thunder_l2c_tad_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_node.attr,
	NULL,
};

static struct attribute_group thunder_l2c_tad_format_group = {
	.name = "format",
	.attrs = thunder_l2c_tad_format_attr,
};

EVENT_ATTR(l2t_hit,	L2C_TAD_EVENT_L2T_HIT);
EVENT_ATTR(l2t_miss,	L2C_TAD_EVENT_L2T_MISS);
EVENT_ATTR(l2t_noalloc,	L2C_TAD_EVENT_L2T_NOALLOC);
EVENT_ATTR(l2_vic,	L2C_TAD_EVENT_L2_VIC);
EVENT_ATTR(sc_fail,	L2C_TAD_EVENT_SC_FAIL);
EVENT_ATTR(sc_pass,	L2C_TAD_EVENT_SC_PASS);
EVENT_ATTR(lfb_occ,	L2C_TAD_EVENT_LFB_OCC);
EVENT_ATTR(wait_lfb,	L2C_TAD_EVENT_WAIT_LFB);
EVENT_ATTR(wait_vab,	L2C_TAD_EVENT_WAIT_VAB);
EVENT_ATTR(rtg_hit,	L2C_TAD_EVENT_RTG_HIT);
EVENT_ATTR(rtg_miss,	L2C_TAD_EVENT_RTG_MISS);
EVENT_ATTR(l2_rtg_vic,	L2C_TAD_EVENT_L2_RTG_VIC);
EVENT_ATTR(l2_open_oci,	L2C_TAD_EVENT_L2_OPEN_OCI);

EVENT_ATTR(qd0_idx,	L2C_TAD_EVENT_QD0_IDX);
EVENT_ATTR(qd0_rdat,	L2C_TAD_EVENT_QD0_RDAT);
EVENT_ATTR(qd0_bnks,	L2C_TAD_EVENT_QD0_BNKS);
EVENT_ATTR(qd0_wdat,	L2C_TAD_EVENT_QD0_WDAT);

EVENT_ATTR(qd1_idx,	L2C_TAD_EVENT_QD1_IDX);
EVENT_ATTR(qd1_rdat,	L2C_TAD_EVENT_QD1_RDAT);
EVENT_ATTR(qd1_bnks,	L2C_TAD_EVENT_QD1_BNKS);
EVENT_ATTR(qd1_wdat,	L2C_TAD_EVENT_QD1_WDAT);

EVENT_ATTR(qd2_idx,	L2C_TAD_EVENT_QD2_IDX);
EVENT_ATTR(qd2_rdat,	L2C_TAD_EVENT_QD2_RDAT);
EVENT_ATTR(qd2_bnks,	L2C_TAD_EVENT_QD2_BNKS);
EVENT_ATTR(qd2_wdat,	L2C_TAD_EVENT_QD2_WDAT);

EVENT_ATTR(qd3_idx,	L2C_TAD_EVENT_QD3_IDX);
EVENT_ATTR(qd3_rdat,	L2C_TAD_EVENT_QD3_RDAT);
EVENT_ATTR(qd3_bnks,	L2C_TAD_EVENT_QD3_BNKS);
EVENT_ATTR(qd3_wdat,	L2C_TAD_EVENT_QD3_WDAT);

EVENT_ATTR(qd4_idx,	L2C_TAD_EVENT_QD4_IDX);
EVENT_ATTR(qd4_rdat,	L2C_TAD_EVENT_QD4_RDAT);
EVENT_ATTR(qd4_bnks,	L2C_TAD_EVENT_QD4_BNKS);
EVENT_ATTR(qd4_wdat,	L2C_TAD_EVENT_QD4_WDAT);

EVENT_ATTR(qd5_idx,	L2C_TAD_EVENT_QD5_IDX);
EVENT_ATTR(qd5_rdat,	L2C_TAD_EVENT_QD5_RDAT);
EVENT_ATTR(qd5_bnks,	L2C_TAD_EVENT_QD5_BNKS);
EVENT_ATTR(qd5_wdat,	L2C_TAD_EVENT_QD5_WDAT);

EVENT_ATTR(qd6_idx,	L2C_TAD_EVENT_QD6_IDX);
EVENT_ATTR(qd6_rdat,	L2C_TAD_EVENT_QD6_RDAT);
EVENT_ATTR(qd6_bnks,	L2C_TAD_EVENT_QD6_BNKS);
EVENT_ATTR(qd6_wdat,	L2C_TAD_EVENT_QD6_WDAT);

EVENT_ATTR(qd7_idx,	L2C_TAD_EVENT_QD7_IDX);
EVENT_ATTR(qd7_rdat,	L2C_TAD_EVENT_QD7_RDAT);
EVENT_ATTR(qd7_bnks,	L2C_TAD_EVENT_QD7_BNKS);
EVENT_ATTR(qd7_wdat,	L2C_TAD_EVENT_QD7_WDAT);

static struct attribute *thunder_l2c_tad_events_attr[] = {
	EVENT_PTR(l2t_hit),
	EVENT_PTR(l2t_miss),
	EVENT_PTR(l2t_noalloc),
	EVENT_PTR(l2_vic),
	EVENT_PTR(sc_fail),
	EVENT_PTR(sc_pass),
	EVENT_PTR(lfb_occ),
	EVENT_PTR(wait_lfb),
	EVENT_PTR(wait_vab),
	EVENT_PTR(rtg_hit),
	EVENT_PTR(rtg_miss),
	EVENT_PTR(l2_rtg_vic),
	EVENT_PTR(l2_open_oci),

	EVENT_PTR(qd0_idx),
	EVENT_PTR(qd0_rdat),
	EVENT_PTR(qd0_bnks),
	EVENT_PTR(qd0_wdat),

	EVENT_PTR(qd1_idx),
	EVENT_PTR(qd1_rdat),
	EVENT_PTR(qd1_bnks),
	EVENT_PTR(qd1_wdat),

	EVENT_PTR(qd2_idx),
	EVENT_PTR(qd2_rdat),
	EVENT_PTR(qd2_bnks),
	EVENT_PTR(qd2_wdat),

	EVENT_PTR(qd3_idx),
	EVENT_PTR(qd3_rdat),
	EVENT_PTR(qd3_bnks),
	EVENT_PTR(qd3_wdat),

	EVENT_PTR(qd4_idx),
	EVENT_PTR(qd4_rdat),
	EVENT_PTR(qd4_bnks),
	EVENT_PTR(qd4_wdat),

	EVENT_PTR(qd5_idx),
	EVENT_PTR(qd5_rdat),
	EVENT_PTR(qd5_bnks),
	EVENT_PTR(qd5_wdat),

	EVENT_PTR(qd6_idx),
	EVENT_PTR(qd6_rdat),
	EVENT_PTR(qd6_bnks),
	EVENT_PTR(qd6_wdat),

	EVENT_PTR(qd7_idx),
	EVENT_PTR(qd7_rdat),
	EVENT_PTR(qd7_bnks),
	EVENT_PTR(qd7_wdat),
	NULL,
};

/* pass2 added/chanegd events */
EVENT_ATTR(open_ccpi,		L2C_TAD_EVENT_OPEN_CCPI);
EVENT_ATTR(lookup,		L2C_TAD_EVENT_LOOKUP);
EVENT_ATTR(lookup_xmc_lcl,	L2C_TAD_EVENT_LOOKUP_XMC_LCL);
EVENT_ATTR(lookup_xmc_rmt,	L2C_TAD_EVENT_LOOKUP_XMC_RMT);
EVENT_ATTR(lookup_mib,		L2C_TAD_EVENT_LOOKUP_MIB);
EVENT_ATTR(lookup_all,		L2C_TAD_EVENT_LOOKUP_ALL);

EVENT_ATTR(tag_alc_hit,		L2C_TAD_EVENT_TAG_ALC_HIT);
EVENT_ATTR(tag_alc_miss,	L2C_TAD_EVENT_TAG_ALC_MISS);
EVENT_ATTR(tag_alc_nalc,	L2C_TAD_EVENT_TAG_ALC_NALC);
EVENT_ATTR(tag_nalc_hit,	L2C_TAD_EVENT_TAG_NALC_HIT);
EVENT_ATTR(tag_nalc_miss,	L2C_TAD_EVENT_TAG_NALC_MISS);

EVENT_ATTR(lmc_wr,		L2C_TAD_EVENT_LMC_WR);
EVENT_ATTR(lmc_sblkdty,		L2C_TAD_EVENT_LMC_SBLKDTY);

EVENT_ATTR(tag_alc_rtg_hit,	L2C_TAD_EVENT_TAG_ALC_RTG_HIT);
EVENT_ATTR(tag_alc_rtg_hite,	L2C_TAD_EVENT_TAG_ALC_RTG_HITE);
EVENT_ATTR(tag_alc_rtg_hits,	L2C_TAD_EVENT_TAG_ALC_RTG_HITS);
EVENT_ATTR(tag_alc_rtg_miss,	L2C_TAD_EVENT_TAG_ALC_RTG_MISS);
EVENT_ATTR(tag_alc_nalc_rtg_hit, L2C_TAD_EVENT_TAG_NALC_RTG_HIT);
EVENT_ATTR(tag_nalc_rtg_miss,	L2C_TAD_EVENT_TAG_NALC_RTG_MISS);
EVENT_ATTR(tag_nalc_rtg_hite,	L2C_TAD_EVENT_TAG_NALC_RTG_HITE);
EVENT_ATTR(tag_nalc_rtg_hits,	L2C_TAD_EVENT_TAG_NALC_RTG_HITS);
EVENT_ATTR(tag_alc_lcl_evict,	L2C_TAD_EVENT_TAG_ALC_LCL_EVICT);
EVENT_ATTR(tag_alc_lcl_clnvic,	L2C_TAD_EVENT_TAG_ALC_LCL_CLNVIC);
EVENT_ATTR(tag_alc_lcl_dtyvic,	L2C_TAD_EVENT_TAG_ALC_LCL_DTYVIC);
EVENT_ATTR(tag_alc_rmt_evict,	L2C_TAD_EVENT_TAG_ALC_RMT_EVICT);
EVENT_ATTR(tag_alc_rmt_vic,	L2C_TAD_EVENT_TAG_ALC_RMT_VIC);

EVENT_ATTR(rtg_alc,		L2C_TAD_EVENT_RTG_ALC);
EVENT_ATTR(rtg_alc_hit,		L2C_TAD_EVENT_RTG_ALC_HIT);
EVENT_ATTR(rtg_alc_hitwb,	L2C_TAD_EVENT_RTG_ALC_HITWB);

EVENT_ATTR(stc_total,		L2C_TAD_EVENT_STC_TOTAL);
EVENT_ATTR(stc_total_fail,	L2C_TAD_EVENT_STC_TOTAL_FAIL);
EVENT_ATTR(stc_rmt,		L2C_TAD_EVENT_STC_RMT);
EVENT_ATTR(stc_rmt_fail,	L2C_TAD_EVENT_STC_RMT_FAIL);
EVENT_ATTR(stc_lcl,		L2C_TAD_EVENT_STC_LCL);
EVENT_ATTR(stc_lcl_fail,	L2C_TAD_EVENT_STC_LCL_FAIL);

EVENT_ATTR(oci_rtg_wait,	L2C_TAD_EVENT_OCI_RTG_WAIT);
EVENT_ATTR(oci_fwd_cyc_hit,	L2C_TAD_EVENT_OCI_FWD_CYC_HIT);
EVENT_ATTR(oci_fwd_race,	L2C_TAD_EVENT_OCI_FWD_RACE);
EVENT_ATTR(oci_haks,		L2C_TAD_EVENT_OCI_HAKS);
EVENT_ATTR(oci_fldx_tag_e_nodat, L2C_TAD_EVENT_OCI_FLDX_TAG_E_NODAT);
EVENT_ATTR(oci_fldx_tag_e_dat,	L2C_TAD_EVENT_OCI_FLDX_TAG_E_DAT);
EVENT_ATTR(oci_rldd,		L2C_TAD_EVENT_OCI_RLDD);
EVENT_ATTR(oci_rldd_pemd,	L2C_TAD_EVENT_OCI_RLDD_PEMD);
EVENT_ATTR(oci_rrq_dat_cnt,	L2C_TAD_EVENT_OCI_RRQ_DAT_CNT);
EVENT_ATTR(oci_rrq_dat_dmask,	L2C_TAD_EVENT_OCI_RRQ_DAT_DMASK);
EVENT_ATTR(oci_rsp_dat_cnt,	L2C_TAD_EVENT_OCI_RSP_DAT_CNT);
EVENT_ATTR(oci_rsp_dat_dmaks,	L2C_TAD_EVENT_OCI_RSP_DAT_DMASK);
EVENT_ATTR(oci_rsp_dat_vicd_cnt, L2C_TAD_EVENT_OCI_RSP_DAT_VICD_CNT);
EVENT_ATTR(oci_rsp_dat_vicd_dmask, L2C_TAD_EVENT_OCI_RSP_DAT_VICD_DMASK);
EVENT_ATTR(oci_rtg_alc_evict,	L2C_TAD_EVENT_OCI_RTG_ALC_EVICT);
EVENT_ATTR(oci_rtg_alc_vic,	L2C_TAD_EVENT_OCI_RTG_ALC_VIC);

static struct attribute *thunder_l2c_tad_pass2_events_attr[] = {
	EVENT_PTR(l2t_hit),
	EVENT_PTR(l2t_miss),
	EVENT_PTR(l2t_noalloc),
	EVENT_PTR(l2_vic),
	EVENT_PTR(sc_fail),
	EVENT_PTR(sc_pass),
	EVENT_PTR(lfb_occ),
	EVENT_PTR(wait_lfb),
	EVENT_PTR(wait_vab),
	EVENT_PTR(open_ccpi),

	EVENT_PTR(lookup),
	EVENT_PTR(lookup_xmc_lcl),
	EVENT_PTR(lookup_xmc_rmt),
	EVENT_PTR(lookup_mib),
	EVENT_PTR(lookup_all),

	EVENT_PTR(tag_alc_hit),
	EVENT_PTR(tag_alc_miss),
	EVENT_PTR(tag_alc_nalc),
	EVENT_PTR(tag_nalc_hit),
	EVENT_PTR(tag_nalc_miss),

	EVENT_PTR(lmc_wr),
	EVENT_PTR(lmc_sblkdty),

	EVENT_PTR(tag_alc_rtg_hit),
	EVENT_PTR(tag_alc_rtg_hite),
	EVENT_PTR(tag_alc_rtg_hits),
	EVENT_PTR(tag_alc_rtg_miss),
	EVENT_PTR(tag_alc_nalc_rtg_hit),
	EVENT_PTR(tag_nalc_rtg_miss),
	EVENT_PTR(tag_nalc_rtg_hite),
	EVENT_PTR(tag_nalc_rtg_hits),
	EVENT_PTR(tag_alc_lcl_evict),
	EVENT_PTR(tag_alc_lcl_clnvic),
	EVENT_PTR(tag_alc_lcl_dtyvic),
	EVENT_PTR(tag_alc_rmt_evict),
	EVENT_PTR(tag_alc_rmt_vic),

	EVENT_PTR(rtg_alc),
	EVENT_PTR(rtg_alc_hit),
	EVENT_PTR(rtg_alc_hitwb),

	EVENT_PTR(stc_total),
	EVENT_PTR(stc_total_fail),
	EVENT_PTR(stc_rmt),
	EVENT_PTR(stc_rmt_fail),
	EVENT_PTR(stc_lcl),
	EVENT_PTR(stc_lcl_fail),

	EVENT_PTR(oci_rtg_wait),
	EVENT_PTR(oci_fwd_cyc_hit),
	EVENT_PTR(oci_fwd_race),
	EVENT_PTR(oci_haks),
	EVENT_PTR(oci_fldx_tag_e_nodat),
	EVENT_PTR(oci_fldx_tag_e_dat),
	EVENT_PTR(oci_rldd),
	EVENT_PTR(oci_rldd_pemd),
	EVENT_PTR(oci_rrq_dat_cnt),
	EVENT_PTR(oci_rrq_dat_dmask),
	EVENT_PTR(oci_rsp_dat_cnt),
	EVENT_PTR(oci_rsp_dat_dmaks),
	EVENT_PTR(oci_rsp_dat_vicd_cnt),
	EVENT_PTR(oci_rsp_dat_vicd_dmask),
	EVENT_PTR(oci_rtg_alc_evict),
	EVENT_PTR(oci_rtg_alc_vic),

	EVENT_PTR(qd0_idx),
	EVENT_PTR(qd0_rdat),
	EVENT_PTR(qd0_bnks),
	EVENT_PTR(qd0_wdat),

	EVENT_PTR(qd1_idx),
	EVENT_PTR(qd1_rdat),
	EVENT_PTR(qd1_bnks),
	EVENT_PTR(qd1_wdat),

	EVENT_PTR(qd2_idx),
	EVENT_PTR(qd2_rdat),
	EVENT_PTR(qd2_bnks),
	EVENT_PTR(qd2_wdat),

	EVENT_PTR(qd3_idx),
	EVENT_PTR(qd3_rdat),
	EVENT_PTR(qd3_bnks),
	EVENT_PTR(qd3_wdat),

	EVENT_PTR(qd4_idx),
	EVENT_PTR(qd4_rdat),
	EVENT_PTR(qd4_bnks),
	EVENT_PTR(qd4_wdat),

	EVENT_PTR(qd5_idx),
	EVENT_PTR(qd5_rdat),
	EVENT_PTR(qd5_bnks),
	EVENT_PTR(qd5_wdat),

	EVENT_PTR(qd6_idx),
	EVENT_PTR(qd6_rdat),
	EVENT_PTR(qd6_bnks),
	EVENT_PTR(qd6_wdat),

	EVENT_PTR(qd7_idx),
	EVENT_PTR(qd7_rdat),
	EVENT_PTR(qd7_bnks),
	EVENT_PTR(qd7_wdat),
	NULL,
};

static struct attribute_group thunder_l2c_tad_events_group = {
	.name = "events",
	.attrs = NULL,
};

static const struct attribute_group *thunder_l2c_tad_attr_groups[] = {
	&thunder_uncore_attr_group,
	&thunder_l2c_tad_format_group,
	&thunder_l2c_tad_events_group,
	NULL,
};

struct pmu thunder_l2c_tad_pmu = {
	.attr_groups	= thunder_l2c_tad_attr_groups,
	.name		= "thunder_l2c_tad",
	.event_init	= thunder_uncore_event_init,
	.add		= thunder_uncore_add,
	.del		= thunder_uncore_del,
	.start		= thunder_uncore_start,
	.stop		= thunder_uncore_stop,
	.read		= thunder_uncore_read,
};

static int event_valid(u64 config)
{
	if ((config > 0 && config <= L2C_TAD_EVENT_WAIT_VAB) ||
	    config == L2C_TAD_EVENT_RTG_HIT ||
	    config == L2C_TAD_EVENT_RTG_MISS ||
	    config == L2C_TAD_EVENT_L2_RTG_VIC ||
	    config == L2C_TAD_EVENT_L2_OPEN_OCI ||
	    ((config & 0x80) && ((config & 0xf) <= 3)))
		return 1;

	if (thunder_uncore_version == 1)
		if (config == L2C_TAD_EVENT_OPEN_CCPI ||
		    (config >= L2C_TAD_EVENT_LOOKUP &&
		     config <= L2C_TAD_EVENT_LOOKUP_ALL) ||
		    (config >= L2C_TAD_EVENT_TAG_ALC_HIT &&
		     config <= L2C_TAD_EVENT_OCI_RTG_ALC_VIC &&
		     config != 0x4d &&
		     config != 0x66 &&
		     config != 0x67))
			return 1;

	return 0;
}

int __init thunder_uncore_l2c_tad_setup(void)
{
	int ret = -ENOMEM;

	thunder_uncore_l2c_tad = kzalloc(sizeof(struct thunder_uncore),
					 GFP_KERNEL);
	if (!thunder_uncore_l2c_tad)
		goto fail_nomem;

	if (thunder_uncore_version == 0)
		thunder_l2c_tad_events_group.attrs = thunder_l2c_tad_events_attr;
	else /* default */
		thunder_l2c_tad_events_group.attrs = thunder_l2c_tad_pass2_events_attr;

	ret = thunder_uncore_setup(thunder_uncore_l2c_tad,
			   PCI_DEVICE_ID_THUNDER_L2C_TAD,
			   L2C_TAD_CONTROL_OFFSET,
			   L2C_TAD_COUNTER_OFFSET + L2C_TAD_NR_COUNTERS
				* sizeof(unsigned long long),
			   &thunder_l2c_tad_pmu,
			   L2C_TAD_NR_COUNTERS);
	if (ret)
		goto fail;

	thunder_uncore_l2c_tad->type = L2C_TAD_TYPE;
	thunder_uncore_l2c_tad->event_valid = event_valid;
	return 0;

fail:
	kfree(thunder_uncore_l2c_tad);
fail_nomem:
	return ret;
}
