/*
 *  arch/arm/include/asm/memory.h
 *
 *  Copyright (C) 2000-2002 Russell King
 *  modification for nommu, Hyok S. Choi, 2004
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Note: this file should not be included by non-asm/.h files
 */
#ifndef __ASM_ARM_MEMORY_H
#define __ASM_ARM_MEMORY_H

#include <linux/compiler.h>
#include <linux/const.h>
#include <linux/types.h>
#include <linux/sizes.h>

#include <asm/cache.h>

#ifdef CONFIG_NEED_MACH_MEMORY_H
#include <mach/memory.h>
#endif

/*
 * Allow for constants defined here to be used from assembly code
 * by prepending the UL suffix only with actual C code compilation.
 */
#define UL(x) _AC(x, UL)

/* PAGE_OFFSET - the virtual address of the start of the kernel image */
#define PAGE_OFFSET		UL(CONFIG_PAGE_OFFSET)

#ifdef CONFIG_MMU

/*
 * TASK_SIZE - the maximum size of a user space task.
 * TASK_UNMAPPED_BASE - the lower boundary of the mmap VM area
 */

/* IAMROOT-12AB:
 * -------------
 * 모듈도 커널 코드 영역이므로 
 *      user space는 커널 space + 모듈(16M)을 제외한 영역이다.
 * rpi2: 2G-16M
 */
#define TASK_SIZE		(UL(CONFIG_PAGE_OFFSET) - UL(SZ_16M))
#define TASK_UNMAPPED_BASE	ALIGN(TASK_SIZE / 3, SZ_16M)

/*
 * The maximum size of a 26-bit user space task.
 */
#define TASK_SIZE_26		(UL(1) << 26)

/*
 * The module space lives between the addresses given by TASK_SIZE
 * and PAGE_OFFSET - it must be within 32MB of the kernel text.
 */
#ifndef CONFIG_THUMB2_KERNEL

/* IAMROOT-12AB:
 * -------------
 * rpi2: MODULES_VADDR=0x7f00_0000
 */
#define MODULES_VADDR		(PAGE_OFFSET - SZ_16M)
#else
/* smaller range for Thumb-2 symbols relocation (2^24)*/
#define MODULES_VADDR		(PAGE_OFFSET - SZ_8M)
#endif

#if TASK_SIZE > MODULES_VADDR
#error Top of user space clashes with start of module space
#endif

/*
 * The highmem pkmap virtual space shares the end of the module area.
 */
#ifdef CONFIG_HIGHMEM
#define MODULES_END		(PAGE_OFFSET - PMD_SIZE)
#else
#define MODULES_END		(PAGE_OFFSET)
#endif

/*
 * The XIP kernel gets mapped at the bottom of the module vm area.
 * Since we use sections to map it, this macro replaces the physical address
 * with its virtual address while keeping offset from the base section.
 */
#define XIP_VIRT_ADDR(physaddr)  (MODULES_VADDR + ((physaddr) & 0x000fffff))

/*
 * Allow 16MB-aligned ioremap pages
 */
#define IOREMAP_MAX_ORDER	24

#else /* CONFIG_MMU */

/*
 * The limitation of user task size can grow up to the end of free ram region.
 * It is difficult to define and perhaps will never meet the original meaning
 * of this define that was meant to.
 * Fortunately, there is no reference for this in noMMU mode, for now.
 */
#define TASK_SIZE		UL(0xffffffff)

#ifndef TASK_UNMAPPED_BASE
#define TASK_UNMAPPED_BASE	UL(0x00000000)
#endif

#ifndef END_MEM
#define END_MEM     		(UL(CONFIG_DRAM_BASE) + CONFIG_DRAM_SIZE)
#endif

/*
 * The module can be at any place in ram in nommu mode.
 */
#define MODULES_END		(END_MEM)
#define MODULES_VADDR		PAGE_OFFSET

#define XIP_VIRT_ADDR(physaddr)  (physaddr)

#endif /* !CONFIG_MMU */

/*
 * We fix the TCM memories max 32 KiB ITCM resp DTCM at these
 * locations
 */
#ifdef CONFIG_HAVE_TCM

