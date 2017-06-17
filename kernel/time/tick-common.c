/*
 * linux/kernel/time/tick-common.c
 *
 * This file contains the base functions to manage periodic tick
 * related events.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 *
 * This code is licenced under the GPL version 2. For details see
 * kernel-base/COPYING.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/module.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Tick devices
 */

/* IAMROOT-12:
 * -------------
 * cpu별로 틱 디바이스를 관리한다.
 *         (클럭이벤트 디바이스를 제어하여 프로그래밍한다.)
 *         (가장 좋은 클럭이벤트 디바이스를 선택한다)
 */
DEFINE_PER_CPU(struct tick_device, tick_cpu_device);
/*
 * Tick next event: keeps track of the tick time
 */
ktime_t tick_next_period;
ktime_t tick_period;

/*
 * tick_do_timer_cpu is a timer core internal variable which holds the CPU NR
 * which is responsible for calling do_timer(), i.e. the timekeeping stuff. This
 * variable has two functions:
 *
 * 1) Prevent a thundering herd issue of a gazillion of CPUs trying to grab the
 *    timekeeping lock all at once. Only the CPU which is assigned to do the
 *    update is handling it.
 *
 * 2) Hand off the duty in the NOHZ idle case by setting the value to
 *    TICK_DO_TIMER_NONE, i.e. a non existing CPU. So the next cpu which looks
 *    at it will take over and keep the time keeping alive.  The handover
 *    procedure also covers cpu hotplug.
 */
int tick_do_timer_cpu __read_mostly = TICK_DO_TIMER_BOOT;

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_device(int cpu)
{
	return &per_cpu(tick_cpu_device, cpu);
}

/**
 * tick_is_oneshot_available - check for a oneshot capable event device
 */
int tick_is_oneshot_available(void)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return 0;
	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 1;
	return tick_broadcast_oneshot_available();
}

/*
 * Periodic tick
 */
static void tick_periodic(int cpu)
{
	if (tick_do_timer_cpu == cpu) {
		write_seqlock(&jiffies_lock);

		/* Keep track of the next tick event */
		tick_next_period = ktime_add(tick_next_period, tick_period);

		do_timer(1);
		write_sequnlock(&jiffies_lock);
		update_wall_time();
	}

	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
void tick_handle_periodic(struct clock_event_device *dev)
{
	int cpu = smp_processor_id();
	ktime_t next = dev->next_event;

	tick_periodic(cpu);

	if (dev->mode != CLOCK_EVT_MODE_ONESHOT)
		return;
	for (;;) {
		/*
		 * Setup the next period for devices, which do not have
		 * periodic mode:
		 */
		next = ktime_add(next, tick_period);

		if (!clockevents_program_event(dev, next, false))
			return;
		/*
		 * Have to be careful here. If we're in oneshot mode,
		 * before we call tick_periodic() in a loop, we need
		 * to be sure we're using a real hardware clocksource.
		 * Otherwise we could get trapped in an infinite
		 * loop, as the tick_periodic() increments jiffies,
		 * which then will increment time, possibly causing
		 * the loop to trigger again and again.
		 */
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
	}
}

/*
 * Setup the device for a periodic tick
 */
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	if (!tick_device_is_functional(dev))
		return;

	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
		clockevents_set_mode(dev, CLOCK_EVT_MODE_PERIODIC);
	} else {
		unsigned long seq;
		ktime_t next;

		do {
			seq = read_seqbegin(&jiffies_lock);
			next = tick_next_period;
		} while (read_seqretry(&jiffies_lock, seq));

		clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);

		for (;;) {
			if (!clockevents_program_event(dev, next, false))
				return;
			next = ktime_add(next, tick_period);
		}
	}
}

/*
 * Setup the tick device
 */
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	ktime_t next_event;
	void (*handler)(struct clock_event_device *) = NULL;

	/*
	 * First device setup ?
	 */
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
		if (tick_do_timer_cpu == TICK_DO_TIMER_BOOT) {
			if (!tick_nohz_full_cpu(cpu))
				tick_do_timer_cpu = cpu;
			else
				tick_do_timer_cpu = TICK_DO_TIMER_NONE;
			tick_next_period = ktime_get();
			tick_period = ktime_set(0, NSEC_PER_SEC / HZ);
		}

		/*
		 * Startup in periodic mode first.
		 */
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}

	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way. This function also returns !=0 when we keep the
	 * current active broadcast state for this CPU.
	 */
	if (tick_device_uses_broadcast(newdev, cpu))
		return;

	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);
	else
		tick_setup_oneshot(newdev, handler, next_event);
}

