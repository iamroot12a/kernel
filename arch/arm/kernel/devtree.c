/*
 *  linux/arch/arm/kernel/devtree.c
 *
 *  Copyright (C) 2009 Canonical Ltd. <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/smp.h>

#include <asm/cputype.h>
#include <asm/setup.h>
#include <asm/page.h>
#include <asm/smp_plat.h>
#include <asm/mach/arch.h>
#include <asm/mach-types.h>


#ifdef CONFIG_SMP
extern struct of_cpu_method __cpu_method_of_table[];

static const struct of_cpu_method __cpu_method_of_table_sentinel
	__used __section(__cpu_method_of_table_end);


static int __init set_smp_ops_by_method(struct device_node *node)
{
	const char *method;
	struct of_cpu_method *m = __cpu_method_of_table;

/* IAMROOT-12AB:
 * -------------
 * 예: bcm7445.dtsi
 *	cpu@0 {
 *		compatible = "brcm,brahma-b15";
 *              device_type = "cpu";
 *              enable-method = "brcm,brahma-b15";
 *              reg = <0>;
 *      }
 *
 * 아래와 같이 CPU_METHOD_OF_DECLARE() 매크로로 __cpu_method_of_table[]에 등록한다.
 * 예: arch/arm/mach-rockchip/platsmp.c
 * CPU_METHOD_OF_DECLARE(rk3066_smp, "rockchip,rk3066-smp", &rockchip_smp_ops);    
 *	static struct smp_operations rockchip_smp_ops __initdata = { 
 *		.smp_prepare_cpus       = rockchip_smp_prepare_cpus,
 *              .smp_boot_secondary     = rockchip_boot_secondary,
 *      #ifdef CONFIG_HOTPLUG_CPU
 *              .cpu_kill               = rockchip_cpu_kill,
 *              .cpu_die                = rockchip_cpu_die,
 *      #endif
 *      };
 */
	if (of_property_read_string(node, "enable-method", &method))
		return 0;

/* IAMROOT-12AB:
 * -------------
 * __cpu_method_of_table에 등록된 cpu method 이름과 비교하여 찾아서 ops를 SMP에 대입한다.
 */
	for (; m->method; m++)
		if (!strcmp(m->method, method)) {
			smp_set_ops(m->ops);
			return 1;
		}

	return 0;
}
#else
static inline int set_smp_ops_by_method(struct device_node *node)
{
	return 1;
}
#endif


/*
 * arm_dt_init_cpu_maps - Function retrieves cpu nodes from the device tree
 * and builds the cpu logical map array containing MPIDR values related to
 * logical cpus
 *
 * Updates the cpu possible mask with the number of parsed cpu nodes
 */
