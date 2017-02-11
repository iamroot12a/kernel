/*
 *  linux/arch/arm/kernel/smp_tlb.c
 *
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/preempt.h>
#include <linux/smp.h>

#include <asm/smp_plat.h>
#include <asm/tlbflush.h>
#include <asm/mmu_context.h>

/**********************************************************************/

/*
 * TLB operations
 */
struct tlb_args {
	struct vm_area_struct *ta_vma;
	unsigned long ta_start;
	unsigned long ta_end;
};

static inline void ipi_flush_tlb_all(void *ignored)
{
	local_flush_tlb_all();
}

static inline void ipi_flush_tlb_mm(void *arg)
{
	struct mm_struct *mm = (struct mm_struct *)arg;

	local_flush_tlb_mm(mm);
}

static inline void ipi_flush_tlb_page(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_page(ta->ta_vma, ta->ta_start);
}

static inline void ipi_flush_tlb_kernel_page(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_kernel_page(ta->ta_start);
}

static inline void ipi_flush_tlb_range(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_range(ta->ta_vma, ta->ta_start, ta->ta_end);
}

static inline void ipi_flush_tlb_kernel_range(void *arg)
{
	struct tlb_args *ta = (struct tlb_args *)arg;

	local_flush_tlb_kernel_range(ta->ta_start, ta->ta_end);
}

static inline void ipi_flush_bp_all(void *ignored)
{
	local_flush_bp_all();
}

#ifdef CONFIG_ARM_ERRATA_798181
bool (*erratum_a15_798181_handler)(void);

static bool erratum_a15_798181_partial(void)
{
	asm("mcr p15, 0, %0, c8, c3, 1" : : "r" (0));
	dsb(ish);
	return false;
}

static bool erratum_a15_798181_broadcast(void)
{
	asm("mcr p15, 0, %0, c8, c3, 1" : : "r" (0));
	dsb(ish);
	return true;
}

/* IAMROOT-12A:
 * ------------
 * CONFIG_ARM_ERRATA_798181이 설정된 경우
 * 특정 아키텍처 리비전 파트에서 전역 변수 erratum_a15_798181_handler에
 * erratum_a15_798181_partial 또는 erratum_a15_798181_broadcast 함수를
 * 대입하여 놓는다.
 *
 * 위의 핸들러는 TLB 캐시 관련한 코드에서 호출된다. (kernel/smp_tlb.c)
 */


void erratum_a15_798181_init(void)
{
	unsigned int midr = read_cpuid_id();
	unsigned int revidr = read_cpuid(CPUID_REVIDR);

	/* Brahma-B15 r0p0..r0p2 affected
	 * Cortex-A15 r0p0..r3p2 w/o ECO fix affected */
	if ((midr & 0xff0ffff0) == 0x420f00f0 && midr <= 0x420f00f2)
		erratum_a15_798181_handler = erratum_a15_798181_broadcast;
	else if ((midr & 0xff0ffff0) == 0x410fc0f0 && midr <= 0x413fc0f2 &&
		 (revidr & 0x210) != 0x210) {
		if (revidr & 0x10)
			erratum_a15_798181_handler =
				erratum_a15_798181_partial;
		else
			erratum_a15_798181_handler =
				erratum_a15_798181_broadcast;
	}
}
#endif

static void ipi_flush_tlb_a15_erratum(void *arg)
{
	dmb();
}

static void broadcast_tlb_a15_erratum(void)
{
	if (!erratum_a15_798181())
		return;

	smp_call_function(ipi_flush_tlb_a15_erratum, NULL, 1);
}

static void broadcast_tlb_mm_a15_erratum(struct mm_struct *mm)
{
	int this_cpu;
	cpumask_t mask = { CPU_BITS_NONE };

	if (!erratum_a15_798181())
		return;

	this_cpu = get_cpu();
	a15_erratum_get_cpumask(this_cpu, mm, &mask);
	smp_call_function_many(&mask, ipi_flush_tlb_a15_erratum, NULL, 1);
	put_cpu();
}

void flush_tlb_all(void)
{
	if (tlb_ops_need_broadcast())
		on_each_cpu(ipi_flush_tlb_all, NULL, 1);
	else
		__flush_tlb_all();
	broadcast_tlb_a15_erratum();
}

void flush_tlb_mm(struct mm_struct *mm)
{
	if (tlb_ops_need_broadcast())
		on_each_cpu_mask(mm_cpumask(mm), ipi_flush_tlb_mm, mm, 1);
	else
		__flush_tlb_mm(mm);
	broadcast_tlb_mm_a15_erratum(mm);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long uaddr)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_vma = vma;
		ta.ta_start = uaddr;
		on_each_cpu_mask(mm_cpumask(vma->vm_mm), ipi_flush_tlb_page,
					&ta, 1);
	} else
		__flush_tlb_page(vma, uaddr);
	broadcast_tlb_mm_a15_erratum(vma->vm_mm);
}

void flush_tlb_kernel_page(unsigned long kaddr)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_start = kaddr;
		on_each_cpu(ipi_flush_tlb_kernel_page, &ta, 1);
	} else
		__flush_tlb_kernel_page(kaddr);
	broadcast_tlb_a15_erratum();
}

void flush_tlb_range(struct vm_area_struct *vma,
                     unsigned long start, unsigned long end)
{
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_vma = vma;
		ta.ta_start = start;
		ta.ta_end = end;
		on_each_cpu_mask(mm_cpumask(vma->vm_mm), ipi_flush_tlb_range,
					&ta, 1);
	} else
		local_flush_tlb_range(vma, start, end);
	broadcast_tlb_mm_a15_erratum(vma->vm_mm);
}


/* IAMROOT-12AB:
 * -------------
 * 해당 영역에 대한 TLB cache를 flush 한다.
 */
void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
/* IAMROOT-12AB:
 * -------------
 * ARMv7의 경우 tlb_ops_need_broadcast() 함수는 false를 반환한다.
 * (아키텍처 broadcast하기 때문에 sw에서는 코어마다 루프를 돌며 
 * 수행할 필요 없이 TLBIMVAIS 명령을 사용하여 inner share 영역에 있는
 * 다른 cpu의 TLB도 flush 하도록 broadcast 한다)
 *
 * smp 머신에서 한 cpu에서 TLB를 flush할 때 다른 cpu에 대해서도
 * TLB를 같이 flush하도록 할 때 사용한다
 *
 * IPI(Inter Processor Interrupt): 다른 cpu를 여러 용도로 소프트 인터럽트를 
 * 걸때 사용
 */
	if (tlb_ops_need_broadcast()) {
		struct tlb_args ta;
		ta.ta_start = start;
		ta.ta_end = end;
		on_each_cpu(ipi_flush_tlb_kernel_range, &ta, 1);
	} else

/* IAMROOT-12AB:
 * -------------
 * ARMv7: v7wbi_flush_kern_tlb_range()
 */
		local_flush_tlb_kernel_range(start, end);
	broadcast_tlb_a15_erratum();
}

void flush_bp_all(void)
{
	if (tlb_ops_need_broadcast())
		on_each_cpu(ipi_flush_bp_all, NULL, 1);
	else
		__flush_bp_all();
}