/* IAMROOT-12AB:
 * -------------
 * ARM에서 사용하는 TCM의 가상 주소
 */
#define ITCM_OFFSET	UL(0xfffe0000)
#define DTCM_OFFSET	UL(0xfffe8000)
#endif

/*
 * Convert a physical address to a Page Frame Number and back
 */
#define	__phys_to_pfn(paddr)	((unsigned long)((paddr) >> PAGE_SHIFT))
#define	__pfn_to_phys(pfn)	((phys_addr_t)(pfn) << PAGE_SHIFT)

/*
 * Convert a page to/from a physical address
 */
#define page_to_phys(page)	(__pfn_to_phys(page_to_pfn(page)))
#define phys_to_page(phys)	(pfn_to_page(__phys_to_pfn(phys)))

/*
 * Minimum guaranted alignment in pgd_alloc().  The page table pointers passed
 * around in head.S and proc-*.S are shifted by this amount, in order to
 * leave spare high bits for systems with physical address extension.  This
 * does not fully accomodate the 40-bit addressing capability of ARM LPAE, but
 * gives us about 38-bits or so.
 */
#ifdef CONFIG_ARM_LPAE
#define ARCH_PGD_SHIFT		L1_CACHE_SHIFT
#else
#define ARCH_PGD_SHIFT		0
#endif
#define ARCH_PGD_MASK		((1 << ARCH_PGD_SHIFT) - 1)

/*
 * PLAT_PHYS_OFFSET is the offset (from zero) of the start of physical
 * memory.  This is used for XIP and NoMMU kernels, and on platforms that don't
 * have CONFIG_ARM_PATCH_PHYS_VIRT. Assembly code must always use
 * PLAT_PHYS_OFFSET and not PHYS_OFFSET.
 */
#define PLAT_PHYS_OFFSET	UL(CONFIG_PHYS_OFFSET)

#ifndef __ASSEMBLY__

/*
 * Physical vs virtual RAM address space conversion.  These are
 * private definitions which should NOT be used outside memory.h
 * files.  Use virt_to_phys/phys_to_virt/__pa/__va instead.
 *
 * PFNs are used to describe any physical page; this means
 * PFN 0 == physical address 0.
 */
#if defined(__virt_to_phys)

/* IAMROOT-12A:
 * ------------
 * 메모리 Case 1.
 * ------------
 * 별도 제작한 __virt_to_phys 매크로 사용 시
 *
 * 이 헤더 화일(asm/memory.h)에서 기본 제공하는 __virt_to_phys() 인라인함수를
 * 사용하지 않을 때 __virt_to_phys 매크로를 정의하여 사용하는 경우에 아래 3줄의
 * 설정을 사용한다. 
 *
 * 아직 ARM 기본 빌드에서는 별도로 정의하여 사용하는 시스템이 없음.
 */

#define PHYS_OFFSET	PLAT_PHYS_OFFSET
#define PHYS_PFN_OFFSET	((unsigned long)(PHYS_OFFSET >> PAGE_SHIFT))

#define virt_to_pfn(kaddr) (__pa(kaddr) >> PAGE_SHIFT)

#elif defined(CONFIG_ARM_PATCH_PHYS_VIRT)

/* IAMROOT-12A:
 * ------------
 * 메모리 Case 2.
 * ------------
 * 물리 시작 주소를 찾아 패치(pv_table)하는 경우 사용
 *
 * 라즈베리2는 이 옵션을 사용
 */

/*
 * Constants used to force the right instruction encodings and shifts
 * so that all we need to do is modify the 8-bit constant field.
 */

/* IAMROOT-12AB:
 * -------------
 * ARM add/mov 명령어등을 구성하는데 큰 상수값을 사용하려고 하는데
 * 큰 상수값을 표현하는 필드가 rotate[11:8], immeidate[7:0] 있다.
 * rotate 필드를 사용하여 immediate 필드 값을 우측으로 회전시켜
 * 큰 상수를 만들어내는데 rotate 값 1당 우측 회전이 2비트씩 이루어진다.
 * 따라서 아래의 0x8100_0000 은 우측으로 8번 회전시켜야 하므로
 * rotate 값은 4(8비트 우측 쉬프트), immediate 값은 0x81 이다.
 *
 * virt_to_phys() 또는 phys_to_virt() 등에서 va 및 pa 등의 주소 변환에
 * msb 8bit만 사용하므로 아래의 0x8100_0000을 사용하여 컴파일하면
 * rotate값이 4로 고정되고 va-pa 변환 함수에서는 rotate 값을 변경할
 * 필요가 없어지므로 코드 절약을 위해 사용한다.
 */
