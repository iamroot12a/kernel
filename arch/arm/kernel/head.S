/*
 *  linux/arch/arm/kernel/head.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Kernel startup code for all 32-bit CPUs
 */

/*
 * 참여자:
 *     문영일 - jakeisname@gmail.com
 *     이벽산 - lbyeoksan@gmail.com
 *     한대근 - dev.daegeunhan@gmail.com
 *     구동수 - cloudevops@naver.com
 *     유계성 - gsryu99@gmail.com 
 *     전성윤 - roland.korea@gmail.com
 *     김관호 - junku80@gmail.com
 *     최영민 - jiggly2k@gmail.com
 *     서태원 - eilly68@gmail.com
 *     한상종 - sjhan00000@gmail.com
 *     김형일 - khi8660@naver.com
 *     김종철 - jongchul.kim@gmail.com
 *     김상덕 - ksd3148@gmail.com
 */

#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/pgtable.h>

#if defined(CONFIG_DEBUG_LL) && !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif

/*
 * swapper_pg_dir is the virtual address of the initial page table.
 * We place the page tables 16K below KERNEL_RAM_VADDR.  Therefore, we must
 * make sure that KERNEL_RAM_VADDR is correctly set.  Currently, we expect
 * the least significant 16 bits to be 0x8000, but we could probably
 * relax this restriction to KERNEL_RAM_VADDR >= PAGE_OFFSET + 0x4000.
 */

/* IAMROOT-12A:
 * ------------
 * 컴파일 시 KERNEL_RAM_VADDR의 하위 16비트가 0x8000이 아니면 에러 발생.
 * 라즈베리파이2: PAGE_OFFSET = CONFIG_PAGE_OFFSET = 0x8000_0000
 *                TEXT_OFFSET = 0x8000
 *                KERNEL_RAM_VADDR = 0x8000_8000
 */
#define KERNEL_RAM_VADDR	(PAGE_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif

/* IAMROOT-12A:
 * ------------
 * CONFIG_ARM_LPAE를 지원하는 칩셋에서 40bit 어드레스를 지원한다.
 * 페이지 테이블 사이즈가 0x5000을 요구한다.
 * 라즈베리파이2: PG_DIR_SIZE = 0x4000
 */
#ifdef CONFIG_ARM_LPAE
	/* LPAE requires an additional page for the PGD */
#define PG_DIR_SIZE	0x5000
#define PMD_ORDER	3
#else
#define PG_DIR_SIZE	0x4000
#define PMD_ORDER	2
#endif

/* IAMROOT-12A:
 * ------------
 * swapper_pg_dir: 페이지테이블의 위치는 커널가상주소 - 페이지테이블 사이즈 
 * 라즈베리파이2: 0x8000_4000
 */
	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE

/* IAMROOT-12A:
 * ------------
 *  pgtbl매크로는 물리메모리 주소로 페이지테이블의 위치를 계산.
 *  pgtbl   rd=r4(0x0000_4000), phys=r8(0x0000_0000)  
 *          add r4, r8, #0x8000
 *          sub r4, r4, #0x4000
 */
	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET
	sub	\rd, \rd, #PG_DIR_SIZE
	.endm

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * This is normally called from the decompressor code.  The requirements
 * are: MMU = off, D-cache = off, I-cache = dont care, r0 = 0,
 * r1 = machine nr, r2 = atags or dtb pointer.
 *
 * This code is mostly position independent, so if you link the kernel at
 * 0xc0008000, you call this at __pa(0xc0008000).
 *
 * See linux/arch/arm/tools/mach-types for the complete list of machine
 * numbers for r1.
 *
 * We're trying to keep crap to a minimum; DO NOT add any machine specific
 * crap here - that's what the boot loader (or in extreme, well justified
 * circumstances, zImage) is for.
 */

/* IAMROOT-12A:
 * ------------
 * .arm: 컴파일러에게 THUMB가 아닌 ARM 코드(32bit) 시작을 알림.
 *       실제 instruction은 없음.
 */

	.arm

/* IAMROOT-12A:
 * ------------
 * __HEAD: .head.text 섹션을 선언
 * ENTRY(stext): lable, globl, align 등이 포함된 매크로 기억. 
 */
	__HEAD
ENTRY(stext)

/* IAMROOT-12A:
 * ------------
 * ARM_BE8(): ARM 빅엔디안이 지원되면 설정을하여 빌드할 수도 있음.
 * setend be: 빅엔디안으로 CPU가 동작.  (cpsr BE 비트를 1로 설정)
 * 라즈베리파이2: 리틀엔디안(ARM_BE8 옵션 사용하지 않음)으로 동작
 */
 ARM_BE8(setend	be )			@ ensure we are in BE8 mode

 THUMB(	adr	r9, BSYM(1f)	)	@ Kernel is always entered in ARM.
 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
 THUMB(	.thumb			)	@ switch to Thumb now.
 THUMB(1:			)

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install
#endif


/* IAMROOT-12A:
 * ------------
 * svc 모드가 아닌 경우 svc 모드로 진입할 수 있게 한다.
 * fiq & irq bit를 disable한다(인터럽트를 허용하지 않도록)
 */
	@ ensure svc mode and all interrupts masked
	safe_svcmode_maskall r9

/* IAMROOT-12A:
 * ------------
 * get MIDR: 프로세서 id를 읽어온다.
 * __lookup_processor_type 함수를 다녀오면 r5에 해당 프로세서의 
 * 구조체 물리주소를 담아온다.
 */
	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type		@ r5=procinfo r9=cpuid
	movs	r10, r5				@ invalid processor (r5=0)?
 THUMB( it	eq )				@ force fixup-able long branch encoding
	beq	__error_p			@ yes, error 'p'

#ifdef CONFIG_ARM_LPAE
	mrc	p15, 0, r3, c0, c1, 4		@ read ID_MMFR0
	and	r3, r3, #0xf			@ extract VMSA support
	cmp	r3, #5				@ long-descriptor translation table format?
 THUMB( it	lo )				@ force fixup-able long branch encoding
	blo	__error_lpae			@ only classic page table format
