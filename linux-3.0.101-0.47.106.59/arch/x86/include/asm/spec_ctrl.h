#ifndef _ASM_X86_SPEC_CTRL_H
#define _ASM_X86_SPEC_CTRL_H

#include <linux/stringify.h>
#include <asm/msr-index.h>
#include <asm/cpufeature.h>
#include <asm/alternative-asm.h>

#ifdef __ASSEMBLY__

.macro __ENABLE_IBRS_CLOBBER
	movl $MSR_IA32_SPEC_CTRL, %ecx
	rdmsr
	orl $FEATURE_ENABLE_IBRS, %eax
	wrmsr
.endm

.macro ENABLE_IBRS_CLOBBER
	call x86_ibrs_enabled
	test %eax, %eax
	jz .Llfence_\@

	__ENABLE_IBRS_CLOBBER
	jmp .Lend_\@

.Llfence_\@:
	lfence
.Lend_\@:
.endm


.macro ENABLE_IBRS

	pushq %rax

	call x86_ibrs_enabled
	test %eax, %eax
	jz .Llfence_\@

	pushq %rcx
	pushq %rdx
	__ENABLE_IBRS_CLOBBER
	popq %rdx
	popq %rcx

	jmp .Lpop_\@

.Llfence_\@:
	lfence

.Lpop_\@:
	popq %rax

.Lend_\@:
.endm


.macro DISABLE_IBRS

	pushq %rax

	call x86_ibrs_enabled
	test %eax, %eax
	jz .Llfence_\@

	pushq %rcx
	pushq %rdx
	movl $MSR_IA32_SPEC_CTRL, %ecx
	rdmsr
	andl $(~FEATURE_ENABLE_IBRS), %eax
	wrmsr
	popq %rdx
	popq %rcx

	jmp .Lpop_\@

.Llfence_\@:
	lfence

.Lpop_\@:
	popq %rax

.Lend_\@:
.endm

#else /* __ASSEMBLY__ */
extern int ibrs_state;
void x86_enable_ibrs(void);
void x86_disable_ibrs(void);
unsigned int x86_ibrs_enabled(void);
unsigned int x86_ibpb_enabled(void);
void x86_spec_check(void);
int nospec(char *str);
void stuff_RSB(void);

static inline void x86_ibp_barrier(void)
{
	if (x86_ibpb_enabled())
		native_wrmsrl(MSR_IA32_PRED_CMD, FEATURE_SET_IBPB);
}

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_SPEC_CTRL_H */
