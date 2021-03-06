/*
 *  linux/drivers/clocksource/arm_arch_timer.c
 *
 *  Copyright (C) 2011 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/sched_clock.h>

#include <asm/arch_timer.h>
#include <asm/virt.h>

#include <clocksource/arm_arch_timer.h>

#define CNTTIDR		0x08
#define CNTTIDR_VIRT(n)	(BIT(1) << ((n) * 4))

#define CNTVCT_LO	0x08
#define CNTVCT_HI	0x0c
#define CNTFRQ		0x10
#define CNTP_TVAL	0x28
#define CNTP_CTL	0x2c
#define CNTV_TVAL	0x38
#define CNTV_CTL	0x3c

#define ARCH_CP15_TIMER	BIT(0)
#define ARCH_MEM_TIMER	BIT(1)
static unsigned arch_timers_present __initdata;

static void __iomem *arch_counter_base;

struct arch_timer {
	void __iomem *base;
	struct clock_event_device evt;
};

#define to_arch_timer(e) container_of(e, struct arch_timer, evt)

/* IAMROOT-12:
 * -------------
 * arch 타이머에 사용하는 주파수(rpi2: 19.2Mhz)
 */
static u32 arch_timer_rate;


/* IAMROOT-12:
 * -------------
 * cpu 마다 4개의 h/w 타이머가 있다.
 */
enum ppi_nr {
	PHYS_SECURE_PPI,
	PHYS_NONSECURE_PPI,
	VIRT_PPI,
	HYP_PPI,
	MAX_TIMER_PPI
};

static int arch_timer_ppi[MAX_TIMER_PPI];

static struct clock_event_device __percpu *arch_timer_evt;

/* IAMROOT-12:
 * -------------
 * 4개의 타이머중 디폴트로 virtual 타이머를 사용한다.
 */
static bool arch_timer_use_virtual = true;

/* IAMROOT-12:
 * -------------
 * "always-on" 속성이 없을 때 true
 */
static bool arch_timer_c3stop;
static bool arch_timer_mem_use_virtual;

/*
 * Architected system timer support.
 */

static __always_inline
void arch_timer_reg_write(int access, enum arch_timer_reg reg, u32 val,
			  struct clock_event_device *clk)
{
	if (access == ARCH_TIMER_MEM_PHYS_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARCH_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTV_TVAL);
			break;
		}
	} else {
		arch_timer_reg_write_cp15(access, reg, val);
	}
}

static __always_inline
u32 arch_timer_reg_read(int access, enum arch_timer_reg reg,
			struct clock_event_device *clk)
{
	u32 val;

	if (access == ARCH_TIMER_MEM_PHYS_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARCH_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTV_TVAL);
			break;
		}
	} else {
		val = arch_timer_reg_read_cp15(access, reg);
	}

	return val;
}

static __always_inline irqreturn_t timer_handler(const int access,
					struct clock_event_device *evt)
{
	unsigned long ctrl;

	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, evt);
	if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {
		ctrl |= ARCH_TIMER_CTRL_IT_MASK;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, evt);
		evt->event_handler(evt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static irqreturn_t arch_timer_handler_virt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_VIRT_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_phys(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_PHYS_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_phys_mem(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_MEM_PHYS_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_virt_mem(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_MEM_VIRT_ACCESS, evt);
}

static __always_inline void timer_set_mode(const int access, int mode,
				  struct clock_event_device *clk)
{
	unsigned long ctrl;
	switch (mode) {
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:

/* IAMROOT-12:
 * -------------
 * 타이머 동작 정지 (CNTP_CTL.bits[0]을 제어한다.)
 */
		ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
		ctrl &= ~ARCH_TIMER_CTRL_ENABLE;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
		break;
	default:
		break;
	}
}

static void arch_timer_set_mode_virt(enum clock_event_mode mode,
				     struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_VIRT_ACCESS, mode, clk);
}

static void arch_timer_set_mode_phys(enum clock_event_mode mode,
				     struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_PHYS_ACCESS, mode, clk);
}