#define __PV_BITS_31_24	0x81000000
#define __PV_BITS_7_0	0x81

extern unsigned long __pv_phys_pfn_offset;
extern u64 __pv_offset;
extern void fixup_pv_table(const void *, unsigned long);
extern const void *__pv_table_begin, *__pv_table_end;

#define PHYS_OFFSET	((phys_addr_t)__pv_phys_pfn_offset << PAGE_SHIFT)
#define PHYS_PFN_OFFSET	(__pv_phys_pfn_offset)

#define virt_to_pfn(kaddr) \
	((((unsigned long)(kaddr) - PAGE_OFFSET) >> PAGE_SHIFT) + \
	 PHYS_PFN_OFFSET)

/* IAMROOT-12A:
 * ------------
 * __pv_stub() 매크로를 사용(add or sub)하는 곳의 주소를 .pv_table 섹션에 푸쉬한다.
 * 첫줄의 @ 와 함수명은 어떤 이유로 존재???
 *
 * instr: add와 sum을 사용
 * %0, %1, %2: to, from, type 순서로 인수 전달
 *
 * call: __pv_stub(x, t, "add", __PV_BITS_31_24);
 *  ->   1: add t, x, __PV_BITS_31_24
 *
 * "=r"     <- 
 * "r"      <-
 * "I"      <-
 *
 * .pushsection -> 현재 섹션을 컴파일러가 보관(push)하고 첫 번째 인수의 섹션 사용을 
 *                 컴파일러에게 설정 지시
 * .popsection ->  push했던 섹션 다시 복구(pop)
 */

#define __pv_stub(from,to,instr,type)			\
	__asm__("@ __pv_stub\n"				\
	"1:	" instr "	%0, %1, %2\n"		\
	"	.pushsection .pv_table,\"a\"\n"		\
	"	.long	1b\n"				\
	"	.popsection\n"				\
	: "=r" (to)					\
	: "r" (from), "I" (type))

/* IAMROOT-12A:
 * ------------
 * %R0: 인수 %0의 상위 32비트를 취급하는 레지스터
 */
#define __pv_stub_mov_hi(t)				\
	__asm__ volatile("@ __pv_stub_mov\n"		\
	"1:	mov	%R0, %1\n"			\
	"	.pushsection .pv_table,\"a\"\n"		\
	"	.long	1b\n"				\
	"	.popsection\n"				\
	: "=r" (t)					\
	: "I" (__PV_BITS_7_0))

/* IAMROOT-12A:
 * ------------
 * %Q0: 인수 %0의 하위 32비트를 취급하는 레지스터
 * 
 * adc 명령은 .pv_table에 push하지 않는다.
 *
 * __PV_BITS_31_24
 *     0b10000001_00000000_00000000_00000000 이라는 큰 상수를 ARM이 만들기 위해
 *     0b10000001에 대해 8번의 우측 rotation을 결정한다.
 *     Rotate 필드에 4를 지정하여 4x2(항상 2배) = 8번의 우측 rotate
 *    
 *    이렇게 미리 해놓으면 나중에 fixup 루틴이 동작할 때 rotate 필드는 수정하지 
 *    않아도된다.
 */
#define __pv_add_carry_stub(x, y)			\
	__asm__ volatile("@ __pv_add_carry_stub\n"	\
	"1:	adds	%Q0, %1, %2\n"			\
	"	adc	%R0, %R0, #0\n"			\
	"	.pushsection .pv_table,\"a\"\n"		\
	"	.long	1b\n"				\
	"	.popsection\n"				\
	: "+r" (y)					\
	: "r" (x), "I" (__PV_BITS_31_24)		\
	: "cc")

