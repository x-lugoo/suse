#ifndef _ASM_X86_ALTERNATIVE_ASM_H
#define _ASM_X86_ALTERNATIVE_ASM_H

#ifdef __ASSEMBLY__

#include <asm/asm.h>

#ifdef CONFIG_SMP
	.macro LOCK_PREFIX
1:	lock
	.section .smp_locks,"a"
	.balign 4
	.long 1b - .
	.previous
	.endm
#else
	.macro LOCK_PREFIX
	.endm
#endif

.macro altinstruction_entry orig alt feature orig_len alt_len
	_ASM_ALIGN
	_ASM_PTR \orig
	_ASM_PTR \alt
	.word \feature
	.byte \orig_len
	.byte \alt_len
.endm

.macro ALTERNATIVE oldinstr, newinstr, feature
140:
       \oldinstr
141:

       .pushsection .altinstructions,"a"
       altinstruction_entry 140b,143f,\feature,141b-140b,144f-143f
       .popsection

       .pushsection .altinstr_replacement,"ax"
143:
       \newinstr
144:
       .popsection
.endm

.macro ALTERNATIVE_2 oldinstr, newinstr1, feature1, newinstr2, feature2
140:
       \oldinstr
141:

       .pushsection .altinstructions,"a"
       altinstruction_entry 140b,143f,\feature1,141b-140b,144f-143f
       altinstruction_entry 140b,144f,\feature2,141b-140b,145f-144f
       .popsection

       .pushsection .altinstr_replacement,"ax"
143:
       \newinstr1
144:
       \newinstr2
145:
       .popsection
.endm

#endif  /*  __ASSEMBLY__  */

#endif /* _ASM_X86_ALTERNATIVE_ASM_H */