static void arch_timer_set_mode_virt_mem(enum clock_event_mode mode,
					 struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_MEM_VIRT_ACCESS, mode, clk);
}

static void arch_timer_set_mode_phys_mem(enum clock_event_mode mode,
					 struct clock_event_device *clk)
{
	timer_set_mode(ARCH_TIMER_MEM_PHYS_ACCESS, mode, clk);
}

static __always_inline void set_next_event(const int access, unsigned long evt,
					   struct clock_event_device *clk)
{
	unsigned long ctrl;
	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
	ctrl |= ARCH_TIMER_CTRL_ENABLE;
	ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
	arch_timer_reg_write(access, ARCH_TIMER_REG_TVAL, evt, clk);
	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
}

static int arch_timer_set_next_event_virt(unsigned long evt,
					  struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_VIRT_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_phys(unsigned long evt,
					  struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_PHYS_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_virt_mem(unsigned long evt,
					      struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_MEM_VIRT_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_phys_mem(unsigned long evt,
					      struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_MEM_PHYS_ACCESS, evt, clk);
	return 0;
}

static void __arch_timer_setup(unsigned type,
			       struct clock_event_device *clk)
{
/* IAMROOT-12:
 * -------------
 * arch 타이머는 oneshot 기능이 있음을 알린다.
 */
	clk->features = CLOCK_EVT_FEAT_ONESHOT;

	if (type == ARCH_CP15_TIMER) {
		if (arch_timer_c3stop)
			clk->features |= CLOCK_EVT_FEAT_C3STOP;
		clk->name = "arch_sys_timer";

/* IAMROOT-12:
 * -------------
 * 클럭 rating이 450이다. (클럭을 여러 개 사용하는 경우 높은 클럭을 사용한다.)
 */
		clk->rating = 450;
		clk->cpumask = cpumask_of(smp_processor_id());
		if (arch_timer_use_virtual) {
			clk->irq = arch_timer_ppi[VIRT_PPI];

/* IAMROOT-12:
 * -------------
 * 모드 설정 및 다음 이벤트 프로그램 설정 핸들러 함수
 */
			clk->set_mode = arch_timer_set_mode_virt;
			clk->set_next_event = arch_timer_set_next_event_virt;
		} else {
			clk->irq = arch_timer_ppi[PHYS_SECURE_PPI];
			clk->set_mode = arch_timer_set_mode_phys;
			clk->set_next_event = arch_timer_set_next_event_phys;
		}
	} else {
		clk->features |= CLOCK_EVT_FEAT_DYNIRQ;
		clk->name = "arch_mem_timer";
		clk->rating = 400;
		clk->cpumask = cpu_all_mask;
		if (arch_timer_mem_use_virtual) {
			clk->set_mode = arch_timer_set_mode_virt_mem;
			clk->set_next_event =
				arch_timer_set_next_event_virt_mem;
		} else {
			clk->set_mode = arch_timer_set_mode_phys_mem;
			clk->set_next_event =
				arch_timer_set_next_event_phys_mem;
		}
	}

/* IAMROOT-12:
 * -------------
 * arch_timer_set_mode_virt() 함수 호출하여 타이머를 일단 정지 시킨다.
 */
	clk->set_mode(CLOCK_EVT_MODE_SHUTDOWN, clk);

	clockevents_config_and_register(clk, arch_timer_rate, 0xf, 0x7fffffff);
}

static void arch_timer_evtstrm_enable(int divider)
{
	u32 cntkctl = arch_timer_get_cntkctl();

	cntkctl &= ~ARCH_TIMER_EVT_TRIGGER_MASK;
	/* Set the divider and enable virtual event stream */
	cntkctl |= (divider << ARCH_TIMER_EVT_TRIGGER_SHIFT)
			| ARCH_TIMER_VIRT_EVT_EN;
	arch_timer_set_cntkctl(cntkctl);
	elf_hwcap |= HWCAP_EVTSTRM;
#ifdef CONFIG_COMPAT
	compat_elf_hwcap |= COMPAT_HWCAP_EVTSTRM;
#endif
}

static void arch_timer_configure_evtstream(void)
{
	int evt_stream_div, pos;


/* IAMROOT-12:
 * -------------
 * arch 타이머에서 이벤트 스트림이 발생되도록 설정한다.
 */
	/* Find the closest power of two to the divisor */
	evt_stream_div = arch_timer_rate / ARCH_TIMER_EVT_STREAM_FREQ;
	pos = fls(evt_stream_div);
	if (pos > 1 && !(evt_stream_div & (1 << (pos - 2))))
		pos--;
	/* enable event stream */
	arch_timer_evtstrm_enable(min(pos, 15));
}

static void arch_counter_set_user_access(void)
{
/* IAMROOT-12:
 * -------------
 * CNTKCTL(Timer PL1 Control register)의 값을 반환한다.
 */
	u32 cntkctl = arch_timer_get_cntkctl();

	/* Disable user access to the timers and the physical counter */
	/* Also disable virtual event stream */

/* IAMROOT-12:
 * -------------
 * PL0(user 레벨)에서 이 arch 타이머에 접근할 수 있도록 허용한다.ㅁ
 * (swi 등의 syscall 없이 즉, 커널 진입 없이 타이머 자원을 access 할 수 있다)
 */
	cntkctl &= ~(ARCH_TIMER_USR_PT_ACCESS_EN
			| ARCH_TIMER_USR_VT_ACCESS_EN
			| ARCH_TIMER_VIRT_EVT_EN
			| ARCH_TIMER_USR_PCT_ACCESS_EN);

	/* Enable user access to the virtual counter */
	cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN;

	arch_timer_set_cntkctl(cntkctl);
}

static int arch_timer_setup(struct clock_event_device *clk)
{

/* IAMROOT-12:
 * -------------
 * arch 타이머를 클럭 이벤트 디바이스로 등록한다.
 * (내부에서 이 타이머가 틱 디바이스로도 등록된다.)
 */
	__arch_timer_setup(ARCH_CP15_TIMER, clk);

/* IAMROOT-12:
 * -------------
 * arch 타이머에 대한 인터럽트를 enable한다.
 */
	if (arch_timer_use_virtual)
		enable_percpu_irq(arch_timer_ppi[VIRT_PPI], 0);
	else {
		enable_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI], 0);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			enable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI], 0);
	}

	arch_counter_set_user_access();

