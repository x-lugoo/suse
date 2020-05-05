#include <linux/init.h>

#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/cpu.h>

#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/cache.h>

void switch_mm(struct mm_struct *prev, struct mm_struct *next,
		struct task_struct *tsk)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm_irqs_off(prev, next, tsk);
	local_irq_restore(flags);
}

void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
			struct task_struct *tsk)
{
	unsigned cpu = smp_processor_id();
	struct mmuext_op _op[2 + (sizeof(long) > 4)], *op = _op;
#ifdef CONFIG_X86_64
	pgd_t *upgd;
#endif

	if (likely(prev != next)) {
		BUG_ON(!xen_feature(XENFEAT_writable_page_tables) &&
		       !PagePinned(virt_to_page(next->pgd)));

#if defined(CONFIG_SMP) && !defined(CONFIG_XEN) /* XEN: no lazy tlb */
		percpu_write(cpu_tlbstate.state, TLBSTATE_OK);
		percpu_write(cpu_tlbstate.active_mm, next);
#endif
		cpumask_set_cpu(cpu, mm_cpumask(next));

		/*
		 * Re-load page tables: load_cr3(next->pgd).
		 *
		 * This logic has an ordering constraint:
		 *
		 *  CPU 0: Write to a PTE for 'next'
		 *  CPU 0: load bit 1 in mm_cpumask.  if nonzero, send IPI.
		 *  CPU 1: set bit 1 in next's mm_cpumask
		 *  CPU 1: load from the PTE that CPU 0 writes (implicit)
		 *
		 * We need to prevent an outcome in which CPU 1 observes
		 * the new PTE value and CPU 0 observes bit 1 clear in
		 * mm_cpumask.  (If that occurs, then the IPI will never
		 * be sent, and CPU 0's TLB will contain a stale entry.)
		 *
		 * The bad outcome can occur if either CPU's load is
		 * reordered before that CPU's store, so both CPUs much
		 * execute full barriers to prevent this from happening.
		 *
		 * Thus, switch_mm needs a full barrier between the
		 * store to mm_cpumask and any operation that could load
		 * from next->pgd.  This barrier synchronizes with
		 * remote TLB flushers.  Fortunately, load_cr3 is
		 * serializing and thus acts as a full barrier.
		 *
		 */
		op->cmd = MMUEXT_NEW_BASEPTR;
		op->arg1.mfn = virt_to_mfn(next->pgd);
		op++;

		/* xen_new_user_pt(next->pgd) */
#ifdef CONFIG_X86_64
		op->cmd = MMUEXT_NEW_USER_BASEPTR;
		upgd = __user_pgd(next->pgd);
		op->arg1.mfn = likely(upgd) ? virt_to_mfn(upgd) : 0;
		op++;
#endif

		/*
		 * load the LDT, if the LDT is different:
		 */
		if (unlikely(prev->context.ldt != next->context.ldt)) {
			/* load_mm_ldt(next) */
			const struct ldt_struct *ldt;

			/* lockless_dereference synchronizes with smp_store_release */
			ldt = lockless_dereference(next->context.ldt);
			op->cmd = MMUEXT_SET_LDT;
			if (unlikely(ldt)) {
				op->arg1.linear_addr = (long)ldt->entries;
				op->arg2.nr_ents     = ldt->size;
			} else {
				op->arg1.linear_addr = 0;
				op->arg2.nr_ents     = 0;
			}
			op++;
		}

		BUG_ON(HYPERVISOR_mmuext_op(_op, op-_op, NULL, DOMID_SELF));

		/* stop TLB flushes for the previous mm */
		cpumask_clear_cpu(cpu, mm_cpumask(prev));
	}
#if defined(CONFIG_SMP) && !defined(CONFIG_XEN) /* XEN: no lazy tlb */
	else {
		percpu_write(cpu_tlbstate.state, TLBSTATE_OK);
		BUG_ON(percpu_read(cpu_tlbstate.active_mm) != next);

		if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next))) {
			/*
			 * We were in lazy tlb mode and leave_mm disabled
			 * tlb flush IPI delivery. We must reload CR3
			 * to make sure to use no freed page tables.
			 *
			 * As above, this is a barrier that forces
			 * TLB repopulation to be ordered after the
			 * store to mm_cpumask.
			 */
			load_cr3(next->pgd);
			xen_new_user_pt(next->pgd);
			load_mm_ldt(next);
		}
	}
#endif
}
