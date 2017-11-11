/*
 * arch/arm/kernel/topology.c
 *
 * Copyright (C) 2011 Linaro Limited.
 * Written by: Vincent Guittot
 *
 * based on arch/sh/kernel/topology.c
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <asm/cputype.h>
#include <asm/topology.h>

/*
 * cpu capacity scale management
 */

/*
 * cpu capacity table
 * This per cpu data structure describes the relative capacity of each core.
 * On a heteregenous system, cores don't have the same computation capacity
 * and we reflect that difference in the cpu_capacity field so the scheduler
 * can take this difference into account during load balance. A per cpu
 * structure is preferred because each CPU updates its own cpu_capacity field
 * during the load balance except for idle cores. One idle core is selected
 * to run the rebalance_domains for all idle cores and the cpu_capacity can be
 * updated during this sequence.
 */
static DEFINE_PER_CPU(unsigned long, cpu_scale);

unsigned long arch_scale_cpu_capacity(struct sched_domain *sd, int cpu)
{
/* IAMROOT-12:
 * -------------
 * 32비트 arm에서 산출된 cpu_scale 값을 cpu capacity로 사용한다.
 */
	return per_cpu(cpu_scale, cpu);
}

static void set_capacity_scale(unsigned int cpu, unsigned long capacity)
{
	per_cpu(cpu_scale, cpu) = capacity;
}

#ifdef CONFIG_OF
struct cpu_efficiency {
	const char *compatible;
	unsigned long efficiency;
};

/*
 * Table of relative efficiency of each processors
 * The efficiency value must fit in 20bit and the final
 * cpu_scale value must be in the range
 *   0 < cpu_scale < 3*SCHED_CAPACITY_SCALE/2
 * in order to return at most 1 when DIV_ROUND_CLOSEST
 * is used to compute the capacity of a CPU.
 * Processors that are not defined in the table,
 * use the default SCHED_CAPACITY_SCALE value for cpu_scale.
 */

/* IAMROOT-12:
 * -------------
 * cpu 성능 지표로 DMIPS와 유사한 값을 사용한다.
 * (2048 -> 1개의 클럭에서 2개의 instruction이 실행됨을 의미)
 *
 * 현재 big-little 플랫폼을 위해 다른 성능을 나타내는 cpu에 대해 
 * 상대 성능을 보여주는 값이다.
 */
static const struct cpu_efficiency table_efficiency[] = {
	{"arm,cortex-a15", 3891},
	{"arm,cortex-a7",  2048},
	{NULL, },
};

static unsigned long *__cpu_capacity;
#define cpu_capacity(cpu)	__cpu_capacity[cpu]

static unsigned long middle_capacity = 1;

/*
 * Iterate all CPUs' descriptor in DT and compute the efficiency
 * (as per table_efficiency). Also calculate a middle efficiency
 * as close as possible to  (max{eff_i} - min{eff_i}) / 2
 * This is later used to scale the cpu_capacity field such that an
 * 'average' CPU is of middle capacity. Also see the comments near
 * table_efficiency[] and update_cpu_capacity().
 */