/* IAMROOT-12:
 * -------------
 * arm에서 유저 스페이스에서 lock을 구현 시 wfe 명령을 사용하였는데 
 * 다른 cpu의 sev에 의해 이벤트가 fire되어야 하는데 동작하지 않는 경우가 
 * 발생하여 arch 타이머에서 10khz 간격으로 이벤트를 주기적으로 발생하여 
 * 혹시라도 모를 lock이 지속되는 것을 강제로 풀게하였다.
 */
	if (IS_ENABLED(CONFIG_ARM_ARCH_TIMER_EVTSTREAM))
		arch_timer_configure_evtstream();

	return 0;
}

static void
arch_timer_detect_rate(void __iomem *cntbase, struct device_node *np)
{
	/* Who has more than one independent system counter? */
	if (arch_timer_rate)
		return;

/* IAMROOT-12:
 * -------------
 * "clock-frequency" 속성 값을 &arch_timer_rate 변수에 저장한다.
 */

	/* Try to determine the frequency from the device tree or CNTFRQ */
	if (of_property_read_u32(np, "clock-frequency", &arch_timer_rate)) {
		if (cntbase)
			arch_timer_rate = readl_relaxed(cntbase + CNTFRQ);
		else
/* IAMROOT-12:
 * -------------
 * 위의 속성 값을 못찾은 경우 CNTFRQ 레지스터에 설정된 클럭 주파수(19.2Mhz)를
 * 알아온다.
 */
			arch_timer_rate = arch_timer_get_cntfrq();
	}

	/* Check the timer frequency. */
	if (arch_timer_rate == 0)
		pr_warn("Architected timer frequency not available\n");
}

