#ifndef _ASM_ARM_TOPOLOGY_H
#define _ASM_ARM_TOPOLOGY_H

/* IAMROOT-12:
 * -------------
 * MPIDR 레지스터를 읽어와서 cpu topology를 생성한다.
 */
#ifdef CONFIG_ARM_CPU_TOPOLOGY

#include <linux/cpumask.h>

/* IAMROOT-12:
 * -------------
 * arm용 cpu topology에 사용하는 구조체
 */
struct cputopo_arm {

/* IAMROOT-12:
 * -------------
 * arm에서 h/w 스레드를 지원하지 않으므로 thread_id는 사용하지 않는다. (-1)
 */
	int thread_id;

/* IAMROOT-12:
 * -------------
 * MPIDR 레지스터를 통해 
 *      affinity level 0 (core number) -> core_id 
 *      affinity level 1 (cluster number) -> socket_id
 */
	int core_id;
	int socket_id;
	cpumask_t thread_sibling;
	cpumask_t core_sibling;
};

extern struct cputopo_arm cpu_topology[NR_CPUS];

#define topology_physical_package_id(cpu)	(cpu_topology[cpu].socket_id)
#define topology_core_id(cpu)		(cpu_topology[cpu].core_id)
#define topology_core_cpumask(cpu)	(&cpu_topology[cpu].core_sibling)
#define topology_thread_cpumask(cpu)	(&cpu_topology[cpu].thread_sibling)

void init_cpu_topology(void);
void store_cpu_topology(unsigned int cpuid);
const struct cpumask *cpu_coregroup_mask(int cpu);

#else

static inline void init_cpu_topology(void) { }
static inline void store_cpu_topology(unsigned int cpuid) { }

#endif

#include <asm-generic/topology.h>

#endif /* _ASM_ARM_TOPOLOGY_H */
