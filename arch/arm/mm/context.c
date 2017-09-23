/*
 *  linux/arch/arm/mm/context.c
 *
 *  Copyright (C) 2002-2003 Deep Blue Solutions Ltd, all rights reserved.
 *  Copyright (C) 2012 ARM Limited
 *
 *  Author: Will Deacon <will.deacon@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/percpu.h>

#include <asm/mmu_context.h>
#include <asm/smp_plat.h>
#include <asm/thread_notify.h>
#include <asm/tlbflush.h>
#include <asm/proc-fns.h>

/*
 * On ARMv6, we have the following structure in the Context ID:
 *
 * 31                         7          0
 * +-------------------------+-----------+
 * |      process ID         |   ASID    |
 * +-------------------------+-----------+
 * |              context ID             |
 * +-------------------------------------+
 *
 * The ASID is used to tag entries in the CPU caches and TLBs.
 * The context ID is used by debuggers and trace logic, and
 * should be unique within all running processes.
 *
 * In big endian operation, the two 32 bit words are swapped if accessed
 * by non-64-bit operations.
 */
#define ASID_FIRST_VERSION	(1ULL << ASID_BITS)
#define NUM_USER_ASIDS		ASID_FIRST_VERSION

static DEFINE_RAW_SPINLOCK(cpu_asid_lock);
static atomic64_t asid_generation = ATOMIC64_INIT(ASID_FIRST_VERSION);

/* IAMROOT-12:
 * -------------
 * arm: 256 비트의 asid_map을 구성한다.
 */
static DECLARE_BITMAP(asid_map, NUM_USER_ASIDS);

static DEFINE_PER_CPU(atomic64_t, active_asids);
static DEFINE_PER_CPU(u64, reserved_asids);
static cpumask_t tlb_flush_pending;

#ifdef CONFIG_ARM_ERRATA_798181
void a15_erratum_get_cpumask(int this_cpu, struct mm_struct *mm,
			     cpumask_t *mask)
{
	int cpu;
	unsigned long flags;
	u64 context_id, asid;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	context_id = mm->context.id.counter;
	for_each_online_cpu(cpu) {
		if (cpu == this_cpu)
			continue;
		/*
		 * We only need to send an IPI if the other CPUs are
		 * running the same ASID as the one being invalidated.
		 */
		asid = per_cpu(active_asids, cpu).counter;
		if (asid == 0)
			asid = per_cpu(reserved_asids, cpu);
		if (context_id == asid)
			cpumask_set_cpu(cpu, mask);
	}
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);
}
#endif

#ifdef CONFIG_ARM_LPAE
/*
 * With LPAE, the ASID and page tables are updated atomicly, so there is
 * no need for a reserved set of tables (the active ASID tracking prevents
 * any issues across a rollover).
 */
#define cpu_set_reserved_ttbr0()
#else
static void cpu_set_reserved_ttbr0(void)
{

/* IAMROOT-12:
 * -------------
 * TTBR1 레지스터 값을 TTBR0로 복사한다.
 * (TTBR1에는 커널에서 사용하는 페이지 테이블(pgd)의 base 주소가 담겨있다.)
 */
	u32 ttb;
	/*
	 * Copy TTBR1 into TTBR0.
	 * This points at swapper_pg_dir, which contains only global
	 * entries so any speculative walks are perfectly safe.
	 */
	asm volatile(
	"	mrc	p15, 0, %0, c2, c0, 1		@ read TTBR1\n"
	"	mcr	p15, 0, %0, c2, c0, 0		@ set TTBR0\n"
	: "=r" (ttb));
	isb();
}
#endif

#ifdef CONFIG_PID_IN_CONTEXTIDR
static int contextidr_notifier(struct notifier_block *unused, unsigned long cmd,
			       void *t)
{
	u32 contextidr;
	pid_t pid;
	struct thread_info *thread = t;

	if (cmd != THREAD_NOTIFY_SWITCH)
		return NOTIFY_DONE;

	pid = task_pid_nr(thread->task) << ASID_BITS;
	asm volatile(
	"	mrc	p15, 0, %0, c13, c0, 1\n"
	"	and	%0, %0, %2\n"
	"	orr	%0, %0, %1\n"
	"	mcr	p15, 0, %0, c13, c0, 1\n"
	: "=r" (contextidr), "+r" (pid)
	: "I" (~ASID_MASK));
	isb();

	return NOTIFY_OK;
}