static void arch_timer_banner(unsigned type)
{
	pr_info("Architected %s%s%s timer(s) running at %lu.%02luMHz (%s%s%s).\n",
		     type & ARCH_CP15_TIMER ? "cp15" : "",
		     type == (ARCH_CP15_TIMER | ARCH_MEM_TIMER) ?  " and " : "",
		     type & ARCH_MEM_TIMER ? "mmio" : "",
		     (unsigned long)arch_timer_rate / 1000000,
		     (unsigned long)(arch_timer_rate / 10000) % 100,
		     type & ARCH_CP15_TIMER ?
			arch_timer_use_virtual ? "virt" : "phys" :
			"",
		     type == (ARCH_CP15_TIMER | ARCH_MEM_TIMER) ?  "/" : "",
		     type & ARCH_MEM_TIMER ?
			arch_timer_mem_use_virtual ? "virt" : "phys" :
			"");
}

u32 arch_timer_get_rate(void)
{
	return arch_timer_rate;
}

static u64 arch_counter_get_cntvct_mem(void)
{
	u32 vct_lo, vct_hi, tmp_hi;

	do {
		vct_hi = readl_relaxed(arch_counter_base + CNTVCT_HI);
		vct_lo = readl_relaxed(arch_counter_base + CNTVCT_LO);
		tmp_hi = readl_relaxed(arch_counter_base + CNTVCT_HI);
	} while (vct_hi != tmp_hi);

	return ((u64) vct_hi << 32) | vct_lo;
}

/*
 * Default to cp15 based access because arm64 uses this function for
 * sched_clock() before DT is probed and the cp15 method is guaranteed
 * to exist on arm64. arm doesn't use this before DT is probed so even
 * if we don't have the cp15 accessors we won't have a problem.
 */
u64 (*arch_timer_read_counter)(void) = arch_counter_get_cntvct;

static cycle_t arch_counter_read(struct clocksource *cs)
{
	return arch_timer_read_counter();
}

static cycle_t arch_counter_read_cc(const struct cyclecounter *cc)
{
	return arch_timer_read_counter();
}

/* IAMROOT-12:
 * -------------
 * 56bit 카운터 소스이고 continue하게 동작한다.
 */
static struct clocksource clocksource_counter = {
	.name	= "arch_sys_counter",
	.rating	= 400,
	.read	= arch_counter_read,
	.mask	= CLOCKSOURCE_MASK(56),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_SUSPEND_NONSTOP,
};

static struct cyclecounter cyclecounter = {
	.read	= arch_counter_read_cc,
	.mask	= CLOCKSOURCE_MASK(56),
};

static struct timecounter timecounter;

struct timecounter *arch_timer_get_timecounter(void)
{
	return &timecounter;
}