#endif

#ifndef CONFIG_XIP_KERNEL
/* IAMROOT-12A:
 * ------------
 * r4: 레이블 2f가 가리키는 주소의 값(빌드시 가상주소) 
 * r8: 레이블 2f+4가 가리키는 주소의 값(PAGE_OFFSET: 0x8000_0000)
 *
 * r4(phys_offset): 물리주소 - 가상주소를 뺀다
 * r8: 적용된 물리 시작주소(0x0000_0000)
 */
	adr	r3, 2f
	ldmia	r3, {r4, r8}
	sub	r4, r3, r4			@ (PHYS_OFFSET - PAGE_OFFSET)
	add	r8, r8, r4			@ PHYS_OFFSET
#else

/* IAMROOT-12A:
 * ------------
 * XIP 커널에서는 코드가 ROM(or Nor flash)에서 동작하므로 adr 방식으로 물리램 주소를
 * 알아올 수 없어서 직접 PLAT_PHYS_OFFSET에 값을 읽어와서 대입
 */
	ldr	r8, =PLAT_PHYS_OFFSET		@ always constant in this case
#endif

	/*
	 * r1 = machine no, r2 = atags or dtb,
	 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
	 */

/* IAMROOT-12A:
 * ------------
 * atag또는 dtb가 유효한지 확인한다. 유효하지 않으면 r2=0
 */
	bl	__vet_atags

/* IAMROOT-12A:
 * ------------
 * SMP(Multi core)코드가 UP(Uni core)에서 동작시 치환해주는 루틴 
 * CONFIG_SMP_ON_UP: SMP 커널이 UP(Uni core)에서 동작할 수 있도록 지원하는 설정
 */
#ifdef CONFIG_SMP_ON_UP
	bl	__fixup_smp
#endif

/* IAMROOT-12A:
 * ------------
 * 커널 빌드 시 설정된 물리메모리의 시작위치가 실제 커널 구동 시 물리메모리의 
 * 시작위치가 서로 다를 수 있기 때문에 pv_offset를 다시 갱신하고 가상메모리와
 * 물리메모리의 주소 변환 함수를 사용하는 코드를 patch하기 위해 필요한 루틴 
 * - 디바이스트리 등의 사용으로 빌드된 커널을 재사용하고 메모리 주소 위치만
 *   달라진 시스템에서 구동하기 위해 사용된다.
 */

#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
	bl	__fixup_pv_table
#endif
	bl	__create_page_tables

	/*
	 * The following calls CPU specific code in a position independent
	 * manner.  See arch/arm/mm/proc-*.S for details.  r10 = base of
	 * xxx_proc_info structure selected by __lookup_processor_type
	 * above.  On return, the CPU will be ready for the MMU to be
	 * turned on, and r0 will hold the CPU control register value.
	 */

/* IAMROOT-12A:
 * ------------
 * mmu가 켜진 후 실행될 가상 주소를 미리 r13에 담아두고
 * #PROCINFO_INITFUNC이 가리키는 해당 프로세서의 초기화 루틴을 수행한다.
 *    -라즈베리파이2: __v7_ca7mp_setup:
 * 그런 후 __enable_mmu의 루틴을 실행하고 ret r13에 의해 가상 주소로 
 * 직접 jump 한다.
 *
 * ../mm/proc-v7.S 
 *
 * __enable_mmu를 수행후 처음으로 가상커널 주소의 __mmap_switched로 이동한다.
 * 기존에 사용해왔던 adr로 접근하지 않고 ldr로 접근함.
 */
	ldr	r13, =__mmap_switched		@ address to jump to after
						@ mmu has been enabled
	adr	lr, BSYM(1f)			@ return (PIC) address
	mov	r8, r4				@ set TTBR1 to swapper_pg_dir
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	ret	r12				)
1:	b	__enable_mmu
ENDPROC(stext)
	.ltorg
#ifndef CONFIG_XIP_KERNEL
2:	.long	.
	.long	PAGE_OFFSET
#endif

/*
 * Setup the initial page tables.  We only setup the barest
 * amount which are required to get the kernel running, which
 * generally means mapping in the kernel code.
 *
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 *
 * Returns:
 *  r0, r3, r5-r7 corrupted
 *  r4 = page table (see ARCH_PGD_SHIFT in asm/memory.h)
 */
__create_page_tables:

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: r4=0x0000_4000, r8=0x0000_0000
 */
	pgtbl	r4, r8				@ page table address

	/*
	 * Clear the swapper page table
	 */

/* IAMROOT-12A:
 * ------------
 * 페이지테이블을 0으로 초기화한다.
 * 라즈베리파이2: 0x0000_4000 ~ 0x0000_07ff까지를 0으로 초기화.
 */
	mov	r0, r4
	mov	r3, #0
	add	r6, r0, #PG_DIR_SIZE
1:	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	teq	r0, r6
	bne	1b

#ifdef CONFIG_ARM_LPAE
	/*
	 * Build the PGD table (first level) to point to the PMD table. A PGD
	 * entry is 64-bit wide.
	 */
	mov	r0, r4
	add	r3, r4, #0x1000			@ first PMD table address
	orr	r3, r3, #3			@ PGD block type
	mov	r6, #4				@ PTRS_PER_PGD
	mov	r7, #1 << (55 - 32)		@ L_PGD_SWAPPER
1:
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4			@ set top PGD entry bits
	str	r3, [r0], #4			@ set bottom PGD entry bits
#else
	str	r3, [r0], #4			@ set bottom PGD entry bits
	str	r7, [r0], #4			@ set top PGD entry bits
#endif
	add	r3, r3, #0x1000			@ next PMD table
	subs	r6, r6, #1
	bne	1b

	add	r4, r4, #0x1000			@ point to the PMD tables
#ifdef CONFIG_CPU_ENDIAN_BE8
	add	r4, r4, #4			@ we only write the bottom word
