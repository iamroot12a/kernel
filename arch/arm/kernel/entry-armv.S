/*
 *  linux/arch/arm/kernel/entry-armv.S
 *
 *  Copyright (C) 1996,1997,1998 Russell King.
 *  ARM700 fix by Matthew Godbolt (linux-user@willothewisp.demon.co.uk)
 *  nommu support by Hyok S. Choi (hyok.choi@samsung.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Low-level vector interface routines
 *
 *  Note:  there is a StrongARM bug in the STMIA rn, {regs}^ instruction
 *  that causes it to save wrong values...  Be aware!
 */

#include <asm/assembler.h>
#include <asm/memory.h>
#include <asm/glue-df.h>
#include <asm/glue-pf.h>
#include <asm/vfpmacros.h>
#ifndef CONFIG_MULTI_IRQ_HANDLER
#include <mach/entry-macro.S>
#endif
#include <asm/thread_notify.h>
#include <asm/unwind.h>
#include <asm/unistd.h>
#include <asm/tls.h>
#include <asm/system_info.h>

#include "entry-header.S"
#include <asm/entry-macro-multi.S>
#include <asm/probes.h>

/*
 * Interrupt handling.
 */
	.macro	irq_handler

/* IAMROOT-12:
 * -------------
 * Device Tree등을 사용하는 시스템에서는 컴파일 타임에 아키텍처가 결정되지 않아
 * irq 핸들러 함수를 결정할 수 없으므로 런타임에 결정해서 동작해야 한다.
 * (부트업 타임에 전역 handle_arch_irq 후크에 런타임에 결정된 irq 핸들러를 
 * 설정해줘야 한다.)
 *
 * 추후 커널 v4.4 이상에서는 아래 MULTI_IRQ_HANDLER를 설정하여 사용한다.
 */
#ifdef CONFIG_MULTI_IRQ_HANDLER
	ldr	r1, =handle_arch_irq
	mov	r0, sp
	adr	lr, BSYM(9997f)
	ldr	pc, [r1]
#else

/* IAMROOT-12:
 * -------------
 * 컴파일 타임에 해당 아키텍처가 결정되었으므로 irq 핸들러를 직접 호출한다.
 * rpi2: MULTI_IRQ_HANDLER를 사용하지 않는다.
 *
 * -> arch/arm/include/asm/entry-macro-multi.S 
 */
	arch_irq_handler_default
#endif
9997:
	.endm

	.macro	pabt_helper
	@ PABORT handler takes pt_regs in r2, fault address in r4 and psr in r5
#ifdef MULTI_PABORT
/* IAMROOT-12:
 * -------------
 * 컴파일 타임에 결정하지 않고 런타임에 아키텍처가 결정되는 경우에 사용된다.
 * (Device Tree를 사용해야 하는 경우에는 무조건 이러한 방법을 사용한다.)
 *
 * 전역 processor (processor 구조체) 포인터 -> ip에 저장
 */
	ldr	ip, .LCprocfns
	mov	lr, pc

