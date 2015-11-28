/*
 *  linux/arch/arm/kernel/head-common.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <asm/assembler.h>

/* IAMROOT-12A:
 * ------------
 * ATAG_CORE_SIZE는 공통헤더(2개 word) + ATAG_CORE(3개 word) = 5. 
 * (바이트로 변환 시 20바이트)
 */
#define ATAG_CORE 0x54410001
#define ATAG_CORE_SIZE ((2*4 + 3*4) >> 2)
#define ATAG_CORE_SIZE_EMPTY ((2*4) >> 2)

/* IAMROOT-12A:
 * ------------
 * 디바이스 트리르 알리는 매직넘버.
 * 바이너리 디바이스 트리는 빅엔디안형태로 사용되므로
 * 아키첵처 엔디안에 따라 변환하여 인식을 하여야 한다.
 */
#ifdef CONFIG_CPU_BIG_ENDIAN
#define OF_DT_MAGIC 0xd00dfeed
#else
#define OF_DT_MAGIC 0xedfe0dd0 /* 0xd00dfeed in big-endian */
#endif

/*
 * Exception handling.  Something went wrong and we can't proceed.  We
 * ought to tell the user, but since we don't have any guarantee that
 * we're even running on the right architecture, we do virtually nothing.
 *
 * If CONFIG_DEBUG_LL is set we try to print out something about the error
 * and hope for the best (useful if bootloader fails to pass a proper
 * machine ID for example).
 */
	__HEAD

/* Determine validity of the r2 atags pointer.  The heuristic requires
 * that the pointer be aligned, in the first 16k of physical RAM and
 * that the ATAG_CORE marker is first and present.  If CONFIG_OF_FLATTREE
 * is selected, then it will also accept a dtb pointer.  Future revisions
 * of this function may be more lenient with the physical address and
 * may also be able to move the ATAGS block if necessary.
 *
 * Returns:
 *  r2 either valid atags pointer, valid dtb pointer, or zero
 *  r5, r6 corrupted
 */


/* IAMROOT-12A:
 * ------------
 * 체크1) align 체크하여 align되어 있지 않으면 에러로 점프.
 */
__vet_atags:
	tst	r2, #0x3			@ aligned?
	bne	1f