void tick_install_replacement(struct clock_event_device *newdev)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int cpu = smp_processor_id();

	clockevents_exchange_device(td->evtdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
}

static bool tick_check_percpu(struct clock_event_device *curdev,
			      struct clock_event_device *newdev, int cpu)
{
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		return false;
	if (cpumask_equal(newdev->cpumask, cpumask_of(cpu)))
		return true;
	/* Check if irq affinity can be set */
	if (newdev->irq >= 0 && !irq_can_set_affinity(newdev->irq))
		return false;
	/* Prefer an existing cpu local device */
	if (curdev && cpumask_equal(curdev->cpumask, cpumask_of(cpu)))
		return false;
	return true;
}

static bool tick_check_preferred(struct clock_event_device *curdev,
				 struct clock_event_device *newdev)
{
	/* Prefer oneshot capable device */
	if (!(newdev->features & CLOCK_EVT_FEAT_ONESHOT)) {

/* IAMROOT-12:
 * -------------
 * 새 클럭이벤트 디바이스가 oneshot 기능이 없고 기존 클럭이벤트 디바이스는 
 * 있는 경우 새 클럭이벤트 디바이스를 사용하지 않는다.
 */
		if (curdev && (curdev->features & CLOCK_EVT_FEAT_ONESHOT))
			return false;

/* IAMROOT-12:
 * -------------
 * 기존 틱 디바이스가 이미 oneshot 모드로 동작하는 경우 새 디바이스를 
 * 선택하지 않는다.
 */
		if (tick_oneshot_mode_active())
			return false;
	}

	/*
	 * Use the higher rated one, but prefer a CPU local device with a lower
	 * rating than a non-CPU local device
	 */

/* IAMROOT-12:
 * -------------
 * 다음 조건에서 새 디바이스를 채용할 수 있다.
 * - 기존 디바이스가 없거나 
 * - 기존 디바이스보다 rating이 더 좋거나 
 * - 기존 디바이스는 범용(cpu와 관련 없는)이면서 
 *   새 디바이스는 per-cpu인 경우
 */
	return !curdev ||
		newdev->rating > curdev->rating ||
	       !cpumask_equal(curdev->cpumask, newdev->cpumask);
}

/*
 * Check whether the new device is a better fit than curdev. curdev
 * can be NULL !
 */
bool tick_check_replacement(struct clock_event_device *curdev,
			    struct clock_event_device *newdev)
{
	if (!tick_check_percpu(curdev, newdev, smp_processor_id()))
		return false;

	return tick_check_preferred(curdev, newdev);
}

/*
 * Check, if the new registered device should be used. Called with
 * clockevents_lock held and interrupts disabled.
 */
void tick_check_new_device(struct clock_event_device *newdev)
{
	struct clock_event_device *curdev;
	struct tick_device *td;
	int cpu;

/* IAMROOT-12:
 * -------------
 * 클럭이 현재 cpu를 지원하지 않으면 빠져나간다.
 * (아키텍처 타이머는 cpu별로 cpumask가 설정된다)
 */
	cpu = smp_processor_id();
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		goto out_bc;

/* IAMROOT-12:
 * -------------
 * 이미 지정된 현재 스케줄 틱 디바이스를 알아온다.
 */
	td = &per_cpu(tick_cpu_device, cpu);
	curdev = td->evtdev;

/* IAMROOT-12:
 * -------------
 * cpu별로 구성된 타이머가 아니면 틱 브로드캐스트 디바이스를 설정하도록 
 * out_bc 레이블로 이동한다.
 * (armv7, armv8 타이머는 cpu별로 구성되어 있다)
 */
	/* cpu local device ? */
	if (!tick_check_percpu(curdev, newdev, cpu))
		goto out_bc;

	/* Preference decision */

/* IAMROOT-12:
 * -------------
 * 새 클럭이벤트 디바이스를 틱 디바이스로 선택받지 못한 경우 
 * 틱 브로드캐스트 디바이스를 설정하도록 out_bc 레이블로 이동한다.
 */
	if (!tick_check_preferred(curdev, newdev))
		goto out_bc;

/* IAMROOT-12:
 * -------------
 * 모듈에 대한 레퍼런스 카운터를 증가시킨다.
 */
	if (!try_module_get(newdev->owner))
		return;

	/*
	 * Replace the eventually existing device by the new
	 * device. If the current device is the broadcast device, do
	 * not give it back to the clockevents layer !
	 */

/* IAMROOT-12:
 * -------------
 * 기존 클럭 이벤트 디바이스가 틱 브로드캐스트용도로 사용되는 경우 
 * 기존 클럭 디바이스를 셧다운 설정한다.
 */
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}