/* IAMROOT-12:
 * -------------
 * processor->_prefetch_abort() 함수로 jump한다.
 */

	ldr	pc, [ip, #PROCESSOR_PABT_FUNC]
#else
/* IAMROOT-12:
 * -------------
 * rpi2: ARMv7 코드로 고정되어 컴파일되므로 아래 루틴이 동작한다.
 * (arch/arm/mm/pabort-v7.S - v7_pabort 엔트리 수행)
 */
	bl	CPU_PABORT_HANDLER
#endif
	.endm

	.macro	dabt_helper

	@
	@ Call the processor-specific abort handler:
	@
	@  r2 - pt_regs
	@  r4 - aborted context pc
	@  r5 - aborted context psr
	@
	@ The abort handler must return the aborted address in r0, and
	@ the fault status register in r1.  r9 must be preserved.
	@
#ifdef MULTI_DABORT
	ldr	ip, .LCprocfns
	mov	lr, pc
	ldr	pc, [ip, #PROCESSOR_DABT_FUNC]
#else
	bl	CPU_DABORT_HANDLER
#endif
	.endm

#ifdef CONFIG_KPROBES
	.section	.kprobes.text,"ax",%progbits
#else
	.text
#endif

/*
 * Invalid mode handlers
 */
	.macro	inv_entry, reason
	sub	sp, sp, #S_FRAME_SIZE
 ARM(	stmib	sp, {r1 - lr}		)
 THUMB(	stmia	sp, {r0 - r12}		)
 THUMB(	str	sp, [sp, #S_SP]		)
 THUMB(	str	lr, [sp, #S_LR]		)
	mov	r1, #\reason
	.endm

__pabt_invalid:
	inv_entry BAD_PREFETCH
	b	common_invalid
ENDPROC(__pabt_invalid)

__dabt_invalid:
	inv_entry BAD_DATA
	b	common_invalid
ENDPROC(__dabt_invalid)

__irq_invalid:
	inv_entry BAD_IRQ
	b	common_invalid
ENDPROC(__irq_invalid)

__und_invalid:
	inv_entry BAD_UNDEFINSTR

	@
	@ XXX fall through to common_invalid
	@

@
@ common_invalid - generic code for failed exception (re-entrant version of handlers)
@
common_invalid:
	zero_fp

	ldmia	r0, {r4 - r6}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r7, #-1			@  ""   ""    ""        ""
	str	r4, [sp]		@ save preserved r0
	stmia	r0, {r5 - r7}		@ lr_<exception>,
					@ cpsr_<exception>, "old_r0"

	mov	r0, sp
	b	bad_mode
ENDPROC(__und_invalid)

/*
 * SVC mode handlers
 */

/* IAMROOT-12:
 * -------------
 * CONFIG_AEABI를 지원하는 경우만 코드가 컴파일되게 한다.
 */
#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5)
#define SPFIX(code...) code
#else
#define SPFIX(code...)
#endif

	.macro	svc_entry, stack_hole=0, trace=1
 UNWIND(.fnstart		)
 UNWIND(.save {r0 - pc}		)

/* IAMROOT-12:
 * -------------
 * sp=레지스터들 + stack_hole - 4 바이트 (r0용은 루틴 뒤에서 추가)
 *
 * stack_hole은 CONFIG_KPROBES 커널 옵션을 사용할 때 stmdb등 멀티플 명령에
 * 대해 에뮬레이션하며 최대 16개의 레지스터 백업 공간을 확보하게 한다.
 */
	sub	sp, sp, #(S_FRAME_SIZE + \stack_hole - 4)
#ifdef CONFIG_THUMB2_KERNEL
 SPFIX(	str	r0, [sp]	)	@ temporarily saved
 SPFIX(	mov	r0, sp		)
 SPFIX(	tst	r0, #4		)	@ test original stack alignment
 SPFIX(	ldr	r0, [sp]	)	@ restored
#else

/* IAMROOT-12:
 * -------------
 * 8바이트 정렬여부를 체크한다.
 */
 SPFIX(	tst	sp, #4		)
#endif

/* IAMROOT-12:
 * -------------
 * r0를 제외한 스택이 8바이트 정렬된 경우 스택을 4만큼 증가한다.
 * (결국 r0를 포함한 스택이 8바이트 정렬되어 있지 않은 경우 4만큼 추가하여 정렬)
 * sp=0x0000_1004 
 * sp=0x0000_1008 -> 0x0000_10004 
 */
 SPFIX(	subeq	sp, sp, #4	)
	stmia	sp, {r1 - r12}

	ldmia	r0, {r3 - r5}
	add	r7, sp, #S_SP - 4	@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""      ""       ""
	add	r2, sp, #(S_FRAME_SIZE + \stack_hole - 4)
 SPFIX(	addeq	r2, r2, #4	)
	str	r3, [sp, #-4]!		@ save the "real" r0 copied
					@ from the exception stack

	mov	r3, lr

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r2 - sp_svc
	@  r3 - lr_svc
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	stmia	r7, {r2 - r6}

	.if \trace
#ifdef CONFIG_TRACE_IRQFLAGS
	bl	trace_hardirqs_off
#endif
	.endif
	.endm

	.align	5
__dabt_svc:
	svc_entry
	mov	r2, sp
	dabt_helper
 THUMB(	ldr	r5, [sp, #S_PSR]	)	@ potentially updated CPSR
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__dabt_svc)

	.align	5
__irq_svc:
	svc_entry
	irq_handler

#ifdef CONFIG_PREEMPT
	get_thread_info tsk
	ldr	r8, [tsk, #TI_PREEMPT]		@ get preempt count
	ldr	r0, [tsk, #TI_FLAGS]		@ get flags
	teq	r8, #0				@ if preempt count != 0
	movne	r0, #0				@ force flags to 0
	tst	r0, #_TIF_NEED_RESCHED
	blne	svc_preempt
#endif

	svc_exit r5, irq = 1			@ return from exception
 UNWIND(.fnend		)
ENDPROC(__irq_svc)

	.ltorg

#ifdef CONFIG_PREEMPT
svc_preempt:
	mov	r8, lr
1:	bl	preempt_schedule_irq		@ irq en/disable is done inside
	ldr	r0, [tsk, #TI_FLAGS]		@ get new tasks TI_FLAGS
	tst	r0, #_TIF_NEED_RESCHED
	reteq	r8				@ go again
	b	1b
#endif

__und_fault:
	@ Correct the PC such that it is pointing at the instruction
	@ which caused the fault.  If the faulting instruction was ARM
	@ the PC will be pointing at the next instruction, and have to
	@ subtract 4.  Otherwise, it is Thumb, and the PC will be
	@ pointing at the second half of the Thumb instruction.  We
	@ have to subtract 2.
	ldr	r2, [r0, #S_PC]
	sub	r2, r2, r1
	str	r2, [r0, #S_PC]
	b	do_undefinstr
ENDPROC(__und_fault)

	.align	5
__und_svc:
#ifdef CONFIG_KPROBES
	@ If a kprobe is about to simulate a "stmdb sp..." instruction,
	@ it obviously needs free stack space which then will belong to
	@ the saved context.
	svc_entry MAX_STACK_SIZE
#else
	svc_entry
#endif
	@
	@ call emulation code, which returns using r9 if it has emulated
	@ the instruction, or the more conventional lr if we are to treat
	@ this as a real undefined instruction
	@
	@  r0 - instruction
	@
#ifndef CONFIG_THUMB2_KERNEL
	ldr	r0, [r4, #-4]
#else
	mov	r1, #2
	ldrh	r0, [r4, #-2]			@ Thumb instruction at LR - 2
	cmp	r0, #0xe800			@ 32-bit instruction if xx >= 0
	blo	__und_svc_fault
	ldrh	r9, [r4]			@ bottom 16 bits
	add	r4, r4, #2
	str	r4, [sp, #S_PC]
	orr	r0, r9, r0, lsl #16
#endif
	adr	r9, BSYM(__und_svc_finish)
	mov	r2, r4
	bl	call_fpe

	mov	r1, #4				@ PC correction to apply
__und_svc_fault:
	mov	r0, sp				@ struct pt_regs *regs
	bl	__und_fault

__und_svc_finish:
	ldr	r5, [sp, #S_PSR]		@ Get SVC cpsr
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__und_svc)

	.align	5

/* IAMROOT-12:
 * -------------
 * 커널(svc)모드에서 pre-fetch abort exception되어 진입한 경우
 */
__pabt_svc:
	svc_entry
	mov	r2, sp				@ regs
	pabt_helper
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__pabt_svc)

	.align	5
__fiq_svc:
	svc_entry trace=0
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi
	svc_exit_via_fiq
 UNWIND(.fnend		)
ENDPROC(__fiq_svc)

	.align	5

/* IAMROOT-12:
 * -------------
 * cr_alignment 주소를 가리킨다.
 */
.LCcralign:
	.word	cr_alignment
#ifdef MULTI_DABORT
.LCprocfns:

/* IAMROOT-12:
 * -------------
 * 전역 processor (processor 구조체)를 가리키는 포인터
 */
	.word	processor
#endif
.LCfp:
	.word	fp_enter

/*
 * Abort mode handlers
 */

@
@ Taking a FIQ in abort mode is similar to taking a FIQ in SVC mode
@ and reuses the same macros. However in abort mode we must also
@ save/restore lr_abt and spsr_abt to make nested aborts safe.
@
	.align 5
__fiq_abt:
	svc_entry trace=0

 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	r1, lr		@ Save lr_abt
	mrs	r2, spsr	@ Save spsr_abt, abort is now safe
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	stmfd	sp!, {r1 - r2}

	add	r0, sp, #8			@ struct pt_regs *regs
	bl	handle_fiq_as_nmi

	ldmfd	sp!, {r1 - r2}
 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	lr, r1		@ Restore lr_abt, abort is unsafe
	msr	spsr_cxsf, r2	@ Restore spsr_abt
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )

	svc_exit_via_fiq
 UNWIND(.fnend		)
ENDPROC(__fiq_abt)

/*
 * User mode handlers
 *
 * EABI note: sp_svc is always 64-bit aligned here, so should S_FRAME_SIZE
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5) && (S_FRAME_SIZE & 7)
#error "sizeof(struct pt_regs) must be a multiple of 8"
#endif

	.macro	usr_entry, trace=1
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)	@ don't unwind the user space

/* IAMROOT-12:
 * -------------
 * stack을 pt_regs 구조체 사이즈 만큼 증가시키고 r1~r12를 백업한다.
 */
	sub	sp, sp, #S_FRAME_SIZE
 ARM(	stmib	sp, {r1 - r12}	)
 THUMB(	stmia	sp, {r0 - r12}	)

/* IAMROOT-12:
 * -------------
 * r7 <- SCTLR 
 * r8 <- cr_alignment 주소
 */
 ATRAP(	mrc	p15, 0, r7, c1, c0, 0)
 ATRAP(	ldr	r8, .LCcralign)

/* IAMROOT-12:
 * -------------
 * r3 ~ r5 <- [r0](이전 모드의 스택 - 3 word stack(r0_<old>, lr_<e>, spsr)
 */
	ldmia	r0, {r3 - r5}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""     ""        ""

/* IAMROOT-12:
 * -------------
 * pt_regs의 첫 번째 r0 위치에 r3(기존 r0)를 백업한다.
 */
	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack

/* IAMROOT-12:
 * -------------
 * r8 <- cr_alignment 값을 로드 
 */
 ATRAP(	ldr	r8, [r8, #0])

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	@ Also, separately save sp_usr and lr_usr
	@

/* IAMROOT-12:
 * -------------
 * stack에 위치한 pt_regs 값
 * ---------------------------
 *           old-r0 <- orig_r0 
 *           psr    <- spsr_<e>  
 *   r0 ->   pc     <- lr_<e>
 *           lr     <- lr
 *           sp     <- sp
 *           ....
 *   sp ->   r0 
 */
	stmia	r0, {r4 - r6}
 ARM(	stmdb	r0, {sp, lr}^			)
 THUMB(	store_user_sp_lr r0, r1, S_SP - S_PC	)

	@ Enable the alignment trap while in kernel mode

/* IAMROOT-12:
 * -------------
 * SCTLR 값을 읽어서 저장해두었던 cr_alignment값과 
 * exception되어 다시 SCTLR을 읽어서 비교하여 다른 경우 
 * cr_alignment -> SCTLR에 저장하여 설정한다.
 */
 ATRAP(	teq	r8, r7)
 ATRAP( mcrne	p15, 0, r8, c1, c0, 0)

	@
	@ Clear FP to mark the first stack frame
	@

/* IAMROOT-12:
 * -------------
 * back trace 등의 디버그 정보를 위해 CONFIG_FRAME_POINTER를 설정하고 
 * 사용한다. 이 옵션을 설정하지 않으면 frame pointer에 대한 정보를 
 * 사용하지 못해 디버그 정보가 상당히 제한된다.
 *
 * 추측: 이 시점 이전의 back trace는 추적을 못하므로 0으로 초기화한다.
 */
	zero_fp

	.if	\trace

/* IAMROOT-12:
 * -------------
 * hardirq tracing을 위한 코드
 */
#ifdef CONFIG_IRQSOFF_TRACER
	bl	trace_hardirqs_off
#endif
	ct_user_exit save = 0
	.endif
	.endm

	.macro	kuser_cmpxchg_check
#if !defined(CONFIG_CPU_32v6K) && defined(CONFIG_KUSER_HELPERS) && \
    !defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)
#ifndef CONFIG_MMU
#warning "NPTL on non MMU needs fixing"
#else
	@ Make sure our user space atomic helper is restarted
	@ if it was interrupted in a critical region.  Here we
	@ perform a quick test inline since it should be false
	@ 99.9999% of the time.  The rest is done out of line.
	cmp	r4, #TASK_SIZE
	blhs	kuser_cmpxchg64_fixup
#endif
#endif
	.endm

	.align	5
__dabt_usr:
	usr_entry
	kuser_cmpxchg_check
	mov	r2, sp
	dabt_helper
	b	ret_from_exception
 UNWIND(.fnend		)
ENDPROC(__dabt_usr)

	.align	5
__irq_usr:
	usr_entry
	kuser_cmpxchg_check
	irq_handler
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user_from_irq
 UNWIND(.fnend		)
ENDPROC(__irq_usr)

	.ltorg

	.align	5
__und_usr:
	usr_entry

	mov	r2, r4
	mov	r3, r5

	@ r2 = regs->ARM_pc, which is either 2 or 4 bytes ahead of the
	@      faulting instruction depending on Thumb mode.
	@ r3 = regs->ARM_cpsr
	@
	@ The emulation code returns using r9 if it has emulated the
	@ instruction, or the more conventional lr if we are to treat
	@ this as a real undefined instruction
	@
	adr	r9, BSYM(ret_from_exception)

	@ IRQs must be enabled before attempting to read the instruction from
	@ user space since that could cause a page/translation fault if the
	@ page table was modified by another CPU.
	enable_irq

	tst	r3, #PSR_T_BIT			@ Thumb mode?
	bne	__und_usr_thumb
	sub	r4, r2, #4			@ ARM instr at LR - 4
1:	ldrt	r0, [r4]
 ARM_BE8(rev	r0, r0)				@ little endian instruction

	@ r0 = 32-bit ARM instruction which caused the exception
	@ r2 = PC value for the following instruction (:= regs->ARM_pc)
	@ r4 = PC value for the faulting instruction
	@ lr = 32-bit undefined instruction function
	adr	lr, BSYM(__und_usr_fault_32)
	b	call_fpe

__und_usr_thumb:
	@ Thumb instruction
	sub	r4, r2, #2			@ First half of thumb instr at LR - 2
#if CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7
/*
 * Thumb-2 instruction handling.  Note that because pre-v6 and >= v6 platforms
 * can never be supported in a single kernel, this code is not applicable at
 * all when __LINUX_ARM_ARCH__ < 6.  This allows simplifying assumptions to be
 * made about .arch directives.
 */
#if __LINUX_ARM_ARCH__ < 7
/* If the target CPU may not be Thumb-2-capable, a run-time check is needed: */
#define NEED_CPU_ARCHITECTURE
	ldr	r5, .LCcpu_architecture
	ldr	r5, [r5]
	cmp	r5, #CPU_ARCH_ARMv7
	blo	__und_usr_fault_16		@ 16bit undefined instruction
/*
 * The following code won't get run unless the running CPU really is v7, so
 * coding round the lack of ldrht on older arches is pointless.  Temporarily
 * override the assembler target arch with the minimum required instead:
 */
	.arch	armv6t2
#endif
2:	ldrht	r5, [r4]
ARM_BE8(rev16	r5, r5)				@ little endian instruction
	cmp	r5, #0xe800			@ 32bit instruction if xx != 0
	blo	__und_usr_fault_16		@ 16bit undefined instruction
3:	ldrht	r0, [r2]
ARM_BE8(rev16	r0, r0)				@ little endian instruction
	add	r2, r2, #2			@ r2 is PC + 2, make it PC + 4
	str	r2, [sp, #S_PC]			@ it's a 2x16bit instr, update
	orr	r0, r0, r5, lsl #16
	adr	lr, BSYM(__und_usr_fault_32)
	@ r0 = the two 16-bit Thumb instructions which caused the exception
	@ r2 = PC value for the following Thumb instruction (:= regs->ARM_pc)
	@ r4 = PC value for the first 16-bit Thumb instruction
	@ lr = 32bit undefined instruction function

#if __LINUX_ARM_ARCH__ < 7
/* If the target arch was overridden, change it back: */
#ifdef CONFIG_CPU_32v6K
	.arch	armv6k
#else
	.arch	armv6
#endif
#endif /* __LINUX_ARM_ARCH__ < 7 */
#else /* !(CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7) */
	b	__und_usr_fault_16
#endif
 UNWIND(.fnend)
ENDPROC(__und_usr)

/*
 * The out of line fixup for the ldrt instructions above.
 */
	.pushsection .fixup, "ax"
	.align	2
4:	str     r4, [sp, #S_PC]			@ retry current instruction
	ret	r9
	.popsection
	.pushsection __ex_table,"a"
	.long	1b, 4b
#if CONFIG_ARM_THUMB && __LINUX_ARM_ARCH__ >= 6 && CONFIG_CPU_V7
	.long	2b, 4b
	.long	3b, 4b
#endif
	.popsection

/*
 * Check whether the instruction is a co-processor instruction.
 * If yes, we need to call the relevant co-processor handler.
 *
 * Note that we don't do a full check here for the co-processor
 * instructions; all instructions with bit 27 set are well
 * defined.  The only instructions that should fault are the
 * co-processor instructions.  However, we have to watch out
 * for the ARM6/ARM7 SWI bug.
 *
 * NEON is a special case that has to be handled here. Not all
 * NEON instructions are co-processor instructions, so we have
 * to make a special case of checking for them. Plus, there's
 * five groups of them, so we have a table of mask/opcode pairs
 * to check against, and if any match then we branch off into the
 * NEON handler code.
 *
 * Emulators may wish to make use of the following registers:
 *  r0  = instruction opcode (32-bit ARM or two 16-bit Thumb)
 *  r2  = PC value to resume execution after successful emulation
 *  r9  = normal "successful" return address
 *  r10 = this threads thread_info structure
 *  lr  = unrecognised instruction return address
 * IRQs enabled, FIQs enabled.
 */
	@
	@ Fall-through from Thumb-2 __und_usr
	@
#ifdef CONFIG_NEON
	get_thread_info r10			@ get current thread
	adr	r6, .LCneon_thumb_opcodes
	b	2f
#endif
call_fpe:
	get_thread_info r10			@ get current thread
#ifdef CONFIG_NEON
	adr	r6, .LCneon_arm_opcodes
2:	ldr	r5, [r6], #4			@ mask value
	ldr	r7, [r6], #4			@ opcode bits matching in mask
	cmp	r5, #0				@ end mask?
	beq	1f
	and	r8, r0, r5
	cmp	r8, r7				@ NEON instruction?
	bne	2b
	mov	r7, #1
	strb	r7, [r10, #TI_USED_CP + 10]	@ mark CP#10 as used
	strb	r7, [r10, #TI_USED_CP + 11]	@ mark CP#11 as used
	b	do_vfp				@ let VFP handler handle this
1:
#endif
	tst	r0, #0x08000000			@ only CDP/CPRT/LDC/STC have bit 27
	tstne	r0, #0x04000000			@ bit 26 set on both ARM and Thumb-2
	reteq	lr
	and	r8, r0, #0x00000f00		@ mask out CP number
 THUMB(	lsr	r8, r8, #8		)
	mov	r7, #1
	add	r6, r10, #TI_USED_CP
 ARM(	strb	r7, [r6, r8, lsr #8]	)	@ set appropriate used_cp[]
 THUMB(	strb	r7, [r6, r8]		)	@ set appropriate used_cp[]
#ifdef CONFIG_IWMMXT
	@ Test if we need to give access to iWMMXt coprocessors
	ldr	r5, [r10, #TI_FLAGS]
	rsbs	r7, r8, #(1 << 8)		@ CP 0 or 1 only
	movcss	r7, r5, lsr #(TIF_USING_IWMMXT + 1)
	bcs	iwmmxt_task_enable
#endif
 ARM(	add	pc, pc, r8, lsr #6	)
 THUMB(	lsl	r8, r8, #2		)
 THUMB(	add	pc, r8			)
	nop

	ret.w	lr				@ CP#0
	W(b)	do_fpe				@ CP#1 (FPE)
	W(b)	do_fpe				@ CP#2 (FPE)
	ret.w	lr				@ CP#3
#ifdef CONFIG_CRUNCH
	b	crunch_task_enable		@ CP#4 (MaverickCrunch)
	b	crunch_task_enable		@ CP#5 (MaverickCrunch)
	b	crunch_task_enable		@ CP#6 (MaverickCrunch)
#else
	ret.w	lr				@ CP#4
	ret.w	lr				@ CP#5
	ret.w	lr				@ CP#6
#endif
	ret.w	lr				@ CP#7
	ret.w	lr				@ CP#8
	ret.w	lr				@ CP#9
#ifdef CONFIG_VFP
	W(b)	do_vfp				@ CP#10 (VFP)
	W(b)	do_vfp				@ CP#11 (VFP)
#else
	ret.w	lr				@ CP#10 (VFP)
	ret.w	lr				@ CP#11 (VFP)
#endif
	ret.w	lr				@ CP#12
	ret.w	lr				@ CP#13
	ret.w	lr				@ CP#14 (Debug)
	ret.w	lr				@ CP#15 (Control)

#ifdef NEED_CPU_ARCHITECTURE
	.align	2
.LCcpu_architecture:
	.word	__cpu_architecture
#endif

#ifdef CONFIG_NEON
	.align	6

.LCneon_arm_opcodes:
	.word	0xfe000000			@ mask
	.word	0xf2000000			@ opcode

	.word	0xff100000			@ mask
	.word	0xf4000000			@ opcode

	.word	0x00000000			@ mask
	.word	0x00000000			@ opcode

.LCneon_thumb_opcodes:
	.word	0xef000000			@ mask
	.word	0xef000000			@ opcode

	.word	0xff100000			@ mask
	.word	0xf9000000			@ opcode

	.word	0x00000000			@ mask
	.word	0x00000000			@ opcode
#endif

do_fpe:
	ldr	r4, .LCfp
	add	r10, r10, #TI_FPSTATE		@ r10 = workspace
	ldr	pc, [r4]			@ Call FP module USR entry point

/*
 * The FP module is called with these registers set:
 *  r0  = instruction
 *  r2  = PC+4
 *  r9  = normal "successful" return address
 *  r10 = FP workspace
 *  lr  = unrecognised FP instruction return address
 */

	.pushsection .data
ENTRY(fp_enter)
	.word	no_fp
	.popsection

ENTRY(no_fp)
	ret	lr
ENDPROC(no_fp)

__und_usr_fault_32:
	mov	r1, #4
	b	1f
__und_usr_fault_16:
	mov	r1, #2
1:	mov	r0, sp
	adr	lr, BSYM(ret_from_exception)
	b	__und_fault
ENDPROC(__und_usr_fault_32)
ENDPROC(__und_usr_fault_16)

	.align	5
__pabt_usr:

/* IAMROOT-12:
 * -------------
 * 스택에 pt_regs 공간을 확보하고 레지스터들을 보관한다.
 */
	usr_entry
	mov	r2, sp				@ regs
	pabt_helper
 UNWIND(.fnend		)
	/* fall through */
/*
 * This is the return code to user mode for abort handlers
 */
ENTRY(ret_from_exception)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)

/* IAMROOT-12:
 * -------------
 * arch/arm/include/asm/assembler.h
 * tsk: r9
 * why: r8
 */
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user
 UNWIND(.fnend		)
ENDPROC(__pabt_usr)
ENDPROC(ret_from_exception)

	.align	5
__fiq_usr:
	usr_entry trace=0
	kuser_cmpxchg_check
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi
	get_thread_info tsk
	restore_user_regs fast = 0, offset = 0
 UNWIND(.fnend		)
ENDPROC(__fiq_usr)

/*
 * Register switch for ARMv3 and ARMv4 processors
 * r0 = previous task_struct, r1 = previous thread_info, r2 = next thread_info
 * previous and next are guaranteed not to be the same.
 */
ENTRY(__switch_to)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)

/* IAMROOT-12:
 * -------------
 * ip <- prev thread_info()->cpu_context 구조체 주소
 */
	add	ip, r1, #TI_CPU_SAVE

/* IAMROOT-12:
 * -------------
 * 레지스터들을 백업한다.
 */
 ARM(	stmia	ip!, {r4 - sl, fp, sp, lr} )	@ Store most regs on stack
 THUMB(	stmia	ip!, {r4 - sl, fp}	   )	@ Store most regs on stack
 THUMB(	str	sp, [ip], #4		   )
 THUMB(	str	lr, [ip], #4		   )

/* IAMROOT-12:
 * -------------
 * TLS 레지스터
 * r4 <- next thread_info()->tp_value[0] 주소
 * r5 <- next thread_info()->tp_value[1] 주소
 */
	ldr	r4, [r2, #TI_TP_VALUE]
	ldr	r5, [r2, #TI_TP_VALUE + 4]

/* IAMROOT-12:
 * -------------
 * armv7: CONFIG_CPU_USE_DOMAINS를 사용하지 않는다.
 */
#ifdef CONFIG_CPU_USE_DOMAINS
	ldr	r6, [r2, #TI_CPU_DOMAIN]
#endif

/* IAMROOT-12:
 * -------------
 * TLS(TPIDRURW)를 설정한다.
 */
	switch_tls r1, r4, r5, r3, r7
#if defined(CONFIG_CC_STACKPROTECTOR) && !defined(CONFIG_SMP)
	ldr	r7, [r2, #TI_TASK]
	ldr	r8, =__stack_chk_guard
	ldr	r7, [r7, #TSK_STACK_CANARY]
#endif

/* IAMROOT-12:
 * -------------
 * armv7: CONFIG_CPU_USE_DOMAINS를 사용하지 않는다.
 */
#ifdef CONFIG_CPU_USE_DOMAINS
	mcr	p15, 0, r6, c3, c0, 0		@ Set domain register
#endif
	mov	r5, r0

/* IAMROOT-12:
 * -------------
 * r4 <- next thread_info()->cpu_context 구조체 주소
 */
	add	r4, r2, #TI_CPU_SAVE

/* IAMROOT-12:
 * -------------
 * 태스크 전환이 이뤄질 때 호출될 notify block의 callback을 호출한다.
 * (vfp_notifier()가 등록되어 사용된다.)
 */
	ldr	r0, =thread_notify_head
	mov	r1, #THREAD_NOTIFY_SWITCH
	bl	atomic_notifier_call_chain
#if defined(CONFIG_CC_STACKPROTECTOR) && !defined(CONFIG_SMP)
	str	r7, [r8]
#endif
 THUMB(	mov	ip, r4			   )
	mov	r0, r5

/* IAMROOT-12:
 * -------------
 * next thread_info()->cpu_context 주소에 백업되어 있던 레지스터를 
 * 현재 레지스터로 불러온다.(restore)
 */
 ARM(	ldmia	r4, {r4 - sl, fp, sp, pc}  )	@ Load all regs saved previously
 THUMB(	ldmia	ip!, {r4 - sl, fp}	   )	@ Load all regs saved previously
 THUMB(	ldr	sp, [ip], #4		   )
 THUMB(	ldr	pc, [ip]		   )
 UNWIND(.fnend		)
ENDPROC(__switch_to)

	__INIT

/*
 * User helpers.
 *
 * Each segment is 32-byte aligned and will be moved to the top of the high
 * vector page.  New segments (if ever needed) must be added in front of
 * existing ones.  This mechanism should be used only for things that are
 * really small and justified, and not be abused freely.
 *
 * See Documentation/arm/kernel_user_helpers.txt for formal definitions.
 */
 THUMB(	.arm	)

	.macro	usr_ret, reg
#ifdef CONFIG_ARM_THUMB
	bx	\reg
#else
	ret	\reg
#endif
	.endm

	.macro	kuser_pad, sym, size
	.if	(. - \sym) & 3
	.rept	4 - (. - \sym) & 3
	.byte	0
	.endr
	.endif
	.rept	(\size - (. - \sym)) / 4
	.word	0xe7fddef1
	.endr
	.endm

#ifdef CONFIG_KUSER_HELPERS
	.align	5
	.globl	__kuser_helper_start
__kuser_helper_start:

/*
 * Due to the length of some sequences, __kuser_cmpxchg64 spans 2 regular
 * kuser "slots", therefore 0xffff0f80 is not used as a valid entry point.
 */

__kuser_cmpxchg64:				@ 0xffff0f60

#if defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)

	/*
	 * Poor you.  No fast solution possible...
	 * The kernel itself must perform the operation.
	 * A special ghost syscall is used for that (see traps.c).
	 */
	stmfd	sp!, {r7, lr}
	ldr	r7, 1f			@ it's 20 bits
	swi	__ARM_NR_cmpxchg64
	ldmfd	sp!, {r7, pc}
1:	.word	__ARM_NR_cmpxchg64

#elif defined(CONFIG_CPU_32v6K)

	stmfd	sp!, {r4, r5, r6, r7}
	ldrd	r4, r5, [r0]			@ load old val
	ldrd	r6, r7, [r1]			@ load new val
	smp_dmb	arm
1:	ldrexd	r0, r1, [r2]			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eoreqs	r3, r1, r5			@ compare with oldval (2)
	strexdeq r3, r6, r7, [r2]		@ store newval if eq
	teqeq	r3, #1				@ success?
	beq	1b				@ if no then retry
	smp_dmb	arm
	rsbs	r0, r3, #0			@ set returned val and C flag
	ldmfd	sp!, {r4, r5, r6, r7}
	usr_ret	lr

#elif !defined(CONFIG_SMP)

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg64
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle of
	 * the critical sequence.  The same strategy as for cmpxchg is used.
	 */
	stmfd	sp!, {r4, r5, r6, lr}
	ldmia	r0, {r4, r5}			@ load old val
	ldmia	r1, {r6, lr}			@ load new val
1:	ldmia	r2, {r0, r1}			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eoreqs	r3, r1, r5			@ compare with oldval (2)
2:	stmeqia	r2, {r6, lr}			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	ldmfd	sp!, {r4, r5, r6, pc}

	.text
kuser_cmpxchg64_fixup:
	@ Called from kuser_cmpxchg_fixup.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0f60 + (1b - __kuser_cmpxchg64)))
	subs	r8, r4, r7
	rsbcss	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
#if __LINUX_ARM_ARCH__ < 6
	bcc	kuser_cmpxchg32_fixup
#endif
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else
#error "incoherent kernel configuration"
#endif

	kuser_pad __kuser_cmpxchg64, 64

__kuser_memory_barrier:				@ 0xffff0fa0
	smp_dmb	arm
	usr_ret	lr

	kuser_pad __kuser_memory_barrier, 32

__kuser_cmpxchg:				@ 0xffff0fc0

#if defined(CONFIG_NEEDS_SYSCALL_FOR_CMPXCHG)

	/*
	 * Poor you.  No fast solution possible...
	 * The kernel itself must perform the operation.
	 * A special ghost syscall is used for that (see traps.c).
	 */
	stmfd	sp!, {r7, lr}
	ldr	r7, 1f			@ it's 20 bits
	swi	__ARM_NR_cmpxchg
	ldmfd	sp!, {r7, pc}
1:	.word	__ARM_NR_cmpxchg

#elif __LINUX_ARM_ARCH__ < 6

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle
	 * of the critical sequence.  To prevent this, code is added to
	 * the IRQ and data abort exception handlers to set the pc back
	 * to the beginning of the critical section if it is found to be
	 * within that critical section (see kuser_cmpxchg_fixup).
	 */
1:	ldr	r3, [r2]			@ load current val
	subs	r3, r3, r0			@ compare with oldval
2:	streq	r1, [r2]			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	usr_ret	lr

	.text
kuser_cmpxchg32_fixup:
	@ Called from kuser_cmpxchg_check macro.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0fc0 + (1b - __kuser_cmpxchg)))
	subs	r8, r4, r7
	rsbcss	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else

	smp_dmb	arm
1:	ldrex	r3, [r2]
	subs	r3, r3, r0
	strexeq	r3, r1, [r2]
	teqeq	r3, #1
	beq	1b
	rsbs	r0, r3, #0
	/* beware -- each __kuser slot must be 8 instructions max */
	ALT_SMP(b	__kuser_memory_barrier)
	ALT_UP(usr_ret	lr)

#endif

	kuser_pad __kuser_cmpxchg, 32

__kuser_get_tls:				@ 0xffff0fe0
	ldr	r0, [pc, #(16 - 8)]	@ read TLS, set in kuser_get_tls_init
	usr_ret	lr
	mrc	p15, 0, r0, c13, c0, 3	@ 0xffff0fe8 hardware TLS code
	kuser_pad __kuser_get_tls, 16
	.rep	3
	.word	0			@ 0xffff0ff0 software TLS value, then
	.endr				@ pad up to __kuser_helper_version

__kuser_helper_version:				@ 0xffff0ffc
	.word	((__kuser_helper_end - __kuser_helper_start) >> 5)

	.globl	__kuser_helper_end
__kuser_helper_end:

#endif

 THUMB(	.thumb	)

/*
 * Vector stubs.
 *
 * This code is copied to 0xffff1000 so we can use branches in the
 * vectors, rather than ldr's.  Note that this code must not exceed
 * a page size.
 *
 * Common stub entry macro:
 *   Enter in IRQ mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 *
 * SP points to a minimal amount of processor-private memory, the address
 * of which is copied into r0 for the mode specific abort handler.
 */

/* IAMROOT-12AB:
 * -------------
 * 벡터는 다음 위치에 존재하고 벡터 스터브는 그 다음 페이지에 위치한다.
 *    - 0xffff_0000에 위치하는 high vector
 *    - 0x0000_0000에 이치하는 low vector 
 *
 * 몇 개의 인터럽트 핸들러가 사용하는 디스패처 매크로
 * name: 
 *	- irq 
 *	- dabt
 *	- pabt 
 *	- und 
 *	- fiq
 * mode:
 *	- ABT_MODE 
 *	- IRQ_MODE 
 *	- FIQ_MODE 
 *	- UND_MODE
 * correction:
 *	- 핸들러 처리 후 복귀할 주소 교정(0, 4, 8)
 */
	.macro	vector_stub, name, mode, correction=0
	.align	5

vector_\name:

/* IAMROOT-12:
 * -------------
 * vector_stub 매크로 
 *	12 byte의 임시 stack에 보관할 내용은 다음과 같다.
 *		- r0   (현재 모드 r0로 scratch되므로 백업해둔다)
 *		- lr   (exception되기 전 pc)
 *		- spsr (exception되기 전 cpsr)
 */
	.if \correction
	sub	lr, lr, #\correction
	.endif

	@
	@ Save r0, lr_<exception> (parent PC) and spsr_<exception>
	@ (parent CPSR)
	@

/* IAMROOT-12:
 * -------------
 * stack 증가 없이 r0, lr(기존 pc) 레지스터를 보관한다.
 *
 *            lr(기존 모드 pc)
 *	sp -> r0
 */
	stmia	sp, {r0, lr}		@ save r0, lr 

/* IAMROOT-12:
 * -------------
 * 기존 모드의 cpsr -> sp+8 위치에 보관
 * 현재 exception stub로 오기 전 cpsr->spsr로 복제되었었다.
 * (exception 발생 시 arm 아키텍처가 자동으로 수행)
 *
 *	      spsr(기존 모드 cpsr)
 *	      lr(기존 모드 pc)
 *	sp -> r0
 */
	mrs	lr, spsr
	str	lr, [sp, #8]		@ save spsr

	@
	@ Prepare for SVC32 mode.  IRQs remain disabled.
	@

/* IAMROOT-12:
 * -------------
 * 현재 모드를 svc 모드로 바꾼다. 
 *	(current mode(IRQ) ^ stub mode(IRQ) ^ mode(SVC) = mode SVC)
 *	     10010         ^    (    10010  ^    10011) = 
 *	     10010         ^    (        00001        ) = 10011
 *
 * 5 bit 비트클리어 명령 후 or 명령으로 5 bit를 더하면 2개의 
 * 인스트럭션이 사용되므로 1 개의 인스트럭션으로 처리하기 위해 
 * eor 명령을 사용하였다.
 *
 * exception 발생 시 arm 아키텍처가 irq, fiq 비트를 자동으로 cpsr에 mask 시킨다.
 */
	mrs	r0, cpsr
	eor	r0, r0, #(\mode ^ SVC_MODE | PSR_ISETSTATE)
	msr	spsr_cxsf, r0

	@
	@ the branch table must immediately follow this code
	@

/* IAMROOT-12:
 * -------------
 * spsr에서 mode 5bit 중 실제 사용하는 4bit만 사용하여 인덱스를 산출한다.
 * pc는 항상 instruction pipe에서 실행되는 위치 + 2 word (8 byte)위치를 
 * 가리키므로 이 매크로의 뒷 부분에는 항상 16개의 테이블이 있는 것을 
 * 보장하여야 한다.
 */
	and	lr, lr, #0x0f
 THUMB(	adr	r0, 1f			)
 THUMB(	ldr	lr, [r0, lr, lsl #2]	)
	mov	r0, sp
 ARM(	ldr	lr, [pc, lr, lsl #2]	)
	movs	pc, lr			@ branch to handler in SVC mode
ENDPROC(vector_\name)

	.align	2
	@ handler addresses follow this label
1:
	.endm

/* IAMROOT-12AB:
 * -------------
 * .stub 섹션에 처음에는 소프트인터럽트 핸들러의 주소가 위치한다.
 * 그 뒤로 8개의 exception 핸들러들이 위치한다.
 *	- SWI (jump만 존재하고 다른 소스에 위치)
 *	- 리셋 핸들러 
 *	- IRQ 디스패처 
 *	- Data Abort 디스패처
 *	- Prefetch 디스패처
 *	- Undefined Instruction 디스패처 
 *	- (Reserved)
 *	- FIQ 디스패처
 */
	.section .stubs, "ax", %progbits
__stubs_start:
	@ This must be the first word
	.word	vector_swi

vector_rst:

/* IAMROOT-12:
 * -------------
 * 리셋 exception이 발생한 경우 SYS_ERROR0 syscall을 호출한다.
 */

 ARM(	swi	SYS_ERROR0	)
 THUMB(	svc	#0		)
 THUMB(	nop			)
	b	vector_und

/*
 * Interrupt dispatcher
 */

/* IAMROOT-12AB:
 * -------------
 * IRQ 디스패처
 */
	vector_stub	irq, IRQ_MODE, 4

	.long	__irq_usr			@  0  (USR_26 / USR_32)
	.long	__irq_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__irq_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__irq_svc			@  3  (SVC_26 / SVC_32)
	.long	__irq_invalid			@  4
	.long	__irq_invalid			@  5
	.long	__irq_invalid			@  6
	.long	__irq_invalid			@  7
	.long	__irq_invalid			@  8
	.long	__irq_invalid			@  9
	.long	__irq_invalid			@  a
	.long	__irq_invalid			@  b
	.long	__irq_invalid			@  c
	.long	__irq_invalid			@  d
	.long	__irq_invalid			@  e
	.long	__irq_invalid			@  f

/*
 * Data abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */

/* IAMROOT-12AB:
 * -------------
 * Data Abort 디스패처
 */
	vector_stub	dabt, ABT_MODE, 8

	.long	__dabt_usr			@  0  (USR_26 / USR_32)
	.long	__dabt_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__dabt_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__dabt_svc			@  3  (SVC_26 / SVC_32)
	.long	__dabt_invalid			@  4
	.long	__dabt_invalid			@  5
	.long	__dabt_invalid			@  6
	.long	__dabt_invalid			@  7
	.long	__dabt_invalid			@  8
	.long	__dabt_invalid			@  9
	.long	__dabt_invalid			@  a
	.long	__dabt_invalid			@  b
	.long	__dabt_invalid			@  c
	.long	__dabt_invalid			@  d
	.long	__dabt_invalid			@  e
	.long	__dabt_invalid			@  f

/*
 * Prefetch abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */

/* IAMROOT-12:
 * -------------
 * pre-fetch abort exception stub 
 *	
 */
	vector_stub	pabt, ABT_MODE, 4

	.long	__pabt_usr			@  0 (USR_26 / USR_32)
	.long	__pabt_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__pabt_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__pabt_svc			@  3 (SVC_26 / SVC_32)
	.long	__pabt_invalid			@  4
	.long	__pabt_invalid			@  5
	.long	__pabt_invalid			@  6
	.long	__pabt_invalid			@  7
	.long	__pabt_invalid			@  8
	.long	__pabt_invalid			@  9
	.long	__pabt_invalid			@  a
	.long	__pabt_invalid			@  b
	.long	__pabt_invalid			@  c
	.long	__pabt_invalid			@  d
	.long	__pabt_invalid			@  e
	.long	__pabt_invalid			@  f

/*
 * Undef instr entry dispatcher
 * Enter in UND mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 */
	vector_stub	und, UND_MODE

	.long	__und_usr			@  0 (USR_26 / USR_32)
	.long	__und_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__und_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__und_svc			@  3 (SVC_26 / SVC_32)
	.long	__und_invalid			@  4
	.long	__und_invalid			@  5
	.long	__und_invalid			@  6
	.long	__und_invalid			@  7
	.long	__und_invalid			@  8
	.long	__und_invalid			@  9
	.long	__und_invalid			@  a
	.long	__und_invalid			@  b
	.long	__und_invalid			@  c
	.long	__und_invalid			@  d
	.long	__und_invalid			@  e
	.long	__und_invalid			@  f

	.align	5

/*=============================================================================
 * Address exception handler
 *-----------------------------------------------------------------------------
 * These aren't too critical.
 * (they're not supposed to happen, and won't happen in 32-bit data mode).
 */

vector_addrexcptn:
	b	vector_addrexcptn

/*=============================================================================
 * FIQ "NMI" handler
 *-----------------------------------------------------------------------------
 * Handle a FIQ using the SVC stack allowing FIQ act like NMI on x86
 * systems.
 */
	vector_stub	fiq, FIQ_MODE, 4

	.long	__fiq_usr			@  0  (USR_26 / USR_32)
	.long	__fiq_svc			@  1  (FIQ_26 / FIQ_32)
	.long	__fiq_svc			@  2  (IRQ_26 / IRQ_32)
	.long	__fiq_svc			@  3  (SVC_26 / SVC_32)
	.long	__fiq_svc			@  4
	.long	__fiq_svc			@  5
	.long	__fiq_svc			@  6
	.long	__fiq_abt			@  7
	.long	__fiq_svc			@  8
	.long	__fiq_svc			@  9
	.long	__fiq_svc			@  a
	.long	__fiq_svc			@  b
	.long	__fiq_svc			@  c
	.long	__fiq_svc			@  d
	.long	__fiq_svc			@  e
	.long	__fiq_svc			@  f

	.globl	vector_fiq_offset
	.equ	vector_fiq_offset, vector_fiq

/* IAMROOT-12AB:
 * -------------
 * 8개의 exception 벡터 중 소프트인터럽트의 핸들러는 원거리에 위치하므로
 * __vectors_start+0x1000에 위치한 주소(소프트인터럽트 핸들러 주소가 담긴)로
 * 이동하게 한다.
 *
 * 나머지 7개의 벡터는 branch 코드를 직접 사용하여 해당 핸들러로 jump 한다.
 * 해당 핸들러는 근거리(다음 페이지)에 위치한다. 
 *
 * 1) 리셋
 * 2) undefined instruction 
 * 3) swi 
 * 4) pre-fetch abort 
 * 5) data abort 
 * 6) n/a
 * 7) irq 
 * 8) fiq
 */
	.section .vectors, "ax", %progbits
__vectors_start:
	W(b)	vector_rst
	W(b)	vector_und
	W(ldr)	pc, __vectors_start + 0x1000
	W(b)	vector_pabt
	W(b)	vector_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_irq
	W(b)	vector_fiq

	.data

/* IAMROOT-12:
 * -------------
 * 부트업 타임에 SCTLR 값이 저장된다.
 */
	.globl	cr_alignment
cr_alignment:
	.space	4

#ifdef CONFIG_MULTI_IRQ_HANDLER
	.globl	handle_arch_irq
handle_arch_irq:

/* IAMROOT-12:
 * -------------
 * irq 핸들러 함수의 주소를 가리킨다. (런타임에 결정)
 */
	.space	4
#endif