#endif
#endif

/* IAMROOT-12A:
 * ------------
 * 해당 프로세서에 설정한 mm_mmuflags정보 가져오기
 *
 * r10: 프로세서타입에 따른 정보.
 *      라즈베리파이2: __v7_ca7mp_proc_info 라벨
 * #PROCINFO_MM_MMUFLAGS는 위 프로세스 구조체 멤버 중 mm_mmuflags의 위치(8)
 * r7: PMD_TYPE_SECT     |   <- 섹션타입
 *     PMD_SECT_AP_WRITE |   <- write
 *     PMD_SECT_AP_READ  |   <- read
 *     PMD_SECT_AF       |   <- 
 *     PMD_FLAGS_SMP         <- SMP
 */
	ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS] @ mm_mmuflags

	/*
	 * Create identity mapping to cater for __enable_mmu.
	 * This identity mapping will be removed by paging_init().
	 */


/* IAMROOT-12A:
 * ------------
 * __turn_mmu_on 함수의 매핑(1:1 VA=PA(identity mapping))
 *
 * __turn_mmu_on ~ __turn_mmu_on_end에는 mmu를 켜서 가상주소의 커널로
 * 넘어가는 코드가 담겨있는데 mmu를 켜기 전에 가상주소와 물리메모리의
 * 주소가 같은 1:1 매핑(identity mapping)을 한다. CPU가 가상 커널 주소로
 * 리턴(jump)하기 전까지는 물리메모리의 위치에 해당하는 가상주소가
 * 동작중이므로 완전히 커널이 위치한 가상주소로 스위칭되기 전까지 1-2개의
 * 섹션이 필요하여 매핑한다.
 */
	adr	r0, __turn_mmu_on_loc
	ldmia	r0, {r3, r5, r6}
	sub	r0, r0, r3			@ virt->phys offset
	add	r5, r5, r0			@ phys __turn_mmu_on
	add	r6, r6, r0			@ phys __turn_mmu_on_end
	mov	r5, r5, lsr #SECTION_SHIFT
	mov	r6, r6, lsr #SECTION_SHIFT

/* IAMROOT-12A:
 * ------------
 * __turn_mmu_on이 시작하는 섹션(31..20비트)과 위에서 읽은 
 * mm_mmuflags(19..0비트)를 orr 시켜 계산된 테이블 엔트리에 저장한다.
 */
1:	orr	r3, r7, r5, lsl #SECTION_SHIFT	@ flags + kernel base

/* IAMROOT-12A:
 * ------------
 * 페이지테이블+r5(__turn_mmu_on~__turn_mmu_on_end의 1M 단위의 인덱스*4) <- r3
 */
	str	r3, [r4, r5, lsl #PMD_ORDER]	@ identity mapping
	cmp	r5, r6
	addlo	r5, r5, #1			@ next section
	blo	1b

	/*
	 * Map our RAM from the start to the end of the kernel .bss section.
	 */

/* IAMROOT-12A:
 * ------------
 * 커널영역에 대한 페이지 테이블 매핑
 *
 * r0: 페이지테이블 시작
 *     저장할 페이지 테이블 엔트리 주소로 4(8)바이트씩 증가하는 카운터 
 *     r4(물리페이지테이블주소) + 가상커널주소의 섹션 인덱스 값*4
 *     라즈베리파이2: 처음 값은 0x0000_6000 부터 시작
 * r6: 페이지테이블 끝
 *     1) 매핑할 커널의 마지막(.bss 섹션) 가상주소-1
 *     2) 기존 r6에 대응하는 페이지 테이블 엔트리 주소로 바꿈.
 * r3: 기록할 섹션엔트리 값
 *     = r8:phys_offset (물리메모리주소) + r7:mm_mmuflags
 */
	add	r0, r4, #PAGE_OFFSET >> (SECTION_SHIFT - PMD_ORDER)
	ldr	r6, =(_end - 1)
	orr	r3, r8, r7
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
1:	str	r3, [r0], #1 << PMD_ORDER
	add	r3, r3, #1 << SECTION_SHIFT
	cmp	r0, r6
	bls	1b

#ifdef CONFIG_XIP_KERNEL
	/*
	 * Map the kernel image separately as it is not located in RAM.
	 */
#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
	mov	r3, pc
	mov	r3, r3, lsr #SECTION_SHIFT
	orr	r3, r7, r3, lsl #SECTION_SHIFT
	add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ORDER]!
	ldr	r6, =(_edata_loc - 1)
	add	r0, r0, #1 << PMD_ORDER
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
1:	cmp	r0, r6
	add	r3, r3, #1 << SECTION_SHIFT
	strls	r3, [r0], #1 << PMD_ORDER
	bls	1b
#endif

	/*
	 * Then map boot params address in r2 if specified.
	 * We map 2 sections in case the ATAGs/DTB crosses a section boundary.
	 */

/* IAMROOT-12A:
 * ------------
 * ATAG/DTB 영역에 대해 2개의 섹션으로 매핑한다.
 *
 * r0: r2(ATAG/DTB)주소의 섹션인덱스 -> 다시 주소로
 * r3: 대상에 대해 수정할 페이지테이블주소
 *     1) r0주소에 대한 가상 주소값 계산
 *        r3 = r0(ATAG/DTB 1M align된 주소) - r8(phys_offset) + PAGE_OFFSET
 *     2) 페이지 테이블 주소로 계산
 *        r4(물리페이지테이블주소) + r3(r0의 가상 주소)의 섹션 인덱스의 offset 
 * r6: 엔트리 값
 *     r7(mm_mmuflags) + r0(r2주소의 섹션인덱스)
 */
	mov	r0, r2, lsr #SECTION_SHIFT
	movs	r0, r0, lsl #SECTION_SHIFT

