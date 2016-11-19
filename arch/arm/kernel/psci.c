/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <uapi/linux/psci.h>

#include <asm/compiler.h>
#include <asm/errno.h>
#include <asm/psci.h>
#include <asm/system_misc.h>


/* IAMROOT-12AB:
 * -------------
 * 전역 PSCI용 ops 핸들러
 */
struct psci_operations psci_ops;


/* IAMROOT-12AB:
 * -------------
 * 전역 invoke_psci_fn에는 hvc 또는 smc 메소드에 따라 호출되는 함수가 대입된다.
 *	- __invoke_psci_fn_hvc()
 *	- __invoke_psci_fn_smc()
 */
static int (*invoke_psci_fn)(u32, u32, u32, u32);
typedef int (*psci_initcall_t)(const struct device_node *);

/* IAMROOT-12AB:
 * -------------
 * Hyperviser Call 루틴
 * Secure Monitor Call 루틴
 */
asmlinkage int __invoke_psci_fn_hvc(u32, u32, u32, u32);
asmlinkage int __invoke_psci_fn_smc(u32, u32, u32, u32);

enum psci_function {
	PSCI_FN_CPU_SUSPEND,
	PSCI_FN_CPU_ON,
	PSCI_FN_CPU_OFF,
	PSCI_FN_MIGRATE,
	PSCI_FN_AFFINITY_INFO,
	PSCI_FN_MIGRATE_INFO_TYPE,
	PSCI_FN_MAX,
};

static u32 psci_function_id[PSCI_FN_MAX];

static int psci_to_linux_errno(int errno)
{
	switch (errno) {
	case PSCI_RET_SUCCESS:
		return 0;
	case PSCI_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case PSCI_RET_INVALID_PARAMS:
		return -EINVAL;
	case PSCI_RET_DENIED:
		return -EPERM;
	};

	return -EINVAL;
}

static u32 psci_power_state_pack(struct psci_power_state state)
{
	return ((state.id << PSCI_0_2_POWER_STATE_ID_SHIFT)
			& PSCI_0_2_POWER_STATE_ID_MASK) |
		((state.type << PSCI_0_2_POWER_STATE_TYPE_SHIFT)
		 & PSCI_0_2_POWER_STATE_TYPE_MASK) |
		((state.affinity_level << PSCI_0_2_POWER_STATE_AFFL_SHIFT)
		 & PSCI_0_2_POWER_STATE_AFFL_MASK);
}

static int psci_get_version(void)
{
	int err;

	err = invoke_psci_fn(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);
	return err;
}

static int psci_cpu_suspend(struct psci_power_state state,
			    unsigned long entry_point)
{
	int err;
	u32 fn, power_state;

	fn = psci_function_id[PSCI_FN_CPU_SUSPEND];
	power_state = psci_power_state_pack(state);
	err = invoke_psci_fn(fn, power_state, entry_point, 0);
	return psci_to_linux_errno(err);
}

static int psci_cpu_off(struct psci_power_state state)
{
	int err;
	u32 fn, power_state;

	fn = psci_function_id[PSCI_FN_CPU_OFF];
	power_state = psci_power_state_pack(state);
	err = invoke_psci_fn(fn, power_state, 0, 0);
	return psci_to_linux_errno(err);
}

static int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_CPU_ON];
	err = invoke_psci_fn(fn, cpuid, entry_point, 0);
	return psci_to_linux_errno(err);
}

static int psci_migrate(unsigned long cpuid)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_MIGRATE];
	err = invoke_psci_fn(fn, cpuid, 0, 0);
	return psci_to_linux_errno(err);
}

static int psci_affinity_info(unsigned long target_affinity,
		unsigned long lowest_affinity_level)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_AFFINITY_INFO];
	err = invoke_psci_fn(fn, target_affinity, lowest_affinity_level, 0);
	return err;
}

static int psci_migrate_info_type(void)
{
	int err;
	u32 fn;

	fn = psci_function_id[PSCI_FN_MIGRATE_INFO_TYPE];
	err = invoke_psci_fn(fn, 0, 0, 0);
	return err;
}

static int get_set_conduit_method(struct device_node *np)
{
	const char *method;

	pr_info("probing for conduit method from DT.\n");

/* IAMROOT-12AB:
 * -------------
 * /psci 노드의 method 속성 값을 읽어와서 사용할 psci 호출 함수를 결정하여
 * 전역 invoke_psci_fn에 대입한다.
 *
 *	- __invoke_psci_fn_hvc()
 *	- __invoke_psci_fn_smc()
 */
	if (of_property_read_string(np, "method", &method)) {
		pr_warn("missing \"method\" property\n");
		return -ENXIO;
	}

	if (!strcmp("hvc", method)) {
		invoke_psci_fn = __invoke_psci_fn_hvc;
	} else if (!strcmp("smc", method)) {
		invoke_psci_fn = __invoke_psci_fn_smc;
	} else {
		pr_warn("invalid \"method\" property: %s\n", method);
		return -EINVAL;
	}
	return 0;
}

static void psci_sys_reset(enum reboot_mode reboot_mode, const char *cmd)
{
	invoke_psci_fn(PSCI_0_2_FN_SYSTEM_RESET, 0, 0, 0);
}

static void psci_sys_poweroff(void)
{
	invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);
}

/*
 * PSCI Function IDs for v0.2+ are well defined so use
 * standard values.
 */