void __init arm_dt_init_cpu_maps(void)
{
	/*
	 * Temp logical map is initialized with UINT_MAX values that are
	 * considered invalid logical map entries since the logical map must
	 * contain a list of MPIDR[23:0] values where MPIDR[31:24] must
	 * read as 0.
	 */
	struct device_node *cpu, *cpus;
	int found_method = 0;
	u32 i, j, cpuidx = 1;

/* IAMROOT-12AB:
 * -------------
 * MPIDR은 ARM 아키텍처 버전에 따라 implementation이 다르다.
 * ARMv7에서는 affinity level 0가 cpu, 1은 cluster, 2는 사용하지 않는다.
 */
	u32 mpidr = is_smp() ? read_cpuid_mpidr() & MPIDR_HWID_BITMASK : 0;

	u32 tmp_map[NR_CPUS] = { [0 ... NR_CPUS-1] = MPIDR_INVALID };
	bool bootcpu_valid = false;

/* IAMROOT-12AB:
 * -------------
 * unflatten된 디바이스 트리 object에서 /cpus 노드 구조체를 찾아온다.
 */
	cpus = of_find_node_by_path("/cpus");

	if (!cpus)
		return;

/* IAMROOT-12AB:
 * -------------
 * 루프를 돌며 cpus의 child 노드를 cpu에 담아온다.
 */
	for_each_child_of_node(cpus, cpu) {
		u32 hwid;

/* IAMROOT-12AB:
 * -------------
 * child 노드의 타입이 "cpu"가 아니면 skip
 */
		if (of_node_cmp(cpu->type, "cpu"))
			continue;

		pr_debug(" * %s...\n", cpu->full_name);
		/*
		 * A device tree containing CPU nodes with missing "reg"
		 * properties is considered invalid to build the
		 * cpu_logical_map.
		 */

/* IAMROOT-12AB:
 * -------------
 * cpu 노드에서 "reg" 속성 값을 hwid에 읽어온다.
 */
		if (of_property_read_u32(cpu, "reg", &hwid)) {
			pr_debug(" * %s missing reg property\n",
				     cpu->full_name);
			return;
		}

		/*
		 * 8 MSBs must be set to 0 in the DT since the reg property
		 * defines the MPIDR[23:0].
		 */
		if (hwid & ~MPIDR_HWID_BITMASK)
			return;

		/*
		 * Duplicate MPIDRs are a recipe for disaster.
		 * Scan all initialized entries and check for
		 * duplicates. If any is found just bail out.
		 * temp values were initialized to UINT_MAX
		 * to avoid matching valid MPIDR[23:0] values.
		 */

/* IAMROOT-12AB:
 * -------------
 * 이미 읽어온 hwid 값이 중복된 경우 경고 메시지 출력후 종료
 */
		for (j = 0; j < cpuidx; j++)
			if (WARN(tmp_map[j] == hwid, "Duplicate /cpu reg "
						     "properties in the DT\n"))
				return;

		/*
		 * Build a stashed array of MPIDR values. Numbering scheme
		 * requires that if detected the boot CPU must be assigned
		 * logical id 0. Other CPUs get sequential indexes starting
		 * from 1. If a CPU node with a reg property matching the
		 * boot CPU MPIDR is detected, this is recorded so that the
		 * logical map built from DT is validated and can be used
		 * to override the map created in smp_setup_processor_id().
		 */

/* IAMROOT-12AB:
 * -------------
 * dtb에서 읽어온 hwid와 보조 레지스터에서 읽어온 mpidr 값을 비교
 */

		if (hwid == mpidr) {
			i = 0;
			bootcpu_valid = true;
		} else {
			i = cpuidx++;
		}

		if (WARN(cpuidx > nr_cpu_ids, "DT /cpu %u nodes greater than "
					       "max cores %u, capping them\n",
					       cpuidx, nr_cpu_ids)) {
			cpuidx = nr_cpu_ids;
			break;
		}

/* IAMROOT-12AB:
 * -------------
 * rpi2: tmp_map[] = { 0xf00, 0xf01, 0xf02, 0xf03 }
 */
		tmp_map[i] = hwid;

/* IAMROOT-12AB:
 * -------------
 * SMP ops가 처음 설정된 경우 한 번만 수행된다.
 */
		if (!found_method)
			found_method = set_smp_ops_by_method(cpu);
	}

	/*
	 * Fallback to an enable-method in the cpus node if nothing found in
	 * a cpu node.
	 */

/* IAMROOT-12AB:
 * -------------
 * cpus child노드인 cpu의 enable-method로 매치되지 않는 경우 
 *		    cpus 노드에 있는 enable-method로도 검색을 시도한다.
 */
	if (!found_method)
		set_smp_ops_by_method(cpus);

/* IAMROOT-12AB:
 * -------------
 * DTB의 cpu 노드의 reg 값이 hwid와 한번도 같은 경우가 없으면 아래 메시지가 출력되고 종료
 */
	if (!bootcpu_valid) {
		pr_warn("DT missing boot CPU MPIDR[23:0], fall back to default cpu_logical_map\n");
		return;
	}

	/*
	 * Since the boot CPU node contains proper data, and all nodes have
	 * a reg property, the DT CPU list can be considered valid and the
	 * logical map created in smp_setup_processor_id() can be overridden
	 */

/* IAMROOT-12AB:
 * -------------
 * __cpu_logical_map[]에 tmp_map[]을 복사하고 각 cpu를 possible 상태로 설정한다.
 */

	for (i = 0; i < cpuidx; i++) {
		set_cpu_possible(i, true);
		cpu_logical_map(i) = tmp_map[i];
		pr_debug("cpu logical map 0x%x\n", cpu_logical_map(i));
	}
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

static const void * __init arch_get_next_mach(const char *const **match)
{
	static const struct machine_desc *mdesc = __arch_info_begin;
	const struct machine_desc *m = mdesc;

	if (m >= __arch_info_end)
		return NULL;

	mdesc++;
	*match = m->dt_compat;
	return m;
}

/**
 * setup_machine_fdt - Machine setup when an dtb was passed to the kernel
 * @dt_phys: physical address of dt blob
 *
 * If a dtb was passed to the kernel in r2, then use it to choose the
 * correct machine_desc and to setup the system.
 */
const struct machine_desc * __init setup_machine_fdt(unsigned int dt_phys)
{
	const struct machine_desc *mdesc, *mdesc_best = NULL;

/* IAMROOT-12A:
 * ------------
 * 2012년에 멀티플랫폼을 지원하기 시작하였고,
 * 라즈베리파이2는 이 옵션을 사용하지 않음.
 */
#ifdef CONFIG_ARCH_MULTIPLATFORM
	DT_MACHINE_START(GENERIC_DT, "Generic DT based system")
	MACHINE_END

	mdesc_best = &__mach_desc_GENERIC_DT;
#endif


/* IAMROOT-12A:
 * ------------
 * 간략하게 dtb가 존재하는지 확인한다.
 */
	if (!dt_phys || !early_init_dt_verify(phys_to_virt(dt_phys)))
		return NULL;

/* IAMROOT-12A:
 * ------------
 * 현재 시스템에 맞는 머신 디스크립터를 찾아온다.
 * (잘 찾아올거라 예상하고 분석하지 않음)
 */
	mdesc = of_flat_dt_match_machine(mdesc_best, arch_get_next_mach);

	if (!mdesc) {
		const char *prop;
		int size;
		unsigned long dt_root;

		early_print("\nError: unrecognized/unsupported "
			    "device tree compatible list:\n[ ");

		dt_root = of_get_flat_dt_root();
		prop = of_get_flat_dt_prop(dt_root, "compatible", &size);
		while (size > 0) {
			early_print("'%s' ", prop);
			size -= strlen(prop) + 1;
			prop += strlen(prop) + 1;
		}
		early_print("]\n\n");

		dump_machine_table(); /* does not return */
	}

/* IAMROOT-12A:
 * ------------
 * 대부분의 머신 디스크립터에는 dt_fixup이 null 인다.
 * (참고 적용 사례: arch/arm/mach-exynos/exynos.c – dt_fixup에 exynos_dt_fixup())
 */
	/* We really don't want to do this, but sometimes firmware provides buggy data */
	if (mdesc->dt_fixup)
		mdesc->dt_fixup();

/* IAMROOT-12A:
 * ------------
 * 전역 변수 boot_command_line에 dtb나 커널이 전달해준 커멘드라인 문자열을 저장
 * memblock에 dtb 메모리 노드의 reg 영역을 추가한다.
 */
	early_init_dt_scan_nodes();

/* IAMROOT-12A:
 * ------------
 * 전역 변수 __machine_arch_type에 머신 번호를 저장한다. 
 */
	/* Change machine number to match the mdesc we're using */
	__machine_arch_type = mdesc->nr;

	return mdesc;
}