static void __init arch_counter_register(unsigned type)
{
	u64 start_count;

	/* Register the CP15 based counter if we have one */

/* IAMROOT-12:
 * -------------
 * arch 타이머의 64비트 카운터 값을 읽어오는 함수를 결정한다.
 * (physical 및 virtual 카운터 중 하나를 선택하여 운용한다)
 */
	if (type & ARCH_CP15_TIMER) {
		if (IS_ENABLED(CONFIG_ARM64) || arch_timer_use_virtual)
			arch_timer_read_counter = arch_counter_get_cntvct;
		else
			arch_timer_read_counter = arch_counter_get_cntpct;
	} else {
		arch_timer_read_counter = arch_counter_get_cntvct_mem;

		/* If the clocksource name is "arch_sys_counter" the
		 * VDSO will attempt to read the CP15-based counter.
		 * Ensure this does not happen when CP15-based
		 * counter is not available.
		 */
		clocksource_counter.name = "arch_mem_counter";
	}

/* IAMROOT-12:
 * -------------
 * 이 시점의 카운터를 읽어온다.
 */
	start_count = arch_timer_read_counter();

/* IAMROOT-12:
 * -------------
 * 클럭 소스를 등록한다.
 */
	clocksource_register_hz(&clocksource_counter, arch_timer_rate);

/* IAMROOT-12:
 * -------------
 * 사이클 카운터에 mult와 shift를 대입한다. <- for timekeeping
 */
	cyclecounter.mult = clocksource_counter.mult;
	cyclecounter.shift = clocksource_counter.shift;

/* IAMROOT-12:
 * -------------
 * 타임 카운터는 고속 디바이스에서 시간 비교를 위해 사용된다.
 * (10G ethernet drive or PTP)
 */
	timecounter_init(&timecounter, &cyclecounter, start_count);

	/* 56 bits minimum, so we assume worst case rollover */

/* IAMROOT-12:
 * -------------
 * sched 클럭으로 등록완료. (sched_clock() 인터페이스)
 */
	sched_clock_register(arch_timer_read_counter, 56, arch_timer_rate);
}

static void arch_timer_stop(struct clock_event_device *clk)
{
	pr_debug("arch_timer_teardown disable IRQ%d cpu #%d\n",
		 clk->irq, smp_processor_id());

	if (arch_timer_use_virtual)
		disable_percpu_irq(arch_timer_ppi[VIRT_PPI]);
	else {
		disable_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI]);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			disable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI]);
	}

	clk->set_mode(CLOCK_EVT_MODE_UNUSED, clk);
}

static int arch_timer_cpu_notify(struct notifier_block *self,
					   unsigned long action, void *hcpu)
{
	/*
	 * Grab cpu pointer in each case to avoid spurious
	 * preemptible warnings
	 */
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_STARTING:

/* IAMROOT-12:
 * -------------
 * cpu가 on될 때 arch 타이머도 셋업한다.
 */
		arch_timer_setup(this_cpu_ptr(arch_timer_evt));
		break;
	case CPU_DYING:
		arch_timer_stop(this_cpu_ptr(arch_timer_evt));
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block arch_timer_cpu_nb = {
	.notifier_call = arch_timer_cpu_notify,
};

#ifdef CONFIG_CPU_PM
static unsigned int saved_cntkctl;
static int arch_timer_cpu_pm_notify(struct notifier_block *self,
				    unsigned long action, void *hcpu)
{

/* IAMROOT-12:
 * -------------
 * suspend 모드로 진입 시 타이머 컨트롤 레지스터 값을 잠시 저장해둔다.
 */
	if (action == CPU_PM_ENTER)
		saved_cntkctl = arch_timer_get_cntkctl();
	else if (action == CPU_PM_ENTER_FAILED || action == CPU_PM_EXIT)

/* IAMROOT-12:
 * -------------
 * resume 진입 시 다시 복원한다.
 */
		arch_timer_set_cntkctl(saved_cntkctl);
	return NOTIFY_OK;
}

static struct notifier_block arch_timer_cpu_pm_notifier = {
	.notifier_call = arch_timer_cpu_pm_notify,
};

static int __init arch_timer_cpu_pm_init(void)
{

/* IAMROOT-12:
 * -------------
 * suspend/resume 시 마다 arch_timer_cpu_pm_notifier() 함수를 호출하도록 
 * 등록한다.
 */
	return cpu_pm_register_notifier(&arch_timer_cpu_pm_notifier);
}
#else
static int __init arch_timer_cpu_pm_init(void)
{
	return 0;
}
#endif

static int __init arch_timer_register(void)
{
	int err;
	int ppi;

/* IAMROOT-12:
 * -------------
 * armv7에 내장된 arch 타이머(로컬 타이머)를 클럭 이벤트 디바이스로도 사용한다.
 */
	arch_timer_evt = alloc_percpu(struct clock_event_device);
	if (!arch_timer_evt) {
		err = -ENOMEM;
		goto out;
	}

	if (arch_timer_use_virtual) {
/* IAMROOT-12:
 * -------------
 * rpi2는 virtual 타이머를 사용한다.
 */
		ppi = arch_timer_ppi[VIRT_PPI];
		err = request_percpu_irq(ppi, arch_timer_handler_virt,
					 "arch_timer", arch_timer_evt);
	} else {
/* IAMROOT-12:
 * -------------
 * rpi3는 physical 타이머를 선택한다. (virtual extension이 있고 
 * 마스터 부팅된 경우)
 */
		ppi = arch_timer_ppi[PHYS_SECURE_PPI];

/* IAMROOT-12:
 * -------------
 * per-cpu용 irq 핸들러를 등록한다.
 */
		err = request_percpu_irq(ppi, arch_timer_handler_phys,
					 "arch_timer", arch_timer_evt);

/* IAMROOT-12:
 * -------------
 * secure용 physical 타이머의 등록이 성공한 경우 non-secure용 physical 타이머를 
 * 사용하여 시도한다.
 */
		if (!err && arch_timer_ppi[PHYS_NONSECURE_PPI]) {
			ppi = arch_timer_ppi[PHYS_NONSECURE_PPI];
			err = request_percpu_irq(ppi, arch_timer_handler_phys,
						 "arch_timer", arch_timer_evt);
			if (err)
				free_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI],
						arch_timer_evt);
		}
	}

	if (err) {
		pr_err("arch_timer: can't register interrupt %d (%d)\n",
		       ppi, err);
		goto out_free;
	}