static int psci_0_2_init(struct device_node *np)
{
	int err, ver;

/* IAMROOT-12AB:
 * -------------
 * 0.1과 내용이 유사한데 다른 점은 
 *	- 버전 체크 수행
 *	- 0.1과 다르게 지정되지 않아도 각 속성 함수를 psci_ops에 대입 
 *	- 전역 arm_pm_restart에 psci_sys_reset() 리셋 함수를 대입 
 *	- 전역 pm_power_off에 psci_sys_poweroff() 파워 off 함수를 대입
 */
	err = get_set_conduit_method(np);

	if (err)
		goto out_put_node;

	ver = psci_get_version();

	if (ver == PSCI_RET_NOT_SUPPORTED) {
		/* PSCI v0.2 mandates implementation of PSCI_ID_VERSION. */
		pr_err("PSCI firmware does not comply with the v0.2 spec.\n");
		err = -EOPNOTSUPP;
		goto out_put_node;
	} else {
		pr_info("PSCIv%d.%d detected in firmware.\n",
				PSCI_VERSION_MAJOR(ver),
				PSCI_VERSION_MINOR(ver));

		if (PSCI_VERSION_MAJOR(ver) == 0 &&
				PSCI_VERSION_MINOR(ver) < 2) {
			err = -EINVAL;
			pr_err("Conflicting PSCI version detected.\n");
			goto out_put_node;
		}
	}

	pr_info("Using standard PSCI v0.2 function IDs\n");
	psci_function_id[PSCI_FN_CPU_SUSPEND] = PSCI_0_2_FN_CPU_SUSPEND;
	psci_ops.cpu_suspend = psci_cpu_suspend;

	psci_function_id[PSCI_FN_CPU_OFF] = PSCI_0_2_FN_CPU_OFF;
	psci_ops.cpu_off = psci_cpu_off;

	psci_function_id[PSCI_FN_CPU_ON] = PSCI_0_2_FN_CPU_ON;
	psci_ops.cpu_on = psci_cpu_on;

	psci_function_id[PSCI_FN_MIGRATE] = PSCI_0_2_FN_MIGRATE;
	psci_ops.migrate = psci_migrate;

	psci_function_id[PSCI_FN_AFFINITY_INFO] = PSCI_0_2_FN_AFFINITY_INFO;
	psci_ops.affinity_info = psci_affinity_info;

	psci_function_id[PSCI_FN_MIGRATE_INFO_TYPE] =
		PSCI_0_2_FN_MIGRATE_INFO_TYPE;
	psci_ops.migrate_info_type = psci_migrate_info_type;

	arm_pm_restart = psci_sys_reset;

	pm_power_off = psci_sys_poweroff;

out_put_node:
	of_node_put(np);
	return err;
}

/*
 * PSCI < v0.2 get PSCI Function IDs via DT.
 */
static int psci_0_1_init(struct device_node *np)
{
	u32 id;
	int err;

/* IAMROOT-12AB:
 * -------------
 * /psci 노드의 method 속성 값을 읽어와서 사용할 psci 호출 함수를 결정하여
 * 전역 invoke_psci_fn에 다음 둘 중 하나의 함수를 대입하여 
 * 아래 기능을 호출할 수 있게 한다.
 *	- __invoke_psci_fn_hvc() -> HyperVisor Call
 *	- __invoke_psci_fn_smc() -> Secure Monitor Call
 */
	err = get_set_conduit_method(np);

	if (err)
		goto out_put_node;

	pr_info("Using PSCI v0.1 Function IDs from DT\n");

/* IAMROOT-12AB:
 * -------------
 * /psci 노드의 각 제어 속성을 읽어와서 psci_function_id[]에 저장하고
 *	구동 핸들러를 psci_ops에 지정한다.
 */
	if (!of_property_read_u32(np, "cpu_suspend", &id)) {
		psci_function_id[PSCI_FN_CPU_SUSPEND] = id;
		psci_ops.cpu_suspend = psci_cpu_suspend;
	}

	if (!of_property_read_u32(np, "cpu_off", &id)) {
		psci_function_id[PSCI_FN_CPU_OFF] = id;
		psci_ops.cpu_off = psci_cpu_off;
	}

	if (!of_property_read_u32(np, "cpu_on", &id)) {
		psci_function_id[PSCI_FN_CPU_ON] = id;
		psci_ops.cpu_on = psci_cpu_on;
	}

	if (!of_property_read_u32(np, "migrate", &id)) {
		psci_function_id[PSCI_FN_MIGRATE] = id;
		psci_ops.migrate = psci_migrate;
	}

out_put_node:
	of_node_put(np);
	return err;
}


/* IAMROOT-12AB:
 * -------------
 * PSCI(Power State Coordination Interface): Power 상태 조절 인터페이스
 *
 * 두 개의 디바이스 드라이버가 준비되어 있다.
 *
 * 예) arch/arm/boot/dts/xenvm-4.2.dts
 *	psci {
 *		compatible      = "arm,psci";
 *              method          = "hvc";
 *              cpu_off         = <1>;
 *              cpu_on          = <2>;
 *      };
 */
static const struct of_device_id psci_of_match[] __initconst = {
	{ .compatible = "arm,psci", .data = psci_0_1_init},
	{ .compatible = "arm,psci-0.2", .data = psci_0_2_init},
	{},
};

int __init psci_init(void)
{
	struct device_node *np;
	const struct of_device_id *matched_np;
	psci_initcall_t init_fn;

/* IAMROOT-12AB:
 * -------------
 * psci_of_match로 dtb의 psci를 검색하여 디바이스명을 찾게되면
 * np에 device_node를 알아오고, matched_np에 찾은 of_device_id를 알아온다.
 */
	np = of_find_matching_node_and_match(NULL, psci_of_match, &matched_np);
	if (!np)
		return -ENODEV;

	init_fn = (psci_initcall_t)matched_np->data;


/* IAMROOT-12AB:
 * -------------
 * np를 인수로 psci_0_1_init() 또는 psci_0_2_init() 함수를 호출하여 psci_ops를 설정한다.
 */
	return init_fn(np);
}
