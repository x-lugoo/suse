#ifndef _ASM_X86_TLBFLUSH_H
#define _ASM_X86_TLBFLUSH_H

#include <linux/mm.h>
#include <linux/sched.h>

#include <asm/processor.h>
#include <asm/system.h>
#ifndef __GENKSYMS__
#include <asm/smp.h>
#endif

/*
 * Declare a couple of kaiser interfaces here for convenience,
 * to avoid the need for asm/kaiser.h in unexpected places.
 */
#ifdef CONFIG_KAISER
extern int kaiser_enabled;
extern void kaiser_setup_pcid(void);
extern void kaiser_flush_tlb_on_return_to_user(void);
#else
#define kaiser_enabled 0
static inline void kaiser_setup_pcid(void)
{
}
static inline void kaiser_flush_tlb_on_return_to_user(void)
{
}
#endif

#define __flush_tlb() xen_tlb_flush()
#define __flush_tlb_global() xen_tlb_flush()
#define __flush_tlb_single(addr) xen_invlpg(addr)
#define __flush_tlb_all() xen_tlb_flush()
#define __flush_tlb_one(addr) xen_invlpg(addr)

#ifdef CONFIG_X86_32
# define TLB_FLUSH_ALL	0xffffffff
#else
# define TLB_FLUSH_ALL	-1ULL
#endif

/*
 * TLB flushing:
 *
 *  - flush_tlb() flushes the current mm struct TLBs
 *  - flush_tlb_all() flushes all processes TLBs
 *  - flush_tlb_mm(mm) flushes the specified mm context TLB's
 *  - flush_tlb_page(vma, vmaddr) flushes one page
 *  - flush_tlb_range(vma, start, end) flushes a range of pages
 *  - flush_tlb_kernel_range(start, end) flushes a range of kernel pages
 *
 * ..but the i386 has somewhat limited tlb flushing capabilities,
 * and page-granular flushes are available only on i486 and up.
 */

#define local_flush_tlb() __flush_tlb()

#define flush_tlb_all xen_tlb_flush_all
#define flush_tlb_current_task() xen_tlb_flush_mask(mm_cpumask(current->mm))
#define flush_tlb_mm(mm) xen_tlb_flush_mask(mm_cpumask(mm))
#define flush_tlb_page(vma, va) xen_invlpg_mask(mm_cpumask((vma)->vm_mm), va)

#define flush_tlb()	flush_tlb_current_task()

static inline void flush_tlb_range(struct vm_area_struct *vma,
				   unsigned long start, unsigned long end)
{
	flush_tlb_mm(vma->vm_mm);
}

#ifndef CONFIG_XEN
#define TLBSTATE_OK	1
#define TLBSTATE_LAZY	2

struct tlb_state {
	struct mm_struct *active_mm;
	int state;
};
DECLARE_PER_CPU_SHARED_ALIGNED(struct tlb_state, cpu_tlbstate);

static inline void reset_lazy_tlbstate(void)
{
	percpu_write(cpu_tlbstate.state, 0);
	percpu_write(cpu_tlbstate.active_mm, &init_mm);
}
#endif

static inline void flush_tlb_kernel_range(unsigned long start,
					  unsigned long end)
{
	flush_tlb_all();
}

#endif /* _ASM_X86_TLBFLUSH_H */