/* IAMROOT-12:
 * -------------
 * 틱 디바이스로 새 클럭 이벤트 디바이스를 선택한다.
 * (shutdown으로 시작한다)
 */
	clockevents_exchange_device(curdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
	return;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	tick_install_broadcast_device(newdev);
}

/*
 * Transfer the do_timer job away from a dying cpu.
 *
 * Called with interrupts disabled.
 */
void tick_handover_do_timer(int *cpup)
{
	if (*cpup == tick_do_timer_cpu) {
		int cpu = cpumask_first(cpu_online_mask);

		tick_do_timer_cpu = (cpu < nr_cpu_ids) ? cpu :
			TICK_DO_TIMER_NONE;
	}
}

/*
 * Shutdown an event device on a given cpu:
 *
 * This is called on a life CPU, when a CPU is dead. So we cannot
 * access the hardware device itself.
 * We just set the mode and remove it from the lists.
 */
void tick_shutdown(unsigned int *cpup)
{
	struct tick_device *td = &per_cpu(tick_cpu_device, *cpup);
	struct clock_event_device *dev = td->evtdev;

	td->mode = TICKDEV_MODE_PERIODIC;
	if (dev) {
		/*
		 * Prevent that the clock events layer tries to call
		 * the set mode function!
		 */
		dev->mode = CLOCK_EVT_MODE_UNUSED;
		clockevents_exchange_device(dev, NULL);
		dev->event_handler = clockevents_handle_noop;
		td->evtdev = NULL;
	}
}

void tick_suspend(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

	clockevents_shutdown(td->evtdev);
}

void tick_resume(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int broadcast = tick_resume_broadcast();

	clockevents_set_mode(td->evtdev, CLOCK_EVT_MODE_RESUME);

	if (!broadcast) {
		if (td->mode == TICKDEV_MODE_PERIODIC)
			tick_setup_periodic(td->evtdev, 0);
		else
			tick_resume_oneshot();
	}
}

static DEFINE_RAW_SPINLOCK(tick_freeze_lock);
static unsigned int tick_freeze_depth;

/**
 * tick_freeze - Suspend the local tick and (possibly) timekeeping.
 *
 * Check if this is the last online CPU executing the function and if so,
 * suspend timekeeping.  Otherwise suspend the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_unfreeze().
 * Interrupts must not be enabled before the subsequent %tick_unfreeze().
 */
void tick_freeze(void)
{
	raw_spin_lock(&tick_freeze_lock);

	tick_freeze_depth++;
	if (tick_freeze_depth == num_online_cpus()) {
		timekeeping_suspend();
	} else {
		tick_suspend();
		tick_suspend_broadcast();
	}

	raw_spin_unlock(&tick_freeze_lock);
}

/**
 * tick_unfreeze - Resume the local tick and (possibly) timekeeping.
 *
 * Check if this is the first CPU executing the function and if so, resume
 * timekeeping.  Otherwise resume the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_freeze().
 * Interrupts must not be enabled after the preceding %tick_freeze().
 */
void tick_unfreeze(void)
{
	raw_spin_lock(&tick_freeze_lock);

	if (tick_freeze_depth == num_online_cpus())
		timekeeping_resume();
	else
		tick_resume();

	tick_freeze_depth--;

	raw_spin_unlock(&tick_freeze_lock);
}

/**
 * tick_init - initialize the tick control
 */
void __init tick_init(void)
{
	tick_broadcast_init();
	tick_nohz_init();
}