static void __init parse_dt_topology(void)
{
	const struct cpu_efficiency *cpu_eff;
	struct device_node *cn = NULL;
	unsigned long min_capacity = ULONG_MAX;
	unsigned long max_capacity = 0;
	unsigned long capacity = 0;
	int cpu = 0;

/* IAMROOT-12:
 * -------------
 * online cpu 수만큼 usigned long 배열을 생성한다.
 * 이 곳에 cpu_capacity 값이 대입된다.
 */
	__cpu_capacity = kcalloc(nr_cpu_ids, sizeof(*__cpu_capacity),
				 GFP_NOWAIT);

	for_each_possible_cpu(cpu) {
		const u32 *rate;
		int len;

		/* too early to use cpu->of_node */

/* IAMROOT-12:
 * -------------
 * "cpu" 노드 정보를 알아온다.
 */
		cn = of_get_cpu_node(cpu, NULL);
		if (!cn) {
			pr_err("missing device node for CPU %d\n", cpu);
			continue;
		}

/* IAMROOT-12:
 * -------------
 * lookup (현재는 빅리틀을 위해 a15, a7만 제공)
 */
		for (cpu_eff = table_efficiency; cpu_eff->compatible; cpu_eff++)
			if (of_device_is_compatible(cn, cpu_eff->compatible))
				break;

/* IAMROOT-12:
 * -------------
 * 테이블이 끝나면 다음 cpu로 skip한다.
 */
		if (cpu_eff->compatible == NULL)
			continue;

/* IAMROOT-12:
 * -------------
 * "clock-frequency" 속성을 읽어온다.
 */
		rate = of_get_property(cn, "clock-frequency", &len);
		if (!rate || len != 4) {
			pr_err("%s missing clock-frequency property\n",
				cn->full_name);
			continue;
		}

/* IAMROOT-12:
 * -------------
 * cortex-a7
 *	"clock-frequency = <1000000000>;" -> rate = 1,000,000,000 = 1Ghz
 *	1,000,000,000 >> 20 = 953
 *	-> capactiy = 953 * 2048(a7) = 1,951,744 (min_capacity)
 *
 * cortex-a15
 *	"clock-frequency = <1500000000>;" -> rate = 1,500,000,000 = 1.5Ghz
 *	1,500,000,000 >> 20 = 1430
 *	-> capactiy = 1430 * 3891(a15) = 5,564,130 (max_capacity)
 */
		capacity = ((be32_to_cpup(rate)) >> 20) * cpu_eff->efficiency;

		/* Save min capacity of the system */
		if (capacity < min_capacity)
			min_capacity = capacity;

		/* Save max capacity of the system */
		if (capacity > max_capacity)
			max_capacity = capacity;

/* IAMROOT-12:
 * -------------
 * __cpu_capacity[]에 capacity 값을 대입한다.
 */
		cpu_capacity(cpu) = capacity;
	}

	/* If min and max capacities are equals, we bypass the update of the
	 * cpu_scale because all CPUs have the same capacity. Otherwise, we
	 * compute a middle_capacity factor that will ensure that the capacity
	 * of an 'average' CPU of the system will be as close as possible to
	 * SCHED_CAPACITY_SCALE, which is the default value, but with the
	 * constraint explained near table_efficiency[].
	 */

/* IAMROOT-12:
 * -------------
 * max_capacity cpu 성능과 mix_capacity cpu 성능 차이가 3배 이상 나는 경우 
 * max_capacity * 2/3 값을 middle_capacity로 사용하고, 그렇지 않은 경우 
 * 그 두 cpu의 중간으로 middle_capacity로 사용한다.
 *
 * min_capacity = 1,951,744    
 * max_capacity = 5,564,130
 * 
 * max_capacity와 min_capacity의 성능차이가 3배 이하인 경우 중간 값을 사용한다.
 * -> middle_capacity = 3,757,937 >> 10 = 3,669
 */
	if (4*max_capacity < (3*(max_capacity + min_capacity)))
		middle_capacity = (min_capacity + max_capacity)
				>> (SCHED_CAPACITY_SHIFT+1);
	else
		middle_capacity = ((max_capacity / 3)
				>> (SCHED_CAPACITY_SHIFT-1)) + 1;

}

/*
 * Look for a customed capacity of a CPU in the cpu_capacity table during the
 * boot. The update of all CPUs is in O(n^2) for heteregeneous system but the
 * function returns directly for SMP system.
 */
static void update_cpu_capacity(unsigned int cpu)
{
	if (!cpu_capacity(cpu))
		return;

/* IAMROOT-12:
 * -------------
 * per_cpu cpu_scale = __cpu_capacity[cpu] / middle_capacity 
 *
 * 예) cortex-a7 1Ghz x 2, cortex-a15 1.5Hz x 2
 *
 * cpu#0: cpu_scale = 1,951,744 / 3,669 =  531
 * cpu#2: cpu_scale = 5,564,130 / 3,669 = 1,516
 */
	set_capacity_scale(cpu, cpu_capacity(cpu) / middle_capacity);

	pr_info("CPU%u: update cpu_capacity %lu\n",
		cpu, arch_scale_cpu_capacity(NULL, cpu));
}

#else
static inline void parse_dt_topology(void) {}
static inline void update_cpu_capacity(unsigned int cpuid) {}
#endif

 /*
 * cpu topology table
 */
struct cputopo_arm cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

const struct cpumask *cpu_coregroup_mask(int cpu)
{
	return &cpu_topology[cpu].core_sibling;
}