/* IAMROOT-12A:
 * ------------
 * 체크2) dtb magic number가 검출되면 정상이라 판단하고 리턴.
 */
	ldr	r5, [r2, #0]
#ifdef CONFIG_OF_FLATTREE
	ldr	r6, =OF_DT_MAGIC		@ is it a DTB?
	cmp	r5, r6
	beq	2f
#endif

/* IAMROOT-12A:
 * ------------
 * 체크3) ATAG_CORE가 처음에 오는지 사이즈 및 ATAG_CORE 태그코드 매치 확인하여
 *        맞으면 ATAG가 맞다고 판단하고 리턴.
 * ATAG_CORE_SIZE: 5
 * ATAG_CORE: 0x54410001
 */
	cmp	r5, #ATAG_CORE_SIZE		@ is first tag ATAG_CORE?
	cmpne	r5, #ATAG_CORE_SIZE_EMPTY
	bne	1f
	ldr	r5, [r2, #4]
	ldr	r6, =ATAG_CORE
	cmp	r5, r6
	bne	1f

2:	ret	lr				@ atag/dtb pointer is ok

/* IAMROOT-12A:
 * ------------
 * r2=0을 담고 리턴(에러)
 */

1:	mov	r2, #0
	ret	lr
ENDPROC(__vet_atags)

/*
 * The following fragment of code is executed with the MMU on in MMU mode,
 * and uses absolute addresses; this is not position independent.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags/dtb pointer
 *  r9  = processor ID
 */
	__INIT
__mmap_switched:

/* IAMROOT-12A:
 * ------------
 * r4: __data_loc     <- 커널 데이터 섹션 시작 가상주소 
 * r5: _sdata         <- 위와 동일하지만 XIP 커널의 경우 RAM영역으로 바뀜
 *                       라즈베리파이2: 0x8000_8000 <- 커널빌드시 결정
 *                       vmlinux.lds.S
 * r6: __bss_start    <- .bss 섹션 시작
 * r7: _end           <- .bss 섹션 끝
 */
	adr	r3, __mmap_switched_data

/* IAMROOT-12A:
 * ------------
 * __data_loc 와 _sdata의 주소가 다른 경우 데이터 섹션을 _sdata 주소로 옮긴다.
 * 즉, XIP 커널이 동작중인 경우 데이터 영역을 kernel space의 시작부분 + TEXT_OFFSET
 * 위치로 옮긴다. (data영역만 ROM -> RAM으로 옮긴다)
 */
	ldmia	r3!, {r4, r5, r6, r7}
	cmp	r4, r5				@ Copy data segment if needed
1:	cmpne	r5, r6
	ldrne	fp, [r4], #4
	strne	fp, [r5], #4
	bne	1b

/* IAMROOT-12A:
 * ------------
 * .bss 영역을 0으로 초기화한다.
 */
	mov	fp, #0				@ Clear BSS (and zero fp)
1:	cmp	r6, r7
	strcc	fp, [r6],#4
	bcc	1b

/* IAMROOT-12A:
 * ------------
 * start_kernel로 점프하기 전에 몇 개의 레지스터를 __mmap_switched_data 
 * 스트럭쳐의 일부에 저장한다.
 *
 * cr_alignment가 가리키는 곳에 SCTLR 값이 들어간다.
 */
 ARM(	ldmia	r3, {r4, r5, r6, r7, sp})
 THUMB(	ldmia	r3, {r4, r5, r6, r7}	)
 THUMB(	ldr	sp, [r3, #16]		)
	str	r9, [r4]			@ Save processor ID
	str	r1, [r5]			@ Save machine type
	str	r2, [r6]			@ Save atags pointer
	cmp	r7, #0
	strne	r0, [r7]			@ Save control register values
	b	start_kernel
ENDPROC(__mmap_switched)

	.align	2
	.type	__mmap_switched_data, %object
__mmap_switched_data:
	.long	__data_loc			@ r4
	.long	_sdata				@ r5
	.long	__bss_start			@ r6
	.long	_end				@ r7
	.long	processor_id			@ r4
	.long	__machine_arch_type		@ r5
	.long	__atags_pointer			@ r6
#ifdef CONFIG_CPU_CP15
	.long	cr_alignment			@ r7
#else
	.long	0				@ r7
#endif
	.long	init_thread_union + THREAD_START_SP @ sp
	.size	__mmap_switched_data, . - __mmap_switched_data

/*
 * This provides a C-API version of __lookup_processor_type
 */
ENTRY(lookup_processor_type)
	stmfd	sp!, {r4 - r6, r9, lr}
	mov	r9, r0
	bl	__lookup_processor_type
	mov	r0, r5
	ldmfd	sp!, {r4 - r6, r9, pc}
ENDPROC(lookup_processor_type)

	__FINIT
	.text

/*
 * Read processor ID register (CP#15, CR0), and look up in the linker-built
 * supported processor list.  Note that we can't use the absolute addresses
 * for the __proc_info lists since we aren't running with the MMU on
 * (and therefore, we are not in the correct address space).  We have to
 * calculate the offset.
 *
 *	r9 = cpuid
 * Returns:
 *	r3, r4, r6 corrupted
 *	r5 = proc_info pointer in physical address space
 *	r9 = cpuid (preserved)
 */

/* IAMROOT-12A:
 * ------------
 * 해당 프로세서에 적합한 구조체의 주소가 r5에 담겨온다.
 *
 * r4: 컴파일시 가상 주소
 * r5: __proc_info_begin
 * r6: __proc_info_end
 * r3: offset(물리주소 - 가상주소) - 라즈베리파이2: 0x8000_0000
 *
 * .init.proc.info 섹션에 ARM CPU 중 빌드시 사용한 아키텍처에 관련된 프로세스
 * 정보가 모두 모여 있음.(라즈베리파이2: ARMv7 관련된 것만 약 500여 바이트)
 *
 * arch/arm/mm/proc-v7.S 화일에 라즈베리파이2가 사용하는 프로세스 구조가 담겨 있음.
 *                       (__v7_ca7mp_proc_info: 위치)
 *
 * 아래에 arch/arm/kernel/vmlinux.lds 화일에서 관련된 부분:
 *	.init.proc.info : { 
 *	. = ALIGN(4); __proc_info_begin = .; *(.proc.info.init) __proc_info_end = .;
 *	}
 */
__lookup_processor_type:
	adr	r3, __lookup_processor_type_data
	ldmia	r3, {r4 - r6}
	sub	r3, r3, r4			@ get offset between virt&phys
	add	r5, r5, r3			@ convert virt addresses to
	add	r6, r6, r3			@ physical address space
1:	ldmia	r5, {r3, r4}			@ value, mask
	and	r4, r4, r9			@ mask wanted bits
	teq	r3, r4
	beq	2f
	add	r5, r5, #PROC_INFO_SZ		@ sizeof(proc_info_list)
	cmp	r5, r6
	blo	1b
	mov	r5, #0				@ unknown processor
2:	ret	lr
ENDPROC(__lookup_processor_type)

/*
 * Look in <asm/procinfo.h> for information about the __proc_info structure.
 */

/* IAMROOT-12A:
 * ------------
 * 12바이트의 프로세서 위치 정보를 담고 있는 객체
 */
	.align	2
	.type	__lookup_processor_type_data, %object
__lookup_processor_type_data:
	.long	.
	.long	__proc_info_begin
	.long	__proc_info_end
	.size	__lookup_processor_type_data, . - __lookup_processor_type_data

__error_lpae:
#ifdef CONFIG_DEBUG_LL
	adr	r0, str_lpae
	bl 	printascii
	b	__error
str_lpae: .asciz "\nError: Kernel with LPAE support, but CPU does not support LPAE.\n"
#else
	b	__error
#endif
	.align
ENDPROC(__error_lpae)

__error_p:
#ifdef CONFIG_DEBUG_LL
	adr	r0, str_p1
	bl	printascii
	mov	r0, r9
	bl	printhex8
	adr	r0, str_p2
	bl	printascii
	b	__error
str_p1:	.asciz	"\nError: unrecognized/unsupported processor variant (0x"
str_p2:	.asciz	").\n"
	.align
#endif
ENDPROC(__error_p)

__error:
#ifdef CONFIG_ARCH_RPC
/*
 * Turn the screen red on a error - RiscPC only.
 */
	mov	r0, #0x02000000
	mov	r3, #0x11
	orr	r3, r3, r3, lsl #8
	orr	r3, r3, r3, lsl #16
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
#endif
1:	mov	r0, r0
	b	1b
ENDPROC(__error)