/* IAMROOT-12A:
 * ------------
 * r0가 0이되는 케이스(ATAG나 DTB가 주어지지 않은 경우)는 
 * 아래의 매핑을 수행하지 않는다.
 */
	subne	r3, r0, r8
	addne	r3, r3, #PAGE_OFFSET
	addne	r3, r4, r3, lsr #(SECTION_SHIFT - PMD_ORDER)
	orrne	r6, r7, r0
	strne	r6, [r3], #1 << PMD_ORDER

/* IAMROOT-12A:
 * ------------
 * DTB는 최대 1M이며, 최대 2개의 섹션이 필요 할 수 있으므로
 * 1M 증가하여 한번 더 매핑한다.
 */
	addne	r6, r6, #1 << SECTION_SHIFT
	strne	r6, [r3]

#if defined(CONFIG_ARM_LPAE) && defined(CONFIG_CPU_ENDIAN_BE8)
	sub	r4, r4, #4			@ Fixup page table pointer
						@ for 64-bit descriptors
#endif

#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
	/*
	 * Map in IO space for serial debugging.
	 * This allows debug messages to be output
	 * via a serial console before paging_init.
	 */
	addruart r7, r3, r0

	mov	r3, r3, lsr #SECTION_SHIFT
	mov	r3, r3, lsl #PMD_ORDER

	add	r0, r4, r3
	mov	r3, r7, lsr #SECTION_SHIFT
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
	orr	r3, r7, r3, lsl #SECTION_SHIFT
#ifdef CONFIG_ARM_LPAE
	mov	r7, #1 << (54 - 32)		@ XN
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4
	str	r3, [r0], #4
#else
	str	r3, [r0], #4
	str	r7, [r0], #4
#endif
#else
	orr	r3, r3, #PMD_SECT_XN
	str	r3, [r0], #4
#endif

#else /* CONFIG_DEBUG_ICEDCC || CONFIG_DEBUG_SEMIHOSTING */
	/* we don't need any serial debugging mappings */
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
#endif

#if defined(CONFIG_ARCH_NETWINDER) || defined(CONFIG_ARCH_CATS)
	/*
	 * If we're using the NetWinder or CATS, we also need to map
	 * in the 16550-type serial port for the debug messages
	 */
	add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x7c000000
	str	r3, [r0]
#endif
#ifdef CONFIG_ARCH_RPC
	/*
	 * Map in screen at 0x02000000 & SCREEN2_BASE
	 * Similar reasons here - for debug.  This is
	 * only for Acorn RiscPC architectures.
	 */
	add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x02000000
	str	r3, [r0]
	add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0]
#endif
#endif
#ifdef CONFIG_ARM_LPAE
	sub	r4, r4, #0x1000		@ point to the PGD table
	mov	r4, r4, lsr #ARCH_PGD_SHIFT
#endif
	ret	lr
ENDPROC(__create_page_tables)
	.ltorg
	.align
__turn_mmu_on_loc:
	.long	.
	.long	__turn_mmu_on
	.long	__turn_mmu_on_end

#if defined(CONFIG_SMP)
	.text
ENTRY(secondary_startup_arm)
	.arm
 THUMB(	adr	r9, BSYM(1f)	)	@ Kernel is entered in ARM.
 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
 THUMB(	.thumb			)	@ switch to Thumb now.
 THUMB(1:			)
ENTRY(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 *
	 * Ensure that we're in SVC mode, and IRQs are disabled.  Lookup
	 * the processor type - there is no need to check the machine type
	 * as it has already been validated by the primary processor.
	 */

 ARM_BE8(setend	be)				@ ensure we are in BE8 mode

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install_secondary
#endif
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type
	movs	r10, r5				@ invalid processor?
	moveq	r0, #'p'			@ yes, error 'p'
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p

	/*
	 * Use the page tables supplied from  __cpu_up.
	 */
	adr	r4, __secondary_data
	ldmia	r4, {r5, r7, r12}		@ address to jump to after
	sub	lr, r4, r5			@ mmu has been enabled
	ldr	r4, [r7, lr]			@ get secondary_data.pgdir
	add	r7, r7, #4
	ldr	r8, [r7, lr]			@ get secondary_data.swapper_pg_dir
	adr	lr, BSYM(__enable_mmu)		@ return address
	mov	r13, r12			@ __secondary_switched address
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	) @ initialise processor
						  @ (return control reg)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	ret	r12				)
ENDPROC(secondary_startup)
ENDPROC(secondary_startup_arm)

	/*
	 * r6  = &secondary_data
	 */
ENTRY(__secondary_switched)
	ldr	sp, [r7, #4]			@ get secondary_data.stack
	mov	fp, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)

	.align

	.type	__secondary_data, %object
__secondary_data:
	.long	.
	.long	secondary_data
	.long	__secondary_switched
#endif /* defined(CONFIG_SMP) */



/*
 * Setup common bits before finally enabling the MMU.  Essentially
 * this is just loading the page table pointer and domain access
 * registers.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r4  = page table (see ARCH_PGD_SHIFT in asm/memory.h)
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 */

/* IAMROOT-12A:
 * ------------
 * SCTLR의 비트를 조작.
 *    -CRA: alignment abort. <- 0
 */

__enable_mmu:
#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
	orr	r0, r0, #CR_A
#else
	bic	r0, r0, #CR_A
#endif
#ifdef CONFIG_CPU_DCACHE_DISABLE
	bic	r0, r0, #CR_C
#endif
#ifdef CONFIG_CPU_BPREDICT_DISABLE
	bic	r0, r0, #CR_Z
#endif
#ifdef CONFIG_CPU_ICACHE_DISABLE
	bic	r0, r0, #CR_I
#endif

/* IAMROOT-12A:
 * ------------
 * 아래의 코드는 ARMv7에서는 deprecated되어 무시되지만 하위 호환성을 위해
 * 도메인 설정을 한다.
 */
#ifndef CONFIG_ARM_LPAE
	mov	r5, #(domain_val(DOMAIN_USER, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_TABLE, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_IO, DOMAIN_CLIENT))
	mcr	p15, 0, r5, c3, c0, 0		@ load domain access register
	mcr	p15, 0, r4, c2, c0, 0		@ load page table pointer