/* IAMROOT-12A:
 * ------------
 * pv_table에 해당 __pv_stub() 함수가 사용된 주소를 push한다.
 * pv_table은 
 *
 * __PV_BITS_31_24: 31번 비트부터 24번 비트까지라는 의미가 있다.
 *          0b10000001_00000000_00000000_00000000 (0x8100_0000)
 *
 * 물리주소 = 가상 주소 + pv_offset
 *
 * 라즈베리파이2의 경우 pv_offset: 0xffff_ffff_8000_0000
 */

static inline phys_addr_t __virt_to_phys(unsigned long x)
{
	phys_addr_t t;

/* IAMROOT-12A:
 * ------------
 * 물리주소 = 가상주소 + pv_offset
 *
 * 1) 32bit 가상 주소 -> 32비트 물리 주소
 *    예) rpi2: 
 *        phys = virt_to_phys(0x8123_0000);
 *             * __pv_stub(0x8123_0000, t, "add", 0x8100_0000);
 *                   	add t, 0x8123_0000, 0x8100_0000 
 *             * 패치 후 (pv_offset = 0xFFFF_FFFF_8000_0000)
 *                   	add t, 0x8123_0000, 0x8000_0000 
 *             * phys = 0x0123_0000
 *  
 * 2) 32bit 가상 주소 -> 64bit 물리주소(LPAE)
 *    예) 64비트 예(물리램=0x0_8000_0000, 커널가상주소=0xC000_0000)
 *        phys = virt_to_phys(0xC123_4560);
 *             * __pv_stub_mov_hi(0xC123_4560, 0x81); 
 *               __pv_add_carry_stub(t, 0xC123_4560, 0x8100_000);
 *                   	mov t[63:32], 0x81
 *                      adds t[31:0], 0x8100_0000
 *                      adc t[63:32], t[63:32], #0
 *             * 패치 후 (pv_offset = 0xFFFF_FFFF_C0000_0000)
 *                   	mov t[63:32], 0xFFFF_FFFF
 *                      adds t[31:0], 0xC123_4560, 0xC000_0000
 *                      adc t[63:32], t[63:32], #0
 *             * phys = 0x0_8123_4560
 *
 * 3) 32bit 가상 주소 -> 64bit 물리주소(LPAE)
 *    예) 64비트 예(물리램=0x1_8000_0000, 커널가상주소=0xC000_0000)
 *        phys = virt_to_phys(0x8123_4560);
 *             * __pv_stub_mov_hi(0xC123_4560, 0x81); 
 *               __pv_add_carry_stub(t, 0xC123_4560, 0x8100_000);
 *                   	mov t[63:32], 0x81
 *                      adds t[31:0], 0x8100_0000
 *                      adc t[63:32], t[63:32], #0
 *             * 패치 후 (pv_offset = 0x0_C000_0000)
 *                   	mov t[63:32], 0x0
 *                      adds t[31:0], 0xC123_4560, 0xC000_0000
 *                      adc t[63:32], t[63:32], #0
 *             * phys = 0x1_8123_4560
 */
	if (sizeof(phys_addr_t) == 4) {
		__pv_stub(x, t, "add", __PV_BITS_31_24);
	} else {
		__pv_stub_mov_hi(t);
		__pv_add_carry_stub(x, t);
	}
	return t;
}

/* IAMROOT-12A:
 * ------------
 * 가상주소 = 물리주소 - pv_offset
 *
 * 1) 32bit 물리 주소 -> 32bit 가상 주소 &
 * 2) 64bit 물리 주소 -> 32bit 가상 주소
 *    - __pv_stub()의 경우 __PV_BITS_31_24를 사용하여
 *      rotate[21:8] 필드 값은 4
      - rotate 필드값의 2배를 rotation 한다.
 *    - 따라서 rotate 값 4 = 우측으로 8번 로테이트
 */

static inline unsigned long __phys_to_virt(phys_addr_t x)
{
	unsigned long t;

	/*
	 * 'unsigned long' cast discard upper word when
	 * phys_addr_t is 64 bit, and makes sure that inline
	 * assembler expression receives 32 bit argument
	 * in place where 'r' 32 bit operand is expected.
	 */
	__pv_stub((unsigned long) x, t, "sub", __PV_BITS_31_24);
	return t;
}