/* IAMROOT-12:
 * -------------
 * arch 타이머를 등록한다. cpu가 on될 때마다 초기화 루틴이 동작한다.
 */
	err = register_cpu_notifier(&arch_timer_cpu_nb);
	if (err)
		goto out_free_irq;

/* IAMROOT-12:
 * -------------
 * suspend 시 타이머 컨트롤 레지스터의 값을 잠시 저장해둔다.
 * 이 값은 다시 resume 시에 복원할 수 있도록한다.
 */
	err = arch_timer_cpu_pm_init();
	if (err)
		goto out_unreg_notify;

	/* Immediately configure the timer on the boot CPU */

/* IAMROOT-12:
 * -------------
 * boot cpu에 대해서는 이미 up되어 있으므로 이 함수를 통해서 arch 타이머를 
 * 설정한다.
 */
	arch_timer_setup(this_cpu_ptr(arch_timer_evt));

	return 0;

out_unreg_notify:
	unregister_cpu_notifier(&arch_timer_cpu_nb);
out_free_irq:
	if (arch_timer_use_virtual)
		free_percpu_irq(arch_timer_ppi[VIRT_PPI], arch_timer_evt);
	else {
		free_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI],
				arch_timer_evt);
		if (arch_timer_ppi[PHYS_NONSECURE_PPI])
			free_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI],
					arch_timer_evt);
	}

out_free:
	free_percpu(arch_timer_evt);
out:
	return err;
}

static int __init arch_timer_mem_register(void __iomem *base, unsigned int irq)
{
	int ret;
	irq_handler_t func;
	struct arch_timer *t;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	t->base = base;
	t->evt.irq = irq;
	__arch_timer_setup(ARCH_MEM_TIMER, &t->evt);

	if (arch_timer_mem_use_virtual)
		func = arch_timer_handler_virt_mem;
	else
		func = arch_timer_handler_phys_mem;

	ret = request_irq(irq, func, IRQF_TIMER, "arch_mem_timer", &t->evt);
	if (ret) {
		pr_err("arch_timer: Failed to request mem timer irq\n");
		kfree(t);
	}

	return ret;
}

static const struct of_device_id arch_timer_of_match[] __initconst = {
	{ .compatible   = "arm,armv7-timer",    },
	{ .compatible   = "arm,armv8-timer",    },
	{},
};

