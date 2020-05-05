#include <linux/perf_event.h>
#include <linux/pci.h>
#include <linux/list.h>
#include <linux/io.h>

#undef pr_fmt
#define pr_fmt(fmt)     "thunderx_uncore: " fmt

enum uncore_type {
	L2C_TAD_TYPE,
	L2C_CBC_TYPE,
	LMC_TYPE,
	OCX_TLK_TYPE,
};

extern int thunder_uncore_version;

#define UNCORE_EVENT_ID_MASK		0xffff
#define UNCORE_EVENT_ID_SHIFT		16

/* maximum number of parallel hardware counters for all uncore parts */
#define MAX_COUNTERS			64

struct thunder_uncore_unit {
	struct list_head entry;
	void __iomem *map;
	struct pci_dev *pdev;
};

struct thunder_uncore_node {
	int nr_units;
	int num_counters;
	struct list_head unit_list;
	struct perf_event *events[MAX_COUNTERS];
};

/* generic uncore struct for different pmu types */
struct thunder_uncore {
	int type;
	struct pmu *pmu;
	int (*event_valid)(u64);
	struct notifier_block cpu_nb;
	struct thunder_uncore_node *nodes[MAX_NUMNODES];
};

#define EVENT_PTR(_id) (&event_attr_##_id.attr.attr)

#define EVENT_ATTR(_name, _val)						   \
static struct perf_pmu_events_attr event_attr_##_name = {		   \
	.attr	   = __ATTR(_name, 0444, thunder_events_sysfs_show, NULL), \
	.event_str = "event=" __stringify(_val),			   \
}

#define EVENT_ATTR_STR(_name, _str)					   \
static struct perf_pmu_events_attr event_attr_##_name = {		   \
	.attr	   = __ATTR(_name, 0444, thunder_events_sysfs_show, NULL), \
	.event_str = _str,						   \
}

static inline struct thunder_uncore_node *get_node(u64 config,
					struct thunder_uncore *uncore)
{
	return uncore->nodes[config >> UNCORE_EVENT_ID_SHIFT];
}

#define get_id(config) (config & UNCORE_EVENT_ID_MASK)

extern struct attribute_group thunder_uncore_attr_group;
extern struct device_attribute format_attr_node;

extern struct thunder_uncore *thunder_uncore_l2c_tad;
extern struct thunder_uncore *thunder_uncore_l2c_cbc;
extern struct thunder_uncore *thunder_uncore_lmc;
extern struct thunder_uncore *thunder_uncore_ocx_tlk;
extern struct pmu thunder_l2c_tad_pmu;
extern struct pmu thunder_l2c_cbc_pmu;
extern struct pmu thunder_lmc_pmu;
extern struct pmu thunder_ocx_tlk_pmu;

/* Prototypes */
struct thunder_uncore *event_to_thunder_uncore(struct perf_event *event);
void thunder_uncore_del(struct perf_event *event, int flags);
int thunder_uncore_event_init(struct perf_event *event);
void thunder_uncore_read(struct perf_event *event);
int thunder_uncore_setup(struct thunder_uncore *uncore, int id,
			 unsigned long offset, unsigned long size,
			 struct pmu *pmu, int counters);
ssize_t thunder_events_sysfs_show(struct device *dev,
				  struct device_attribute *attr,
				  char *page);

int thunder_uncore_l2c_tad_setup(void);
int thunder_uncore_l2c_cbc_setup(void);
int thunder_uncore_lmc_setup(void);
int thunder_uncore_ocx_tlk_setup(void);