#else

/* IAMROOT-12A:
 * ------------
 * 메모리 Case 3.
 * ------------
 * 물리 시작 주소를 찾아 패치(pv_table)하지 않는 경우 사용
 * (DTB 등 사용전에는 물리 시작 주소가 빌드 시 결정됨)
 */

#define PHYS_OFFSET	PLAT_PHYS_OFFSET
#define PHYS_PFN_OFFSET	((unsigned long)(PHYS_OFFSET >> PAGE_SHIFT))

static inline phys_addr_t __virt_to_phys(unsigned long x)
{
	return (phys_addr_t)x - PAGE_OFFSET + PHYS_OFFSET;
}

static inline unsigned long __phys_to_virt(phys_addr_t x)
{
	return x - PHYS_OFFSET + PAGE_OFFSET;
}

#define virt_to_pfn(kaddr) \
	((((unsigned long)(kaddr) - PAGE_OFFSET) >> PAGE_SHIFT) + \
	 PHYS_PFN_OFFSET)

#endif

/*
 * These are *only* valid on the kernel direct mapped RAM memory.
 * Note: Drivers should NOT use these.  They are the wrong
 * translation for translating DMA addresses.  Use the driver
 * DMA support - see dma-mapping.h.
 */
#define virt_to_phys virt_to_phys
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
	return __virt_to_phys((unsigned long)(x));
}

#define phys_to_virt phys_to_virt
static inline void *phys_to_virt(phys_addr_t x)
{
	return (void *)__phys_to_virt(x);
}

/*
 * Drivers should NOT use these either.
 */
#define __pa(x)			__virt_to_phys((unsigned long)(x))
#define __va(x)			((void *)__phys_to_virt((phys_addr_t)(x)))
#define pfn_to_kaddr(pfn)	__va((pfn) << PAGE_SHIFT)

extern phys_addr_t (*arch_virt_to_idmap)(unsigned long x);

/*
 * These are for systems that have a hardware interconnect supported alias of
 * physical memory for idmap purposes.  Most cases should leave these
 * untouched.
 */
static inline phys_addr_t __virt_to_idmap(unsigned long x)
{
	if (arch_virt_to_idmap)
		return arch_virt_to_idmap(x);
	else
		return __virt_to_phys(x);
}

#define virt_to_idmap(x)	__virt_to_idmap((unsigned long)(x))

/*
 * Virtual <-> DMA view memory address translations
 * Again, these are *only* valid on the kernel direct mapped RAM
 * memory.  Use of these is *deprecated* (and that doesn't mean
 * use the __ prefixed forms instead.)  See dma-mapping.h.
 */
#ifndef __virt_to_bus
#define __virt_to_bus	__virt_to_phys
#define __bus_to_virt	__phys_to_virt
#define __pfn_to_bus(x)	__pfn_to_phys(x)
#define __bus_to_pfn(x)	__phys_to_pfn(x)
#endif

#ifdef CONFIG_VIRT_TO_BUS
#define virt_to_bus virt_to_bus
static inline __deprecated unsigned long virt_to_bus(void *x)
{
	return __virt_to_bus((unsigned long)x);
}

#define bus_to_virt bus_to_virt
static inline __deprecated void *bus_to_virt(unsigned long x)
{
	return (void *)__bus_to_virt(x);
}
#endif

/*
 * Conversion between a struct page and a physical address.
 *
 *  page_to_pfn(page)	convert a struct page * to a PFN number
 *  pfn_to_page(pfn)	convert a _valid_ PFN number to struct page *
 *
 *  virt_to_page(k)	convert a _valid_ virtual address to struct page *
 *  virt_addr_valid(k)	indicates whether a virtual address is valid
 */
#define ARCH_PFN_OFFSET		PHYS_PFN_OFFSET

#define virt_to_page(kaddr)	pfn_to_page(virt_to_pfn(kaddr))
#define virt_addr_valid(kaddr)	(((unsigned long)(kaddr) >= PAGE_OFFSET && (unsigned long)(kaddr) < (unsigned long)high_memory) \
					&& pfn_valid(virt_to_pfn(kaddr)))

#endif

#include <asm-generic/memory_model.h>

#endif