static struct notifier_block contextidr_notifier_block = {
	.notifier_call = contextidr_notifier,
};

static int __init contextidr_notifier_init(void)
{
	return thread_register_notifier(&contextidr_notifier_block);
}
arch_initcall(contextidr_notifier_init);
#endif

static void flush_context(unsigned int cpu)
{
	int i;
	u64 asid;

	/* Update the list of reserved ASIDs and the ASID bitmap. */
	bitmap_clear(asid_map, 0, NUM_USER_ASIDS);
	for_each_possible_cpu(i) {
		asid = atomic64_xchg(&per_cpu(active_asids, i), 0);
		/*
		 * If this CPU has already been through a
		 * rollover, but hasn't run another task in
		 * the meantime, we must preserve its reserved
		 * ASID, as this is the only trace we have of
		 * the process it is still running.
		 */
		if (asid == 0)
			asid = per_cpu(reserved_asids, i);
		__set_bit(asid & ~ASID_MASK, asid_map);
		per_cpu(reserved_asids, i) = asid;
	}

	/* Queue a TLB invalidate and flush the I-cache if necessary. */
	cpumask_setall(&tlb_flush_pending);

	if (icache_is_vivt_asid_tagged())
		__flush_icache_all();
}

static int is_reserved_asid(u64 asid)
{

/* IAMROOT-12:
 * -------------
 * cpu에 이미 예약된 asid인 경우 true를 반환한다.
 * 전체 cpu를 검색한다.
 */
	int cpu;
	for_each_possible_cpu(cpu)
		if (per_cpu(reserved_asids, cpu) == asid)
			return 1;
	return 0;
}

static u64 new_context(struct mm_struct *mm, unsigned int cpu)
{
	static u32 cur_idx = 1;
	u64 asid = atomic64_read(&mm->context.id);
	u64 generation = atomic64_read(&asid_generation);

	if (asid != 0) {
		/*
		 * If our current ASID was active during a rollover, we
		 * can continue to use it and this was just a false alarm.
		 */
/* IAMROOT-12:
 * -------------
 * cpu에 이미 예약된 asid인 경우 대역번호만 바꿔서 반환한다.
 *
 * asid_generation      |     mm->context.id & 0xff
 *   0x400              |              0x380 & 0xff
 *   0x400              |               0x80
 *                    0x480 
 */
		if (is_reserved_asid(asid))
			return generation | (asid & ~ASID_MASK);

		/*
		 * We had a valid ASID in a previous life, so try to re-use
		 * it if possible.,
		 */

/* IAMROOT-12:
 * -------------
 * asid_map에 해당 비트가 비어있어 배치가 가능한 asid인 경우 해당 비트를 
 * 설정하고 bump_gen으로 이동
 *
 * 예) asid=0x380 -> asid_map에서 0x80 비트를 검사한다.
 */
		asid &= ~ASID_MASK;
		if (!__test_and_set_bit(asid, asid_map))
			goto bump_gen;
	}

	/*
	 * Allocate a free ASID. If we can't find one, take a note of the
	 * currently active ASIDs and mark the TLBs as requiring flushes.
	 * We always count from ASID #1, as we reserve ASID #0 to switch
	 * via TTBR0 and to avoid speculative page table walks from hitting
	 * in any partial walk caches, which could be populated from
	 * overlapping level-1 descriptors used to map both the module
	 * area and the userspace stack.
	 */

/* IAMROOT-12:
 * -------------
 * 비어있는 번호를 찾는다.
 */
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);