/*
 * The current assumption is that we can power gate each core independently.
 * This will be superseded by DT binding once available.
 */
const struct cpumask *cpu_corepower_mask(int cpu)
{
	return &cpu_topology[cpu].thread_sibling;
}

static void update_siblings_masks(unsigned int cpuid)
{
	struct cputopo_arm *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];
	int cpu;

	/* update core and thread sibling masks */
	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

/* IAMROOT-12:
 * -------------
 * arm에서는 클러스터 id라고 인식해야 한다.
 */
		if (cpuid_topo->socket_id != cpu_topo->socket_id)
			continue;

/* IAMROOT-12:
 * -------------
 * 같은 클러스터인 경우 core_sibling에 해당 cpu의 비트를 추가한다.
 * (자기 자신의 클러스터는 항상 자신의 코어 번호를 1로 설정한다.)
 */
		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);

/* IAMROOT-12:
 * -------------
 * 같은 코어인 경우 thread_sibling에 해당 cpu의 비트를 추가한다.
 * (자기 자신의 core는 항상 자신의 thread 번호를 1로 설정한다.)
 */
		if (cpuid_topo->core_id != cpu_topo->core_id)
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);
		if (cpu != cpuid)
			cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);
	}
	smp_wmb();
}

/*
 * store_cpu_topology is called at boot when only one cpu is running
 * and with the mutex cpu_hotplug.lock locked, when several cpus have booted,
 * which prevents simultaneous write access to cpu_topology array
 */
void store_cpu_topology(unsigned int cpuid)
{
	struct cputopo_arm *cpuid_topo = &cpu_topology[cpuid];
	unsigned int mpidr;

	/* If the cpu topology has been already set, just return */
	if (cpuid_topo->core_id != -1)
		return;

/* IAMROOT-12:
 * -------------
 * 해당 cpu의 mpidr 레지스터를 읽는다. (cpu affinity level을 읽어들인다)
 */
	mpidr = read_cpuid_mpidr();

	/* create cpu topology mapping */
	if ((mpidr & MPIDR_SMP_BITMASK) == MPIDR_SMP_VALUE) {
		/*
		 * This is a multiprocessor system
		 * multiprocessor format & multiprocessor mode field are set
		 */

/* IAMROOT-12:
 * -------------
 * h/w muti-thread를 지원하는지 여부를 판단(현재 지원하지 않음)
 * 3단계 affinity level 값을 읽어온다.
 */
		if (mpidr & MPIDR_MT_BITMASK) {
			/* core performance interdependency */
			cpuid_topo->thread_id = MPIDR_AFFINITY_LEVEL(mpidr, 0);
			cpuid_topo->core_id = MPIDR_AFFINITY_LEVEL(mpidr, 1);
			cpuid_topo->socket_id = MPIDR_AFFINITY_LEVEL(mpidr, 2);
		} else {
			/* largely independent cores */

/* IAMROOT-12:
 * -------------
 * 2단계 affinity level 값을 읽어온다.
 */
			cpuid_topo->thread_id = -1;
			cpuid_topo->core_id = MPIDR_AFFINITY_LEVEL(mpidr, 0);
			cpuid_topo->socket_id = MPIDR_AFFINITY_LEVEL(mpidr, 1);
		}
	} else {
		/*
		 * This is an uniprocessor system
		 * we are in multiprocessor format but uniprocessor system
		 * or in the old uniprocessor format
		 */
		cpuid_topo->thread_id = -1;
		cpuid_topo->core_id = 0;
		cpuid_topo->socket_id = -1;
	}

/* IAMROOT-12:
 * -------------
 * mpidr을 읽어서 core_sibling만 갱신한다.
 *
 * 처음 cpu#0이 online되어 이 함수가 호출되었을 때,
 * cpu_topology[0] = { .thread_id=-1, .core_id=0, .socket_id=0,
 *                     .thread_sibling=0b0001, .core_sibling=0b0001 }
 * cpu_topology[1] = { .thread_id=-1, .core_id=-1,.socket_id=-1,
 *                     .thread_sibling=0, .core_sibling=0 }
 * cpu_topology[2] = { .thread_id=-1, .core_id=-1,.socket_id=-1,
 *                     .thread_sibling=0, .core_sibling=0 }
 * cpu_topology[3] = { .thread_id=-1, .core_id=-1,.socket_id=-1,
 *                     .thread_sibling=0, .core_sibling=0 }
 *
 * 두 번째 cpu#1이 online되면  
 * cpu_topology[0] = { .thread_id=-1, .core_id=0, .socket_id=0,
 *                     .thread_sibling=0b0001, .core_sibling=0b0011 }
 * cpu_topology[1] = { .thread_id=-1, .core_id=1,.socket_id=0,
 *                     .thread_sibling=0b0010, .core_sibling=0b0011 }
 * cpu_topology[2] = { .thread_id=-1, .core_id=-1,.socket_id=-1,
 *                     .thread_sibling=0, .core_sibling=0 }
 * cpu_topology[3] = { .thread_id=-1, .core_id=-1,.socket_id=-1,
 *                     .thread_sibling=0, .core_sibling=0 }
 *
 * ...
 *
 * 네 번째 cpu#3이 online되면  
 * cpu_topology[0] = { .thread_id=-1, .core_id=0, .socket_id=0,
 *                     .thread_sibling=0b0001, .core_sibling=0b1111 }
 * cpu_topology[1] = { .thread_id=-1, .core_id=1,.socket_id=0,
 *                     .thread_sibling=0b0010, .core_sibling=0b1111 }
 * cpu_topology[2] = { .thread_id=-1, .core_id=2,.socket_id=0,
 *                     .thread_sibling=0b0100, .core_sibling=0b1111 }
 * cpu_topology[3] = { .thread_id=-1, .core_id=3,.socket_id=0,
 *                     .thread_sibling=0b1000, .core_sibling=0b1111 }
 */
	update_siblings_masks(cpuid);

	update_cpu_capacity(cpuid);

	pr_info("CPU%u: thread %d, cpu %d, socket %d, mpidr %x\n",
		cpuid, cpu_topology[cpuid].thread_id,
		cpu_topology[cpuid].core_id,
		cpu_topology[cpuid].socket_id, mpidr);
}