#endif
	b	__turn_mmu_on
ENDPROC(__enable_mmu)

/*
 * Enable the MMU.  This completely changes the structure of the visible
 * memory space.  You will not be able to trace execution through this.
 * If you have an enquiry about this, *please* check the linux-arm-kernel
 * mailing list archives BEFORE sending another post to the list.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 *
 * other registers depend on the function called upon completion
 */

/* IAMROOT-12A:
 * ------------
 * .idmap 영역은 1:1 identity mapping 영역이라하여 별도의 섹션에 저장.
 * MMU를 켜는 순간에 다음으로 수행되는 명령들이 가상주소를 얻기 위해
 * TLB를 통해 페이지 테이블에 접근하게 되는데 이 가상주소에 대응하는
 * 매핑이 페이지 테이블에 있어야 한다.
 *
 * r0: write SCTLR
 * r3: read IDR	
 */

	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(__turn_mmu_on)
	mov	r0, r0
	instr_sync
	mcr	p15, 0, r0, c1, c0, 0		@ write control reg
	mrc	p15, 0, r3, c0, c0, 0		@ read id reg
	instr_sync
	mov	r3, r3
	mov	r3, r13
	ret	r3
__turn_mmu_on_end:
ENDPROC(__turn_mmu_on)
	.popsection


#ifdef CONFIG_SMP_ON_UP
	__HEAD
__fixup_smp:

/* IAMROOT-12A:
 * ------------
 * 1) UP case A - ARMv7이 아니면 UP
 * 0x000f000: armv7
 * tst는 and 연산 후 상태 반영, teq는 xor 연산 후 상태 반영
 * MIDR을 읽어서 MIDR.architecture가 f가 아니면(armv7이 아니면) UP에서
 * 동작한 것이라고 판단하여 __fixup_smp_on_up으로 jump
 *
 * Implementer[31..24]:		0x41	=ARM
 * Variant[23..20]:		0x0	=Major revision number
 * Architecture[19..16]:	0xf	=ARMv7
 * Primary part number[15..4]:	0xc07	=Cortex-A7 MPCore part number
 * Revision[3..0]:		0x3	=Minor revision number
 */
	and	r3, r9, #0x000f0000	@ architecture version
	teq	r3, #0x000f0000		@ CPU ID supported?
	bne	__fixup_smp_on_up	@ no, assume UP

/* IAMROOT-12A:
 * ------------
 * 2) ARM11MPCore인 경우 SMP
 * 여기서부터 armv7
 *
 * variant, Architecture, Revision 비트를 clear 
 * Implementer=0x41, primary part number=0xb02 이면 11MPCore로 판단한다.
 * 이 프로세서는 MP 프로세서 이므로 루틴을 더 이상 실행하지 않고 종료.
 */
	bic	r3, r9, #0x00ff0000
	bic	r3, r3, #0x0000000f	@ mask 0xff00fff0
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000b000
	orr	r4, r4, #0x00000020	@ val 0x4100b020
	teq	r3, r4			@ ARM 11MPCore?
	reteq	lr			@ yes, assume SMP

/* IAMROOT-12A:
 * ------------
 * 3) UP case B - Multiprocessing Extension이 포함되었지만 UP로 구성된 시스템은 UP
 * MPIDR[31]=Multiprocess Extensions이 있는 경우 항상 1
 * MIPDR[30]=UP인경우 1, MP인경우 0
 */
	mrc	p15, 0, r0, c0, c0, 5	@ read MPIDR
	and	r0, r0, #0xc0000000	@ multiprocessing extensions and
	teq	r0, #0x80000000		@ not part of a uniprocessor system?
	bne    __fixup_smp_on_up	@ no, assume UP

/* IAMROOT-12A:
 * ------------
 * 4) Cortex-A9 MPCore가 아닌 다른 시리즈들은 SMP
 * r3가 r4(0x4100c090)와 비교하여 동일하지 않으면 리턴.
 * Cortex-A9의 경우 UP로 만든적이 있다.
 */
	@ Core indicates it is SMP. Check for Aegis SOC where a single
	@ Cortex-A9 CPU is present but SMP operations fault.
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000c000
	orr	r4, r4, #0x00000090
	teq	r3, r4			@ Check for ARM Cortex-A9
	retne	lr			@ Not ARM Cortex-A9,

/* IAMROOT-12A:
 * ------------
 * 5) UP case C - Cortex-A9 이면서 IO base 주소가 0이면 UP
 * CBAR에서 IO장치의 base 주소를 얻어온다.(TRM 참고)
 * IO 기본 주소는 CBAR[31..15]=bit31~bit15, CBAR[7..0]=bit39~bit32
 * IO 기본 주소가 0이면 UP로 인식
 */
	@ If a future SoC *does* use 0x0 as the PERIPH_BASE, then the
	@ below address check will need to be #ifdef'd or equivalent
	@ for the Aegis platform.
	mrc	p15, 4, r0, c15, c0	@ get SCU base address
	teq	r0, #0x0		@ '0' on actual UP A9 hardware
	beq	__fixup_smp_on_up	@ So its an A9 UP