static const struct of_device_id arch_timer_mem_of_match[] __initconst = {
	{ .compatible   = "arm,armv7-timer-mem", },
	{},
};

static bool __init
arch_timer_probed(int type, const struct of_device_id *matches)
{
	struct device_node *dn;
	bool probed = true;

	dn = of_find_matching_node(NULL, matches);
	if (dn && of_device_is_available(dn) && !(arch_timers_present & type))
		probed = false;
	of_node_put(dn);

	return probed;
}

static void __init arch_timer_common_init(void)
{

/* IAMROOT-12:
 * -------------
 * cp15로 동작하는 arch 타이머나 메모리 맵드로 동작하는 arch 타이머 
 * 둘 다 사용하는 common 코드 이다.
 */
	unsigned mask = ARCH_CP15_TIMER | ARCH_MEM_TIMER;

	/* Wait until both nodes are probed if we have two timers */
	if ((arch_timers_present & mask) != mask) {
		if (!arch_timer_probed(ARCH_MEM_TIMER, arch_timer_mem_of_match))
			return;
		if (!arch_timer_probed(ARCH_CP15_TIMER, arch_timer_of_match))
			return;
	}

	arch_timer_banner(arch_timers_present);

/* IAMROOT-12:
 * -------------
 * 카운터 관련한 클럭 인터페이스를 제공하기 위해 준비한다.
 *	- 클럭소스
 *	- 타임카운터(사이클카운터)
 *	- sched 클럭
 */
	arch_counter_register(arch_timers_present);

/* IAMROOT-12:
 * -------------
 * delay 타이머로 등록한다.
 */
	arch_timer_arch_init();
}

/* IAMROOT-12:
 * -------------
 * armv7 내장 local 타이머 초기화
 * (cpu당 4개의 h/w 타이머가 있고 리눅스 커널은 그 중 하나를 사용한다.)
 */
static void __init arch_timer_init(struct device_node *np)
{
	int i;

/* IAMROOT-12:
 * -------------
 * 처음 진입 시 arch_timers_present에 설정된 플래그가 없다.
 * (디바이스 트리의 이중 구현으로 인한 진입을 막는다.)
 */
	if (arch_timers_present & ARCH_CP15_TIMER) {
		pr_warn("arch_timer: multiple nodes in dt, skipping\n");
		return;
	}

	arch_timers_present |= ARCH_CP15_TIMER;

/* IAMROOT-12:
 * -------------
 * 4개의 타이머에 대한 irq 디스크립터를 초기화하고 irq(virq) 번호를 알아온다.
 */
	for (i = PHYS_SECURE_PPI; i < MAX_TIMER_PPI; i++)
		arch_timer_ppi[i] = irq_of_parse_and_map(np, i);

/* IAMROOT-12:
 * -------------
 * 디바이스 트리 또는 카운터 레지스터를 통해 클럭 주파수(rpi2: 19.2Mhz)를 알아온다.
 */
	arch_timer_detect_rate(NULL, np);

	/*
	 * If we cannot rely on firmware initializing the timer registers then
	 * we should use the physical timers instead.
	 */

/* IAMROOT-12:
 * -------------
 * 특정 시스템에서는 리눅스가 무조건 physical 타이머를 사용하여야 한다.
 */
	if (IS_ENABLED(CONFIG_ARM) &&
	    of_property_read_bool(np, "arm,cpu-registers-not-fw-configured"))
			arch_timer_use_virtual = false;

	/*
	 * If HYP mode is available, we know that the physical timer
	 * has been configured to be accessible from PL1. Use it, so
	 * that a guest can use the virtual timer instead.
	 *
	 * If no interrupt provided for virtual timer, we'll have to
	 * stick to the physical timer. It'd better be accessible...
	 */

/* IAMROOT-12:
 * -------------
 * 현재 시스템이 virtual extension을 지원하는 경우에는 guest os에 virtual 
 * 타이머를 사용하도록 현재 시스템은 physical 타이머를 사용한다.
 * virtual 타이머 설정이 없는 경우 항상 physical 타이머를 사용한다.
 */
	if (is_hyp_mode_available() || !arch_timer_ppi[VIRT_PPI]) {
		arch_timer_use_virtual = false;

		if (!arch_timer_ppi[PHYS_SECURE_PPI] ||
		    !arch_timer_ppi[PHYS_NONSECURE_PPI]) {
			pr_warn("arch_timer: No interrupt available, giving up\n");
			return;
		}
	}

/* IAMROOT-12:
 * -------------
 * 시스템이 c3stop 기능을 지원하지 않는 경우 "always-on" 속성을 사용한다.
 * 코어(보통 클러스터 전체가)가 deep-sleep 상태로 전환되는 경우 타이머 및
 * 인터럽트 컨트롤러의 전원도 다운시킬 수도 있다. 그런데 이렇게 전원을 
 * 끄지 않고 항상 켜있는 경우 이러한 속성을 사용한다.
 *
 * 일반적인 arm 시스템에서 코어 1개가 sleep하는 경우 절전을 위해 wfi로
 * 처리한다. 그런데 클러스터 내에 있는 코어들이 모두 sleep 하는 경우 
 * deep-sleep 상태로 전환시킬 수 있다.
 */
	arch_timer_c3stop = !of_property_read_bool(np, "always-on");

/* IAMROOT-12:
 * -------------
 * arch 타이머를 등록한다.
 */
	arch_timer_register();

/* IAMROOT-12:
 * -------------
 * arch 타이머를 각종 클럭 인터페이스에 제공한다.
 */
	arch_timer_common_init();
}
CLOCKSOURCE_OF_DECLARE(armv7_arch_timer, "arm,armv7-timer", arch_timer_init);
CLOCKSOURCE_OF_DECLARE(armv8_arch_timer, "arm,armv8-timer", arch_timer_init);