static inline int cpu_corepower_flags(void)
{
	return SD_SHARE_PKG_RESOURCES  | SD_SHARE_POWERDOMAIN;
}

/* IAMROOT-12:
 * -------------
 * 32비트 arm 커널의 경우 default topology를 사용하지 않고, 
 * 별도로 제공되는 arm_topology[]를 사용한다.
 *
 * 기본 DIE 단계만 사용하고, 빅/리틀 클러스터가 동시에 동작해야 하는 경우와
 * 전원 클러스터 제어를 위해 두 개의 단계가 추가된다.
 *
 * GMC -> MC -> DIE 
 *
 * arm64 및 대부분의 아키텍처에서는 아래의 default_topology를 사용한다.
 */
static struct sched_domain_topology_level arm_topology[] = {
#ifdef CONFIG_SCHED_MC
	{ cpu_corepower_mask, cpu_corepower_flags, SD_INIT_NAME(GMC) },
	{ cpu_coregroup_mask, cpu_core_flags, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

/*
 * init_cpu_topology is called at boot when only one cpu is running
 * which prevent simultaneous write access to cpu_topology array
 */
void __init init_cpu_topology(void)
{
	unsigned int cpu;

	/* init core mask and capacity */
	for_each_possible_cpu(cpu) {
		struct cputopo_arm *cpu_topo = &(cpu_topology[cpu]);

		cpu_topo->thread_id = -1;
		cpu_topo->core_id =  -1;
		cpu_topo->socket_id = -1;
		cpumask_clear(&cpu_topo->core_sibling);
		cpumask_clear(&cpu_topo->thread_sibling);

/* IAMROOT-12:
 * -------------
 * 초기값으로 cpu_capacity=1024를 지정한다.
 */
		set_capacity_scale(cpu, SCHED_CAPACITY_SCALE);
	}
	smp_wmb();

/* IAMROOT-12:
 * -------------
 * "clock-frequency" 속성 * table_efficiency 테이블을 사용하여 
 * __cpu_capacity[] 와 middle_capacity를 산출한다.
 */
	parse_dt_topology();

	/* Set scheduler topology descriptor */

/* IAMROOT-12:
 * -------------
 * arm은 default_topology를 사용하지 않고 arm_topology를 사용한다.
 * (arm64는 default_topology를 사용한다)
 */

	set_sched_topology(arm_topology);
}