/* IAMROOT-12A:
 * ------------
 * 6) UP case D - Cortex-A9 이면서 CPU 수가 1개이면 UP
 * IO 기본 주소 + 4의 위치에서 SCU 설정값을 읽어온다. 
 * 여기서 CPU 갯수를 읽어와서 0(UP)이 아닌 경우(MP) 리턴
 * CPU가 1개 이면 루틴을 빠져나가지 않고 계속 __fixup_smp_on_up: 루틴 진행
 */
	ldr	r0, [r0, #4]		@ read SCU Config
ARM_BE8(rev	r0, r0)			@ byteswap if big endian
	and	r0, r0, #0x3		@ number of CPUs
	teq	r0, #0x0		@ is 1?
	retne	lr

/* IAMROOT-12A:
 * ------------
 * r3: offset
 * r4: __smpalt_begin
 * r5: __smpalt_end 
 */
__fixup_smp_on_up:
	adr	r0, 1f
	ldmia	r0, {r3 - r5}
	sub	r3, r0, r3
	add	r4, r4, r3
	add	r5, r5, r3
	b	__do_fixup_smp_on_up
ENDPROC(__fixup_smp)

	.align
1:	.word	.
	.word	__smpalt_begin
	.word	__smpalt_end

	.pushsection .data
	.globl	smp_on_up
smp_on_up:
	ALT_SMP(.long	1)
	ALT_UP(.long	0)
	.popsection
#endif

/* IAMROOT-12A:
 * ------------
 * SMP 코드 자리에 UP코드를 복사하는 주 로직.
 * ALT_SMP()와 ALT_UP는 붙어 다닌다.
 * ALT_SMP와 ALT_UP에 들어가는 명령(instruction)은 항상 4바이트 이다.
 *
 * #define ALT_SMP(instr...)    \
 * 9998:   instr
 *
 * #define ALT_UP(instr...)			\
 *	.pushsection ".alt.smp.init", "a";	\
 *	.long   9998b;				\
 *	instr;					\
 *	.popsection
 *
 * ALT_SMP는 실제 코드섹션 영역에 그대로 저장되고,
 * ALT_UP는 .alt.smp.init 섹션에 4바이트씩 두 번 push되는데 
 * 처음 word는 ALT_SMP의 명령주소를 가리키고 있고
 * (나중에 치환할 주소를 알아내기 위함), 두 번째 word는 UP 코드가 있다.
 *
 * 아래 로직은 r4 -> r5가 될 때까지 루프를 돌며,
 * r4: 처음에 __smpalt_begin으로 시작해서 __smpalt_end로 카운트하는 변수
 * r0: ALT_SMP에 있는 instr 주소 
 * r6: ALT_UP에 있는 instr 값
 * r6를 r0+r3(offset) 위치에 저장
 */
	.text
__do_fixup_smp_on_up:
	cmp	r4, r5
	reths	lr
	ldmia	r4!, {r0, r6}
 ARM(	str	r6, [r0, r3]	)
 THUMB(	add	r0, r0, r3	)
#ifdef __ARMEB__
 THUMB(	mov	r6, r6, ror #16	)	@ Convert word order for big-endian.
#endif
 THUMB(	strh	r6, [r0], #2	)	@ For Thumb-2, store as two halfwords
 THUMB(	mov	r6, r6, lsr #16	)	@ to be robust against misaligned r3.
 THUMB(	strh	r6, [r0]	)
	b	__do_fixup_smp_on_up
ENDPROC(__do_fixup_smp_on_up)

ENTRY(fixup_smp)
	stmfd	sp!, {r4 - r6, lr}
	mov	r4, r0
	add	r5, r0, r1
	mov	r3, #0
	bl	__do_fixup_smp_on_up
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(fixup_smp)

#ifdef __ARMEB__
#define LOW_OFFSET	0x4
#define HIGH_OFFSET	0x0
#else
#define LOW_OFFSET	0x0
#define HIGH_OFFSET	0x4
#endif

#ifdef CONFIG_ARM_PATCH_PHYS_VIRT

/* __fixup_pv_table - patch the stub instructions with the delta between
 * PHYS_OFFSET and PAGE_OFFSET, which is assumed to be 16MiB aligned and
 * can be expressed by an immediate shifter operand. The stub instruction
 * has a form of '(add|sub) rd, rn, #imm'.
 */

/* IAMROOT-12A:
 * ------------
 *  __HEAD -> .head.text 섹션 
 *
 * r3: offset (물리주소 - 가상주소)
 * r4: pv_table의 시작 물리 주소
 * r5: pv_table의 끝 물리 주소
 * r6: __pv_phys_pfn_offset(라벨) 변수의 주소 
 * r7: __pv_offset 변수의(라벨) 주소
 * r8: 물리 시작 주소로 이 함수 호출 전에 계산됨. (phys_offset)
 */
	__HEAD
__fixup_pv_table:
	adr	r0, 1f
	ldmia	r0, {r3-r7}

/* IAMROOT-12A:
 * ------------
 * mvn: 오퍼랜드 값을 비트단위로 not을 수행. 
 *      ip -> 0xffff_ffff
 *
 * r4~r7의 가상 주소에 offset를 적용해 물리주소로 변경한다.
 */
	mvn	ip, #0
	subs	r3, r0, r3	@ PHYS_OFFSET - PAGE_OFFSET
	add	r4, r4, r3	@ adjust table start address
	add	r5, r5, r3	@ adjust table end address
	add	r6, r6, r3	@ adjust __pv_phys_pfn_offset address
	add	r7, r7, r3	@ adjust __pv_offset address

/* IAMROOT-12A:
 * ------------
 * r0:__pv_phys_pfn_offset(0x0) <----(pfn)------ r8:phys_offset(0x0000_0000)
 *     물리주소를 12비트 우측으로 쉬프트하면 페이지 번호를 얻을 수 있다.
 *
 * 결국 __pv_phys_pfn_offset에는 교정된 물리메모리의 pfn값이 들어가고,
 *		(라즈베리파이2:  0x0)
 *      __pv_offset에는 물리-가상 주소 즉 offset 값이 들어간다.
 *              이 값은 물리 <--> 가상 주소 변환에 offset으로 사용된다.
 *		(라즈베리파이2:  0xffff_ffff_8000_0000)
 */
	mov	r0, r8, lsr #PAGE_SHIFT	@ convert to PFN
	str	r0, [r6]	@ save computed PHYS_OFFSET to __pv_phys_pfn_offset

/* IAMROOT-12A:
 * ------------
 * PHYS_OFFSET - PAGE_OFFSET의 결과가 음수인 경우 아래 명령 수행
 *   이 때에는 0xffff_ffff(음수의미)를 __pv_offset+#HIGHMEM에 저장 
 * 라즈베리파이2: strcc가 실행되어 __pv_offset+#HIGHMEM에 0xffff_ffff
 *
 * 물리 RAM 주소가 커널시작가상주소보다 작은 경우에만 저장한다. 
 *	pv_offset+#HIGH_OFFSET에-1(0xffff_ffff)을 저장
 */
	strcc	ip, [r7, #HIGH_OFFSET]	@ save to __pv_offset high bits

/* IAMROOT-12A:
 * ------------
 * teq로 비교를 하는데 r3의 하위 24비트 align이 되어 있는지 확인.
 * (r6는 align 비교를 위해 잠시 사용하고 버리는 용도)
 * 실제로 MSB 8bit를 사용하여 add, sub등의 명령에서 오퍼랜드 부분의 
 * immediate 필드의 8비트만을 변경하므로 반드시 align 되어 있어야 한다.
 * 결국 가상 주소와 물리 주소의 변환은 MSB 8bit만 이루어져야 하므로
 * 24비트 align을 체크해야 한다.
 */
	mov	r6, r3, lsr #24	@ constant for add/sub instructions
	teq	r3, r6, lsl #24 @ must be 16MiB aligned
THUMB(	it	ne		@ cross section branch )
	bne	__error
	str	r3, [r7, #LOW_OFFSET]	@ save to __pv_offset low bits
	b	__fixup_a_pv_table
ENDPROC(__fixup_pv_table)

/* IAMROOT-12A:
 * ------------
 *  아래 구조는 빌드 시에 결정되며 순서대로 다음과 같다.
 *  .:                     빌드 시 주소(커널 가상주소로 시작. 0x8000_xxxx)
 *  __pv_table_begin:      __pv_table 시작 주소(kernel/vmlinux.lds.S 참고)
 *  __pv_table_end:        __pv_table 끝 주소 
 *  __pv_phys_pfn_offset:  이 소스 100여줄 밑에 __pv_phys_pfn_offset 데이터 주소 
 *  __pv_offset:                     "          __pv_offset 데이터 주소
 *
 *  __pv_table은 .init.pv_table 섹션에 위치하고 
 *               커널 C 소스에서 __pv_stub() 함수를 사용한 경우 그 함수 사용
 *               위치가 담긴다.
 */
	.align
1:	.long	.
	.long	__pv_table_begin
	.long	__pv_table_end
2:	.long	__pv_phys_pfn_offset
	.long	__pv_offset

	.text
__fixup_a_pv_table:

/* IAMROOT-12A:
 * ------------
 * r6: pv_offset 변수의 물리 주소
 */
	adr	r0, 3f
	ldr	r6, [r0]
	add	r6, r6, r3
/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2:
 *     r0(0xffff_ffff) <- pv_offset + #HIGH_OFFSET 
 *     r6(0x8000_0000) <- pv_offset + #LOW_OFFSET
 */
	ldr	r0, [r6, #HIGH_OFFSET]	@ pv_offset high word
	ldr	r6, [r6, #LOW_OFFSET]	@ pv_offset low word

/* IAMROOT-12A:
 * ------------
 * r6 <- add 또는 sub 인스트럭션의 immediate 영역(8비트)만을 수정하기 위해
 *       r6를 24비트 쉬프트한다. (라즈베리파이2: 0x0000_0080)  
 */
	mov	r6, r6, lsr #24

/* IAMROOT-12A:
 * ------------
 * compare: r0(__pv_offset+#HIGH_OFFSET) <-> 0xffff_ffff  (cmp r0, #-1)
 */
	cmn	r0, #1
#ifdef CONFIG_THUMB2_KERNEL
	moveq	r0, #0x200000	@ set bit 21, mov to mvn instruction
	lsls	r6, #24
	beq	2f
	clz	r7, r6
	lsr	r6, #24
	lsl	r6, r7
	bic	r6, #0x0080
	lsrs	r7, #1
	orrcs	r6, #0x0080
	orr	r6, r6, r7, lsl #12
	orr	r6, #0x4000
	b	2f
1:	add     r7, r3
	ldrh	ip, [r7, #2]
ARM_BE8(rev16	ip, ip)
	tst	ip, #0x4000
	and	ip, #0x8f00
	orrne	ip, r6	@ mask in offset bits 31-24
	orreq	ip, r0	@ mask in offset bits 7-0
ARM_BE8(rev16	ip, ip)
	strh	ip, [r7, #2]
	bne	2f
	ldrh	ip, [r7]
ARM_BE8(rev16	ip, ip)
	bic	ip, #0x20
	orr	ip, ip, r0, lsr #16
ARM_BE8(rev16	ip, ip)
	strh	ip, [r7]
2:	cmp	r4, r5
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	bx	lr
#else

/* IAMROOT-12A:
 * ------------
 * r0: 라즈베리파이2의 경우 moveq가 수행되어 r0 <- 0x400000
 *
 * 정확히 표현하자면,
 * 커널 시작 가상 주소가 물리 주소보다 큰 경우에 아래 코드가 실행된다.
 * ARM 32bit with LPAE (36bit)를 위해 사용되는 코드로 결국 LPAE를 사용할 때
 * 사용하기 위한 코드이다.
 *
 * ARM은 32비트 -> 64비트로 프로세서가 이전하는 타이밍에 임시로 LPAE를
 * 지원하였는데 32비트 x86 시스템에서 4G 이상 물리메모리를 지원하기 위해
 * 나온 기법과 동일하다. x86은 아직도 이 옵션을 사용하는 PC가 많이 있지만
 * ARM에서는 대용량 메모리를 32비트 ARM에서 지원할 목적으로 LPAE가 나왔지만
 * 나온지 2년도 안된 사이에 64비트로 옮겨갔다.
 */
#ifdef CONFIG_CPU_ENDIAN_BE8
	moveq	r0, #0x00004000	@ set bit 22, mov to mvn instruction
#else
	moveq	r0, #0x400000	@ set bit 22, mov to mvn instruction
#endif
	b	2f

/* IAMROOT-12A:
 * ------------
 * r7: pv_table에서 읽어온 엔트리에는 add(s), sub 및 mov 등의 명령을 가리키는
 *     주소가 담겨있다.
 * ip: add(s), sub 및 mov 등의 명령을 가져오는데 r7에는 가상 주소가 담겨있으므로
 *     pv_offset를 적용하여 물리 주소로 변경한다.
 */
1:	ldr	ip, [r7, r3]
#ifdef CONFIG_CPU_ENDIAN_BE8
	@ in BE8, we load data in BE, but instructions still in LE
	bic	ip, ip, #0xff000000
	tst	ip, #0x000f0000	@ check the rotation field
	orrne	ip, ip, r6, lsl #24 @ mask in offset bits 31-24
	biceq	ip, ip, #0x00004000 @ clear bit 22
	orreq	ip, ip, r0      @ mask in offset bits 7-0
#else

/* IAMROOT-12A:
 * ------------
 * add 및 sub 명령의 immediate 부분을 클리어
 */
	bic	ip, ip, #0x000000ff

/* IAMROOT-12A:
 * ------------
 * 명령에서 rotate 필드를 체크하여 0이 아닌 경우, 즉 물리 메모리가 32비트를 사용하는
 * 경우 immediate 부분만 갱신.
 */
	tst	ip, #0xf00	@ check the rotation field
	orrne	ip, ip, r6	@ mask in offset bits 31-24

/* IAMROOT-12A:
 * ------------
 * 로테이션 필드가 0이 되는 조건: 
 *    - 물리메모리가 64비트 시스템이면서 __virt_to_phys()를 사용한 경우,
 *    - __pv_stub_mov_hi() 매크로를 호출하고, 
 *    - 내부에서 mov 명령을 사용하여 __PV_BITS_7_0을 대입하여 로테이트 필드가 0이 됨.
 *    - 라즈베리파이2: 물리메모리가 32비트로 설정되어 동작하므로 이 경우 수행되지 않음
 * bit22의 opcode는 mov(0) / mvn(1) 
 * 먼저 bit22를 클리어하여 mov 상태로 바꾸고, r0(mov/mvn을 결정)를 더한다.
 *
 * 가상주소를 물리주소로 리턴하는 경우 64비트(LPAE) 물리주소를 사용하면
 * 단순히 가상주소에 양수의 offset를 더하는 경우 4G의 영역을 초과한다. 
 * 따라서 물리주소의 결과값의 상위 word에 -1(mvn #0)을 둔 상태에서 adds를 하게되면
 * 정확한 물리주소가 리턴된다.
 *
 * 반대로 물리주소를 가상주소로 변환하는 경우 물리주소 크기와 관계 없이 리턴되는 값은
 * 언제나 32비트의 가상주소이므로 미리 -1을 두고 연산할 필요가 없어진다.
 *
 * 물론 32비트 물리주소의 경우에도 가상주소와 물리주소의 양방향 변환 시 빌림처리등이
 * 필요 없어서 단순히 더하는 것으로 정확한 연산이 이루어진다.
 */
	biceq	ip, ip, #0x400000	@ clear bit 22
	orreq	ip, ip, r0	@ mask in offset bits 7-0
#endif

/* IAMROOT-12A:
 * ------------
 * ip 값(명령+rotate+immediate)이 결정되었으므로 코드 영역을 ip로 바꿔치기 함.
 */
	str	ip, [r7, r3]

/* IAMROOT-12A:
 * ------------
 * 루프(__pv_table_end)의 끝?
 */
2:	cmp	r4, r5

/* IAMROOT-12A:
 * ------------
 * pv_table에서 주소를 가져오고 카운터는 4를 증가.
 */
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	ret	lr
#endif
ENDPROC(__fixup_a_pv_table)

/* IAMROOT-12A:
 * ------------
 * 이 아래의 값도 빌드 시에 결정되며 소스 코드 중 몇 줄 밑에 있는 
 * __pv_offset 데이터 위치를 가리키는 주소를 담고 있다.
 */
	.align
3:	.long __pv_offset

ENTRY(fixup_pv_table)
	stmfd	sp!, {r4 - r7, lr}
	mov	r3, #0			@ no offset
	mov	r4, r0			@ r0 = table start
	add	r5, r0, r1		@ r1 = table size
	bl	__fixup_a_pv_table
	ldmfd	sp!, {r4 - r7, pc}
ENDPROC(fixup_pv_table)

/* IAMROOT-12A:
 * ------------
 * __pv_offset의 경우 2개의 워드를 사용한다. 
 * quad: ARM이 아닌 아키텍처에서는 워드가 2바이트 따라서 quad word가 8바이트,
 *       ARM에서는 워드가 4바이트지만 quad 타입은 위와 동일하게 8바이트.
 *
 * __pv_offset+#LOW_OFFSET에 교정된 pv_offset를 저장한다.
 * __pv_offset+#HIGH_OFFSET에 초기 값 0x0000_0000로 채운다.
 *
 *   #HIGH_OFFSET는 빅엔디안에서 #0이며 리틀엔디안에서는 #4
 *   #LOW_OFFSET는  빅엔디안에서 #4이며 리틀엔디안에서는 #0
 *
 * 라즈베리파이2: 빌드 시 초기 값 0이 담기고, 
 *                실제 수행 후 __pv_phys_pfn_offset = 0x0000_0000
 *                             __pv_offset = 0xffff_ffff_8000_0000
 */
	.data
	.globl	__pv_phys_pfn_offset
	.type	__pv_phys_pfn_offset, %object
__pv_phys_pfn_offset:
	.word	0
	.size	__pv_phys_pfn_offset, . -__pv_phys_pfn_offset

	.globl	__pv_offset
	.type	__pv_offset, %object
__pv_offset:
	.quad	0
	.size	__pv_offset, . -__pv_offset
#endif

#include "head-common.S"
