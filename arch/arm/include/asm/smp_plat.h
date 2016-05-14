/*
 * ARM specific SMP header, this contains our implementation
 * details.
 */
#ifndef __ASMARM_SMP_PLAT_H
#define __ASMARM_SMP_PLAT_H

#include <linux/cpumask.h>
#include <linux/err.h>

#include <asm/cpu.h>
#include <asm/cputype.h>

/*
 * Return true if we are running on a SMP platform
 */
static inline bool is_smp(void)
{
#ifndef CONFIG_SMP
	return false;
#elif defined(CONFIG_SMP_ON_UP)
/* IAMROOT-12A:
 * ------------
 * fixup_smp 루틴에 의해 실제 SMP 상에서 실행될 때 true가 되고
 * up에서 실행될 때에는 false가 된다.
 */
	extern unsigned int smp_on_up;
	return !!smp_on_up;
#else
	return true;
#endif
}

/**
 * smp_cpuid_part() - return part id for a given cpu
 * @cpu:	logical cpu id.
 *
 * Return: part id of logical cpu passed as argument.
 */
static inline unsigned int smp_cpuid_part(int cpu)
{
	struct cpuinfo_arm *cpu_info = &per_cpu(cpu_data, cpu);

	return is_smp() ? cpu_info->cpuid & ARM_CPU_PART_MASK :
			  read_cpuid_part();
}

/* all SMP configurations have the extended CPUID registers */
#ifndef CONFIG_MMU
#define tlb_ops_need_broadcast()	0
#else

/* IAMROOT-12AB:
 * -------------
 * ARMv7의 경우 MMFR3.Maintenance_broadcast=2이므로 이 함수는 false를 반환한다.
 * 값이 0인 경우는 Cache, BP, TLB 조작 시 local cpu에만 적용된다.
 *      1인 경우는 Cache, BP 조작 시 다른 cpu에도 broadcast되어 동시에 조작되는데
 *                 TLB 조작의 경우만 local cpu에 적용된다.
 *      2인 경우는 Cache, BP, TLB 조작 시 다른 cpu에도 broadcast되어 동시에 조작된다.
 */
static inline int tlb_ops_need_broadcast(void)
{
	if (!is_smp())
		return 0;

	return ((read_cpuid_ext(CPUID_EXT_MMFR3) >> 12) & 0xf) < 2;
}
#endif

#if !defined(CONFIG_SMP) || __LINUX_ARM_ARCH__ >= 7
#define cache_ops_need_broadcast()	0
#else
static inline int cache_ops_need_broadcast(void)
{
	if (!is_smp())
		return 0;

	return ((read_cpuid_ext(CPUID_EXT_MMFR3) >> 12) & 0xf) < 1;
}
#endif

/*
 * Logical CPU mapping.
 */
extern u32 __cpu_logical_map[];
#define cpu_logical_map(cpu)	__cpu_logical_map[cpu]
/*
 * Retrieve logical cpu index corresponding to a given MPIDR[23:0]
 *  - mpidr: MPIDR[23:0] to be used for the look-up
 *
 * Returns the cpu logical index or -EINVAL on look-up error
 */
static inline int get_logical_index(u32 mpidr)
{
	int cpu;
	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		if (cpu_logical_map(cpu) == mpidr)
			return cpu;
	return -EINVAL;
}

/*
 * NOTE ! Assembly code relies on the following
 * structure memory layout in order to carry out load
 * multiple from its base address. For more
 * information check arch/arm/kernel/sleep.S
 */
struct mpidr_hash {
	u32	mask; /* used by sleep.S */
	u32	shift_aff[3]; /* used by sleep.S */
	u32	bits;
};

extern struct mpidr_hash mpidr_hash;

static inline u32 mpidr_hash_size(void)
{
	return 1 << mpidr_hash.bits;
}

extern int platform_can_cpu_hotplug(void);

#endif