/* IAMROOT-12:
 * -------------
 * 256개가 모두 할당되어 비어 있는 번호가 없으면 
 */
	if (asid == NUM_USER_ASIDS) {

/* IAMROOT-12:
 * -------------
 * 최근 발급한 번호에 256을 더해 새로운 대역번호를 설정한다.
 * 예) 0x300 -> 0x400
 */
		generation = atomic64_add_return(ASID_FIRST_VERSION,
						 &asid_generation);
/* IAMROOT-12:
 * -------------
 * tlb 및 icache(vivt 타입만) 플러싱을 수행한다.
 */
		flush_context(cpu);
		asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);
	}

	__set_bit(asid, asid_map);
	cur_idx = asid;

bump_gen:
	asid |= generation;
	cpumask_clear(mm_cpumask(mm));
	return asid;
}

void check_and_switch_context(struct mm_struct *mm, struct task_struct *tsk)
{
	unsigned long flags;
	unsigned int cpu = smp_processor_id();
	u64 asid;

/* IAMROOT-12:
 * -------------
 * vmalloc 매핑이 변경된 경우 vmalloc에 해당하는 커널용 pgd 엔트리(arm:120개)를
 * 복사하여 현재 mm에 갱신한다.
 */
	if (unlikely(mm->context.vmalloc_seq != init_mm.context.vmalloc_seq))
		__check_vmalloc_seq(mm);

	/*
	 * We cannot update the pgd and the ASID atomicly with classic
	 * MMU, so switch exclusively to global mappings to avoid
	 * speculative page table walking with the wrong TTBR.
	 */
/* IAMROOT-12:
 * -------------
 * TTBR1 레지스터 값을 TTBR0로 복사한다.
 * (TTBR1에는 커널에서 사용하는 페이지 테이블(pgd)의 base 주소가 담겨있다.)
 *
 * 최근 asid 대역번호와 스위칭될 태스크의 asid가 같은 대역(8비트)에 있는 경우 
 * fastpath를 사용한다.
 *
 * 예) asid_generation=0x300, mm->context.id=0x340
 *     -> 각각 8비트를 우측 shift한 결과가 같으므로 fastpath 
 *
 * 예) asid_generation=0x400, mm->context.id=0x3f0
 *     -> 각각 8비트를 우측 shift한 결과가 다르므로 slowpath 
 */
	cpu_set_reserved_ttbr0();

	asid = atomic64_read(&mm->context.id);
	if (!((asid ^ atomic64_read(&asid_generation)) >> ASID_BITS)
	    && atomic64_xchg(&per_cpu(active_asids, cpu), asid))
		goto switch_mm_fastpath;

/* IAMROOT-12:
 * -------------
 * mm 스위칭 slow-path
 */
	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	/* Check that our ASID belongs to the current generation. */

/* IAMROOT-12:
 * -------------
 * 최근 asid 대역 번호(asid_generation)와 태스크의 asid 대역이 다른 경우 
 * 다시 asid를 발급받아 mm->context.id에 저장한다.
 * (새로 받은 asid는 최근 발급한 대역대로 변경된다.)
 */
	asid = atomic64_read(&mm->context.id);
	if ((asid ^ atomic64_read(&asid_generation)) >> ASID_BITS) {
		asid = new_context(mm, cpu);
		atomic64_set(&mm->context.id, asid);
	}

/* IAMROOT-12:
 * -------------
 * tlb 플러싱이 지연된 경우 branch predict 캐시와 tlb 캐시를 모두 플러시한다.
 * - flush_context()함수에서 설정한다.
 */
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending)) {
		local_flush_bp_all();
		local_flush_tlb_all();
	}

	atomic64_set(&per_cpu(active_asids, cpu), asid);
	cpumask_set_cpu(cpu, mm_cpumask(mm));
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

switch_mm_fastpath:

/* IAMROOT-12:
 * -------------
 * 해당 아키텍처의 swtich_mm 함수를 호출한다. cpu_v7_switch_mm()
 *
 * CONTEXTIDR <- mm->context.id 
 * TTBR0 <- mm->pgd | #TTB_FLAGS_SMP
 */
	cpu_switch_mm(mm->pgd, mm);
}