static void __init arch_timer_mem_init(struct device_node *np)
{
	struct device_node *frame, *best_frame = NULL;
	void __iomem *cntctlbase, *base;
	unsigned int irq;
	u32 cnttidr;

	arch_timers_present |= ARCH_MEM_TIMER;
	cntctlbase = of_iomap(np, 0);
	if (!cntctlbase) {
		pr_err("arch_timer: Can't find CNTCTLBase\n");
		return;
	}

	cnttidr = readl_relaxed(cntctlbase + CNTTIDR);
	iounmap(cntctlbase);

	/*
	 * Try to find a virtual capable frame. Otherwise fall back to a
	 * physical capable frame.
	 */
	for_each_available_child_of_node(np, frame) {
		int n;

		if (of_property_read_u32(frame, "frame-number", &n)) {
			pr_err("arch_timer: Missing frame-number\n");
			of_node_put(best_frame);
			of_node_put(frame);
			return;
		}

		if (cnttidr & CNTTIDR_VIRT(n)) {
			of_node_put(best_frame);
			best_frame = frame;
			arch_timer_mem_use_virtual = true;
			break;
		}
		of_node_put(best_frame);
		best_frame = of_node_get(frame);
	}

	base = arch_counter_base = of_iomap(best_frame, 0);
	if (!base) {
		pr_err("arch_timer: Can't map frame's registers\n");
		of_node_put(best_frame);
		return;
	}

	if (arch_timer_mem_use_virtual)
		irq = irq_of_parse_and_map(best_frame, 1);
	else
		irq = irq_of_parse_and_map(best_frame, 0);
	of_node_put(best_frame);
	if (!irq) {
		pr_err("arch_timer: Frame missing %s irq",
		       arch_timer_mem_use_virtual ? "virt" : "phys");
		return;
	}

	arch_timer_detect_rate(base, np);
	arch_timer_mem_register(base, irq);
	arch_timer_common_init();
}
CLOCKSOURCE_OF_DECLARE(armv7_arch_timer_mem, "arm,armv7-timer-mem",
		       arch_timer_mem_init);
