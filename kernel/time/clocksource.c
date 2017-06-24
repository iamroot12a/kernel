/*
 * linux/kernel/time/clocksource.c
 *
 * This file contains the functions which manage clocksource drivers.
 *
 * Copyright (C) 2004, 2005 IBM, John Stultz (johnstul@us.ibm.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * TODO WishList:
 *   o Allow clocksource drivers to be unregistered
 */

#include <linux/device.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h> /* for spin_unlock_irq() using preempt_count() m68k */
#include <linux/tick.h>
#include <linux/kthread.h>

#include "tick-internal.h"
#include "timekeeping_internal.h"

/**
 * clocks_calc_mult_shift - calculate mult/shift factors for scaled math of clocks
 * @mult:	pointer to mult variable
 * @shift:	pointer to shift variable
 * @from:	frequency to convert from
 * @to:		frequency to convert to
 * @maxsec:	guaranteed runtime conversion range in seconds
 *
 * The function evaluates the shift/mult pair for the scaled math
 * operations of clocksources and clockevents.
 *
 * @to and @from are frequency values in HZ. For clock sources @to is
 * NSEC_PER_SEC == 1GHz and @from is the counter frequency. For clock
 * event @to is the counter frequency and @from is NSEC_PER_SEC.
 *
 * The @maxsec conversion range argument controls the time frame in
 * seconds which must be covered by the runtime conversion with the
 * calculated mult and shift factors. This guarantees that no 64bit
 * overflow happens when the input value of the conversion is
 * multiplied with the calculated mult factor. Larger ranges may
 * reduce the conversion accuracy by chosing smaller mult and shift
 * factors.
 */
void
clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */

/* IAMROOT-12:
 * -------------
 * from=1G(10E9) -> to=19.2M, maxsec=111
 *	tmp=0b0001_1001 (25)
 *		 <----> (5번 shift해서 0을 만들 수 있다.)
 *      sftacc=32-5=27
 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}

/*[Clocksource internal variables]---------
 * curr_clocksource:
 *	currently selected clocksource.
 * clocksource_list:
 *	linked list with the registered clocksources
 * clocksource_mutex:
 *	protects manipulations to curr_clocksource and the clocksource_list
 * override_name:
 *	Name of the user-specified clocksource.
 */
static struct clocksource *curr_clocksource;
static LIST_HEAD(clocksource_list);
static DEFINE_MUTEX(clocksource_mutex);
static char override_name[CS_NAME_LEN];
static int finished_booting;

#ifdef CONFIG_CLOCKSOURCE_WATCHDOG
static void clocksource_watchdog_work(struct work_struct *work);
static void clocksource_select(void);

static LIST_HEAD(watchdog_list);
static struct clocksource *watchdog;
static struct timer_list watchdog_timer;
static DECLARE_WORK(watchdog_work, clocksource_watchdog_work);
static DEFINE_SPINLOCK(watchdog_lock);
static int watchdog_running;
static atomic_t watchdog_reset_pending;

static int clocksource_watchdog_kthread(void *data);
static void __clocksource_change_rating(struct clocksource *cs, int rating);

/*
 * Interval: 0.5sec Threshold: 0.0625s
 */
#define WATCHDOG_INTERVAL (HZ >> 1)
#define WATCHDOG_THRESHOLD (NSEC_PER_SEC >> 4)

static void clocksource_watchdog_work(struct work_struct *work)
{
	/*
	 * If kthread_run fails the next watchdog scan over the
	 * watchdog_list will find the unstable clock again.
	 */
	kthread_run(clocksource_watchdog_kthread, NULL, "kwatchdog");
}

static void __clocksource_unstable(struct clocksource *cs)
{
	cs->flags &= ~(CLOCK_SOURCE_VALID_FOR_HRES | CLOCK_SOURCE_WATCHDOG);
	cs->flags |= CLOCK_SOURCE_UNSTABLE;
	if (finished_booting)
		schedule_work(&watchdog_work);
}

static void clocksource_unstable(struct clocksource *cs, int64_t delta)
{
	printk(KERN_WARNING "Clocksource %s unstable (delta = %Ld ns)\n",
	       cs->name, delta);
	__clocksource_unstable(cs);
}

/**
 * clocksource_mark_unstable - mark clocksource unstable via watchdog
 * @cs:		clocksource to be marked unstable
 *
 * This function is called instead of clocksource_change_rating from
 * cpu hotplug code to avoid a deadlock between the clocksource mutex
 * and the cpu hotplug mutex. It defers the update of the clocksource
 * to the watchdog thread.
 */
void clocksource_mark_unstable(struct clocksource *cs)
{
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	if (!(cs->flags & CLOCK_SOURCE_UNSTABLE)) {
		if (list_empty(&cs->wd_list))
			list_add(&cs->wd_list, &watchdog_list);
		__clocksource_unstable(cs);
	}
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

static void clocksource_watchdog(unsigned long data)
{
	struct clocksource *cs;
	cycle_t csnow, wdnow, delta;
	int64_t wd_nsec, cs_nsec;
	int next_cpu, reset_pending;

	spin_lock(&watchdog_lock);
	if (!watchdog_running)
		goto out;

	reset_pending = atomic_read(&watchdog_reset_pending);

	list_for_each_entry(cs, &watchdog_list, wd_list) {

		/* Clocksource already marked unstable? */
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			if (finished_booting)
				schedule_work(&watchdog_work);
			continue;
		}

		local_irq_disable();
		csnow = cs->read(cs);
		wdnow = watchdog->read(watchdog);
		local_irq_enable();

		/* Clocksource initialized ? */
		if (!(cs->flags & CLOCK_SOURCE_WATCHDOG) ||
		    atomic_read(&watchdog_reset_pending)) {
			cs->flags |= CLOCK_SOURCE_WATCHDOG;
			cs->wd_last = wdnow;
			cs->cs_last = csnow;
			continue;
		}

		delta = clocksource_delta(wdnow, cs->wd_last, watchdog->mask);
		wd_nsec = clocksource_cyc2ns(delta, watchdog->mult,
					     watchdog->shift);

		delta = clocksource_delta(csnow, cs->cs_last, cs->mask);
		cs_nsec = clocksource_cyc2ns(delta, cs->mult, cs->shift);
		cs->cs_last = csnow;
		cs->wd_last = wdnow;

		if (atomic_read(&watchdog_reset_pending))
			continue;

		/* Check the deviation from the watchdog clocksource. */
		if ((abs(cs_nsec - wd_nsec) > WATCHDOG_THRESHOLD)) {
			clocksource_unstable(cs, cs_nsec - wd_nsec);
			continue;
		}

		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) &&
		    (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS) &&
		    (watchdog->flags & CLOCK_SOURCE_IS_CONTINUOUS)) {
			/* Mark it valid for high-res. */
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;

			/*
			 * clocksource_done_booting() will sort it if
			 * finished_booting is not set yet.
			 */
			if (!finished_booting)
				continue;

			/*
			 * If this is not the current clocksource let
			 * the watchdog thread reselect it. Due to the
			 * change to high res this clocksource might
			 * be preferred now. If it is the current
			 * clocksource let the tick code know about
			 * that change.
			 */
			if (cs != curr_clocksource) {
				cs->flags |= CLOCK_SOURCE_RESELECT;
				schedule_work(&watchdog_work);
			} else {
				tick_clock_notify();
			}
		}
	}

	/*
	 * We only clear the watchdog_reset_pending, when we did a
	 * full cycle through all clocksources.
	 */
	if (reset_pending)
		atomic_dec(&watchdog_reset_pending);

	/*
	 * Cycle through CPUs to check if the CPUs stay synchronized
	 * to each other.
	 */
	next_cpu = cpumask_next(raw_smp_processor_id(), cpu_online_mask);
	if (next_cpu >= nr_cpu_ids)
		next_cpu = cpumask_first(cpu_online_mask);
	watchdog_timer.expires += WATCHDOG_INTERVAL;
	add_timer_on(&watchdog_timer, next_cpu);
out:
	spin_unlock(&watchdog_lock);
}

static inline void clocksource_start_watchdog(void)
{
	if (watchdog_running || !watchdog || list_empty(&watchdog_list))
		return;
	init_timer(&watchdog_timer);
	watchdog_timer.function = clocksource_watchdog;
	watchdog_timer.expires = jiffies + WATCHDOG_INTERVAL;
	add_timer_on(&watchdog_timer, cpumask_first(cpu_online_mask));
	watchdog_running = 1;
}

static inline void clocksource_stop_watchdog(void)
{
	if (!watchdog_running || (watchdog && !list_empty(&watchdog_list)))
		return;
	del_timer(&watchdog_timer);
	watchdog_running = 0;
}

static inline void clocksource_reset_watchdog(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &watchdog_list, wd_list)
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
}

static void clocksource_resume_watchdog(void)
{
	atomic_inc(&watchdog_reset_pending);
}

static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
		/* cs is a clocksource to be watched. */
		list_add(&cs->wd_list, &watchdog_list);
		cs->flags &= ~CLOCK_SOURCE_WATCHDOG;
	} else {
		/* cs is a watchdog. */
		if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
		/* Pick the best watchdog. */
		if (!watchdog || cs->rating > watchdog->rating) {
			watchdog = cs;
			/* Reset watchdog cycles */
			clocksource_reset_watchdog();
		}
	}
	/* Check if the watchdog timer needs to be started. */
	clocksource_start_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

static void clocksource_dequeue_watchdog(struct clocksource *cs)
{
	unsigned long flags;

	spin_lock_irqsave(&watchdog_lock, flags);
	if (cs != watchdog) {
		if (cs->flags & CLOCK_SOURCE_MUST_VERIFY) {
			/* cs is a watched clocksource. */
			list_del_init(&cs->wd_list);
			/* Check if the watchdog timer needs to be stopped. */
			clocksource_stop_watchdog();
		}
	}
	spin_unlock_irqrestore(&watchdog_lock, flags);
}

static int __clocksource_watchdog_kthread(void)
{
	struct clocksource *cs, *tmp;
	unsigned long flags;
	LIST_HEAD(unstable);
	int select = 0;

	spin_lock_irqsave(&watchdog_lock, flags);
	list_for_each_entry_safe(cs, tmp, &watchdog_list, wd_list) {
		if (cs->flags & CLOCK_SOURCE_UNSTABLE) {
			list_del_init(&cs->wd_list);
			list_add(&cs->wd_list, &unstable);
			select = 1;
		}
		if (cs->flags & CLOCK_SOURCE_RESELECT) {
			cs->flags &= ~CLOCK_SOURCE_RESELECT;
			select = 1;
		}
	}
	/* Check if the watchdog timer needs to be stopped. */
	clocksource_stop_watchdog();
	spin_unlock_irqrestore(&watchdog_lock, flags);

	/* Needs to be done outside of watchdog lock */
	list_for_each_entry_safe(cs, tmp, &unstable, wd_list) {
		list_del_init(&cs->wd_list);
		__clocksource_change_rating(cs, 0);
	}
	return select;
}

static int clocksource_watchdog_kthread(void *data)
{
	mutex_lock(&clocksource_mutex);
	if (__clocksource_watchdog_kthread())
		clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}

static bool clocksource_is_watchdog(struct clocksource *cs)
{
	return cs == watchdog;
}

#else /* CONFIG_CLOCKSOURCE_WATCHDOG */

static void clocksource_enqueue_watchdog(struct clocksource *cs)
{
	if (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS)
		cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;
}

static inline void clocksource_dequeue_watchdog(struct clocksource *cs) { }
static inline void clocksource_resume_watchdog(void) { }
static inline int __clocksource_watchdog_kthread(void) { return 0; }
static bool clocksource_is_watchdog(struct clocksource *cs) { return false; }
void clocksource_mark_unstable(struct clocksource *cs) { }

#endif /* CONFIG_CLOCKSOURCE_WATCHDOG */

/**
 * clocksource_suspend - suspend the clocksource(s)
 */
void clocksource_suspend(void)
{
	struct clocksource *cs;

	list_for_each_entry_reverse(cs, &clocksource_list, list)
		if (cs->suspend)
			cs->suspend(cs);
}

/**
 * clocksource_resume - resume the clocksource(s)
 */
void clocksource_resume(void)
{
	struct clocksource *cs;

	list_for_each_entry(cs, &clocksource_list, list)
		if (cs->resume)
			cs->resume(cs);

	clocksource_resume_watchdog();
}

/**
 * clocksource_touch_watchdog - Update watchdog
 *
 * Update the watchdog after exception contexts such as kgdb so as not
 * to incorrectly trip the watchdog. This might fail when the kernel
 * was stopped in code which holds watchdog_lock.
 */
void clocksource_touch_watchdog(void)
{
	clocksource_resume_watchdog();
}

/**
 * clocksource_max_adjustment- Returns max adjustment amount
 * @cs:         Pointer to clocksource
 *
 */
static u32 clocksource_max_adjustment(struct clocksource *cs)
{
	u64 ret;
	/*
	 * We won't try to correct for more than 11% adjustments (110,000 ppm),
	 */

/* IAMROOT-12:
 * -------------
 * mult의 11% 값을 반환한다.
 */
	ret = (u64)cs->mult * 11;
	do_div(ret,100);
	return (u32)ret;
}

/**
 * clocks_calc_max_nsecs - Returns maximum nanoseconds that can be converted
 * @mult:	cycle to nanosecond multiplier
 * @shift:	cycle to nanosecond divisor (power of two)
 * @maxadj:	maximum adjustment value to mult (~11%)
 * @mask:	bitmask for two's complement subtraction of non 64 bit counters
 */

/* IAMROOT-12:
 * -------------
 * mult, shift, maxadj, mask를 사용해서 변환될 수 있는 최대 나노초
 */
u64 clocks_calc_max_nsecs(u32 mult, u32 shift, u32 maxadj, u64 mask)
{
	u64 max_nsecs, max_cycles;

	/*
	 * Calculate the maximum number of cycles that we can pass to the
	 * cyc2ns function without overflowing a 64-bit signed result. The
	 * maximum number of cycles is equal to ULLONG_MAX/(mult+maxadj)
	 * which is equivalent to the below.
	 * max_cycles < (2^63)/(mult + maxadj)
	 * max_cycles < 2^(log2((2^63)/(mult + maxadj)))
	 * max_cycles < 2^(log2(2^63) - log2(mult + maxadj))
	 * max_cycles < 2^(63 - log2(mult + maxadj))
	 * max_cycles < 1 << (63 - log2(mult + maxadj))
	 * Please note that we add 1 to the result of the log2 to account for
	 * any rounding errors, ensure the above inequality is satisfied and
	 * no overflow will occur.
	 */

/* IAMROOT-12:
 * -------------
 * mult=0x3415_5555 + maxadj 한 값이 사용하는 비트는 30bit이다.
 * 따라서 아래는 63-30=33bit,    2^33 = 0x2_0000_0000
 */
	max_cycles = 1ULL << (63 - (ilog2(mult + maxadj) + 1));

	/*
	 * The actual maximum number of cycles we can defer the clocksource is
	 * determined by the minimum of max_cycles and mask.
	 * Note: Here we subtract the maxadj to make sure we don't sleep for
	 * too long if there's a large negative adjustment.
	 */
	max_cycles = min(max_cycles, mask);

/* IAMROOT-12:
 * -------------
 * 0x2_0000_0000 * (0x3415_5555에서 11% 뺀 수) >> 24 
 */

	max_nsecs = clocksource_cyc2ns(max_cycles, mult - maxadj, shift);

	return max_nsecs;
}

/**
 * clocksource_max_deferment - Returns max time the clocksource can be deferred
 * @cs:         Pointer to clocksource
 *
 */
static u64 clocksource_max_deferment(struct clocksource *cs)
{
	u64 max_nsecs;

	max_nsecs = clocks_calc_max_nsecs(cs->mult, cs->shift, cs->maxadj,
					  cs->mask);
	/*
	 * To ensure that the clocksource does not wrap whilst we are idle,
	 * limit the time the clocksource can be deferred by 12.5%. Please
	 * note a margin of 12.5% is used because this can be computed with
	 * a shift, versus say 10% which would require division.
	 */

/* IAMROOT-12:
 * -------------
 * 12.5% 마진을 빼서 반환한다.
 */
	return max_nsecs - (max_nsecs >> 3);
}

#ifndef CONFIG_ARCH_USES_GETTIMEOFFSET

/* IAMROOT-12:
 * -------------
 * CONFIG_ARCH_USES_GETTIMEOFFSET: 예전 스타일에서 사용한다.
 * (대부분의 arm은 이 커널 옵션을 사용하지 않는다)
 *
 * 현재 사용하는 틱모드가 oneshot 모드가 아닌 경우 rating이 가장 좋은 
 * 클럭 소스를 선택한다. 단 oneshot 모드인 경우 high-resolution 클럭 리소스를
 * 선택해야 한다.
 */
static struct clocksource *clocksource_find_best(bool oneshot, bool skipcur)
{
	struct clocksource *cs;


/* IAMROOT-12:
 * -------------
 * 클럭 소스가 아직 결정되지 않은 경우 또는 클럭 소스 리스트가 비어 있는 경우
 * 함수를 빠져나간다.
 *
 * fs_initcall(clocksource_done_booting)을 통해 finished_booting=1이 된다.
 * 그 전까지는 0이므로 함수를 빠져나간다.
 */
	if (!finished_booting || list_empty(&clocksource_list))
		return NULL;

	/*
	 * We pick the clocksource with the highest rating. If oneshot
	 * mode is active, we pick the highres valid clocksource with
	 * the best rating.
	 */
	list_for_each_entry(cs, &clocksource_list, list) {

/* IAMROOT-12:
 * -------------
 * rating 순으로 정렬된 클럭 소스 리스트에서 
 *	skipcur가 지정된 경우 현재 클럭 소스를 제외하고
 *	이미 oneshot 클럭소스를 선택한 경우 high-resolution만을 요청한다.
 */

		if (skipcur && cs == curr_clocksource)
			continue;
		if (oneshot && !(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			continue;
		return cs;
	}
	return NULL;
}

static void __clocksource_select(bool skipcur)
{

/* IAMROOT-12:
 * -------------
 * 틱 디바이스가 one shot으로 동작하는지 여부를 알아온다.
 */
	bool oneshot = tick_oneshot_mode_active();
	struct clocksource *best, *cs;

	/* Find the best suitable clocksource */
	best = clocksource_find_best(oneshot, skipcur);
	if (!best)
		return;

	/* Check for the override clocksource. */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (strcmp(cs->name, override_name) != 0)
			continue;
		/*
		 * Check to make sure we don't switch to a non-highres
		 * capable clocksource if the tick code is in oneshot
		 * mode (highres or nohz)
		 */
		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) && oneshot) {
			/* Override clocksource cannot be used. */
			printk(KERN_WARNING "Override clocksource %s is not "
			       "HRT compatible. Cannot switch while in "
			       "HRT/NOHZ mode\n", cs->name);
			override_name[0] = 0;
		} else
			/* Override clocksource can be used. */
			best = cs;
		break;
	}

	if (curr_clocksource != best && !timekeeping_notify(best)) {
		pr_info("Switched to clocksource %s\n", best->name);
		curr_clocksource = best;
	}
}

/**
 * clocksource_select - Select the best clocksource available
 *
 * Private function. Must hold clocksource_mutex when called.
 *
 * Select the clocksource with the best rating, or the clocksource,
 * which is selected by userspace override.
 */
static void clocksource_select(void)
{

/* IAMROOT-12:
 * -------------
 * 현재 이미 선택되어 있는 클럭을 제외하고 클럭 소스를 선택하게 한다.
 */
	return __clocksource_select(false);
}

static void clocksource_select_fallback(void)
{
	return __clocksource_select(true);
}

#else /* !CONFIG_ARCH_USES_GETTIMEOFFSET */

static inline void clocksource_select(void) { }
static inline void clocksource_select_fallback(void) { }

#endif

/*
 * clocksource_done_booting - Called near the end of core bootup
 *
 * Hack to avoid lots of clocksource churn at boot time.
 * We use fs_initcall because we want this to start before
 * device_initcall but after subsys_initcall.
 */
static int __init clocksource_done_booting(void)
{
	mutex_lock(&clocksource_mutex);
	curr_clocksource = clocksource_default_clock();
	finished_booting = 1;
	/*
	 * Run the watchdog first to eliminate unstable clock sources
	 */
	__clocksource_watchdog_kthread();
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}
fs_initcall(clocksource_done_booting);

/*
 * Enqueue the clocksource sorted by rating
 */
static void clocksource_enqueue(struct clocksource *cs)
{
	struct list_head *entry = &clocksource_list;
	struct clocksource *tmp;

/* IAMROOT-12:
 * -------------
 * 클럭 소스 리스트에 요청한 클럭 소스를 추가한다.
 * (선두가 rating이 높은 것)
 */
	list_for_each_entry(tmp, &clocksource_list, list)
		/* Keep track of the place, where to insert */
		if (tmp->rating >= cs->rating)
			entry = &tmp->list;
	list_add(&cs->list, entry);
}

/**
 * __clocksource_updatefreq_scale - Used update clocksource with new freq
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 *
 * This should only be called from the clocksource->enable() method.
 *
 * This *SHOULD NOT* be called directly! Please use the
 * clocksource_updatefreq_hz() or clocksource_updatefreq_khz helper functions.
 */
void __clocksource_updatefreq_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	u64 sec;
	/*
	 * Calc the maximum number of seconds which we can run before
	 * wrapping around. For clocksources which have a mask > 32bit
	 * we need to limit the max sleep time to have a good
	 * conversion precision. 10 minutes is still a reasonable
	 * amount. That results in a shift value of 24 for a
	 * clocksource with mask >= 40bit and f >= 4GHz. That maps to
	 * ~ 0.06ppm granularity for NTP. We apply the same 12.5%
	 * margin as we do in clocksource_max_deferment()
	 */

/* IAMROOT-12:
 * -------------
 * 타이머가 사용하는 카운터 최대 값에서 마진 12.5%를 빼고 주파수로 나눈 값이 
 * 최대 사용할 수 있는 초라고 산출해낸다. 이 값이 10분 이상인 경우 그 이상의 
 * 기간은 필요 없으므로 최대 10분으로 제한한다.
 *
 * 10분으로 제한을 시키면 그 만큼 여유가 되는 비트를 정밀도(shift)를 
 * 올리는데 사용할 수 있다.
 *
 * arch 타이머: cs->mask=(2^56)-1
 *              0x00ff_ffff_ffff_ffff 
 *            - 0x001f_ffff_ffff_ffff
 *            -----------------------
 *              0x00e0_0000_0000_0000 (12.5 감소 값)
 *            / 0x          0124_F800 (19.2Mhz)
 *            /                     1 (scale=1)
 *            -----------------------
 *              0x          c3bb_f3a8 (seconds)
 */
	sec = (cs->mask - (cs->mask >> 3));
	do_div(sec, freq);
	do_div(sec, scale);
	if (!sec)
		sec = 1;
	else if (sec > 600 && cs->mask > UINT_MAX)
		sec = 600;

/* IAMROOT-12:
 * -------------
 * rpi2: from=19.2Mhz, to=1G, scale=1, sec=600
 *       -> mult=0x3415_5555, shift=24 (실수=약 52)
 */
	clocks_calc_mult_shift(&cs->mult, &cs->shift, freq,
			       NSEC_PER_SEC / scale, sec * scale);

	/*
	 * for clocksources that have large mults, to avoid overflow.
	 * Since mult may be adjusted by ntp, add an safety extra margin
	 *
	 */

/* IAMROOT-12:
 * -------------
 * mult의 11%에 해당하는 값을 maxadj에 저장한다.
 */
	cs->maxadj = clocksource_max_adjustment(cs);

/* IAMROOT-12:
 * -------------
 * max_adj를 적용한 mult값이 overflow 또는 underflow가 발생하는 경우 
 * 절반으로 줄여나간다. 정밀도(shift)도 줄여나간다.
 */
	while ((cs->mult + cs->maxadj < cs->mult)
		|| (cs->mult - cs->maxadj > cs->mult)) {
		cs->mult >>= 1;
		cs->shift--;
		cs->maxadj = clocksource_max_adjustment(cs);
	}


/* IAMROOT-12:
 * -------------
 * nohz full(idle)을 위해 프로그래밍을 너무 오랫동안 쉬게 할 수 없다.
 * 최대 idle 범위로 제한하기 위해 이 값을 사용한다.
 */
	cs->max_idle_ns = clocksource_max_deferment(cs);
}
EXPORT_SYMBOL_GPL(__clocksource_updatefreq_scale);

/**
 * __clocksource_register_scale - Used to install new clocksources
 * @cs:		clocksource to be registered
 * @scale:	Scale factor multiplied against freq to get clocksource hz
 * @freq:	clocksource frequency (cycles per second) divided by scale
 *
 * Returns -EBUSY if registration fails, zero otherwise.
 *
 * This *SHOULD NOT* be called directly! Please use the
 * clocksource_register_hz() or clocksource_register_khz helper functions.
 */
int __clocksource_register_scale(struct clocksource *cs, u32 scale, u32 freq)
{

	/* Initialize mult/shift and max_idle_ns */

/* IAMROOT-12:
 * -------------
 * 클럭소스에 대한 등록(mult/shift, max_idle_ns)을 수행한다.
 */
	__clocksource_updatefreq_scale(cs, scale, freq);

	/* Add clocksource to the clocksource list */
	mutex_lock(&clocksource_mutex);
	clocksource_enqueue(cs);
	clocksource_enqueue_watchdog(cs);

/* IAMROOT-12:
 * -------------
 * 현재 이미 선택되어 있는 클럭을 제외하고 클럭 소스를 선택하게 한다.
 */
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(__clocksource_register_scale);


/**
 * clocksource_register - Used to install new clocksources
 * @cs:		clocksource to be registered
 *
 * Returns -EBUSY if registration fails, zero otherwise.
 */
int clocksource_register(struct clocksource *cs)
{
	/* calculate max adjustment for given mult/shift */
	cs->maxadj = clocksource_max_adjustment(cs);
	WARN_ONCE(cs->mult + cs->maxadj < cs->mult,
		"Clocksource %s might overflow on 11%% adjustment\n",
		cs->name);

	/* calculate max idle time permitted for this clocksource */

/* IAMROOT-12:
 * -------------
 * nohz full을 위해 틱 프로그래밍 시 이 범위를 벗어나지 않도록 하기 위해 
 * 제시 되는 시간이다.
 * (rpi2: 348초로 제한된다)
 *
 * 클럭소스에 대해 준비된 값들:
 *	- mult & shift
 *	- min_delta_ns & max_delta_ns
 *	- mode 
 *	- rating
 *	- max_idle_ns
 */
	cs->max_idle_ns = clocksource_max_deferment(cs);

	mutex_lock(&clocksource_mutex);

/* IAMROOT-12:
 * -------------
 * 클럭 소스를 클럭 소스 리스트에 추가할 때 rating 내림 차순으로 한다. 
 */
	clocksource_enqueue(cs);

/* IAMROOT-12:
 * -------------
 * x86등에서 unstable한 클럭을 위한 워치독이다.
 */
	clocksource_enqueue_watchdog(cs);

/* IAMROOT-12:
 * -------------
 * 가장 rating이 좋은 클럭 소스를 선택해온다.
 */
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
	return 0;
}
EXPORT_SYMBOL(clocksource_register);

static void __clocksource_change_rating(struct clocksource *cs, int rating)
{
	list_del(&cs->list);
	cs->rating = rating;
	clocksource_enqueue(cs);
}

/**
 * clocksource_change_rating - Change the rating of a registered clocksource
 * @cs:		clocksource to be changed
 * @rating:	new rating
 */
void clocksource_change_rating(struct clocksource *cs, int rating)
{
	mutex_lock(&clocksource_mutex);
	__clocksource_change_rating(cs, rating);
	clocksource_select();
	mutex_unlock(&clocksource_mutex);
}
EXPORT_SYMBOL(clocksource_change_rating);

/*
 * Unbind clocksource @cs. Called with clocksource_mutex held
 */
static int clocksource_unbind(struct clocksource *cs)
{
	/*
	 * I really can't convince myself to support this on hardware
	 * designed by lobotomized monkeys.
	 */
	if (clocksource_is_watchdog(cs))
		return -EBUSY;

	if (cs == curr_clocksource) {
		/* Select and try to install a replacement clock source */
		clocksource_select_fallback();
		if (curr_clocksource == cs)
			return -EBUSY;
	}
	clocksource_dequeue_watchdog(cs);
	list_del_init(&cs->list);
	return 0;
}

/**
 * clocksource_unregister - remove a registered clocksource
 * @cs:	clocksource to be unregistered
 */
int clocksource_unregister(struct clocksource *cs)
{
	int ret = 0;

	mutex_lock(&clocksource_mutex);
	if (!list_empty(&cs->list))
		ret = clocksource_unbind(cs);
	mutex_unlock(&clocksource_mutex);
	return ret;
}
EXPORT_SYMBOL(clocksource_unregister);

#ifdef CONFIG_SYSFS
/**
 * sysfs_show_current_clocksources - sysfs interface for current clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing current clocksource.
 */
static ssize_t
sysfs_show_current_clocksources(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	count = snprintf(buf, PAGE_SIZE, "%s\n", curr_clocksource->name);
	mutex_unlock(&clocksource_mutex);

	return count;
}

ssize_t sysfs_get_uname(const char *buf, char *dst, size_t cnt)
{
	size_t ret = cnt;

	/* strings from sysfs write are not 0 terminated! */
	if (!cnt || cnt >= CS_NAME_LEN)
		return -EINVAL;

	/* strip of \n: */
	if (buf[cnt-1] == '\n')
		cnt--;
	if (cnt > 0)
		memcpy(dst, buf, cnt);
	dst[cnt] = 0;
	return ret;
}

/**
 * sysfs_override_clocksource - interface for manually overriding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	name of override clocksource
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually overriding the default
 * clocksource selection.
 */
static ssize_t sysfs_override_clocksource(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	ssize_t ret;

	mutex_lock(&clocksource_mutex);

	ret = sysfs_get_uname(buf, override_name, count);
	if (ret >= 0)
		clocksource_select();

	mutex_unlock(&clocksource_mutex);

	return ret;
}

/**
 * sysfs_unbind_current_clocksource - interface for manually unbinding clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	unused
 * @count:	length of buffer
 *
 * Takes input from sysfs interface for manually unbinding a clocksource.
 */
static ssize_t sysfs_unbind_clocksource(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct clocksource *cs;
	char name[CS_NAME_LEN];
	ssize_t ret;

	ret = sysfs_get_uname(buf, name, count);
	if (ret < 0)
		return ret;

	ret = -ENODEV;
	mutex_lock(&clocksource_mutex);
	list_for_each_entry(cs, &clocksource_list, list) {
		if (strcmp(cs->name, name))
			continue;
		ret = clocksource_unbind(cs);
		break;
	}
	mutex_unlock(&clocksource_mutex);

	return ret ? ret : count;
}

/**
 * sysfs_show_available_clocksources - sysfs interface for listing clocksource
 * @dev:	unused
 * @attr:	unused
 * @buf:	char buffer to be filled with clocksource list
 *
 * Provides sysfs interface for listing registered clocksources
 */
static ssize_t
sysfs_show_available_clocksources(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct clocksource *src;
	ssize_t count = 0;

	mutex_lock(&clocksource_mutex);
	list_for_each_entry(src, &clocksource_list, list) {
		/*
		 * Don't show non-HRES clocksource if the tick code is
		 * in one shot mode (highres=on or nohz=on)
		 */
		if (!tick_oneshot_mode_active() ||
		    (src->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			count += snprintf(buf + count,
				  max((ssize_t)PAGE_SIZE - count, (ssize_t)0),
				  "%s ", src->name);
	}
	mutex_unlock(&clocksource_mutex);

	count += snprintf(buf + count,
			  max((ssize_t)PAGE_SIZE - count, (ssize_t)0), "\n");

	return count;
}

/*
 * Sysfs setup bits:
 */
static DEVICE_ATTR(current_clocksource, 0644, sysfs_show_current_clocksources,
		   sysfs_override_clocksource);

static DEVICE_ATTR(unbind_clocksource, 0200, NULL, sysfs_unbind_clocksource);

static DEVICE_ATTR(available_clocksource, 0444,
		   sysfs_show_available_clocksources, NULL);

static struct bus_type clocksource_subsys = {
	.name = "clocksource",
	.dev_name = "clocksource",
};

static struct device device_clocksource = {
	.id	= 0,
	.bus	= &clocksource_subsys,
};

static int __init init_clocksource_sysfs(void)
{
	int error = subsys_system_register(&clocksource_subsys, NULL);

	if (!error)
		error = device_register(&device_clocksource);
	if (!error)
		error = device_create_file(
				&device_clocksource,
				&dev_attr_current_clocksource);
	if (!error)
		error = device_create_file(&device_clocksource,
					   &dev_attr_unbind_clocksource);
	if (!error)
		error = device_create_file(
				&device_clocksource,
				&dev_attr_available_clocksource);
	return error;
}

device_initcall(init_clocksource_sysfs);
#endif /* CONFIG_SYSFS */

/**
 * boot_override_clocksource - boot clock override
 * @str:	override name
 *
 * Takes a clocksource= boot argument and uses it
 * as the clocksource override name.
 */
static int __init boot_override_clocksource(char* str)
{
	mutex_lock(&clocksource_mutex);
	if (str)
		strlcpy(override_name, str, sizeof(override_name));
	mutex_unlock(&clocksource_mutex);
	return 1;
}

__setup("clocksource=", boot_override_clocksource);

/**
 * boot_override_clock - Compatibility layer for deprecated boot option
 * @str:	override name
 *
 * DEPRECATED! Takes a clock= boot argument and uses it
 * as the clocksource override name
 */
static int __init boot_override_clock(char* str)
{
	if (!strcmp(str, "pmtmr")) {
		printk("Warning: clock=pmtmr is deprecated. "
			"Use clocksource=acpi_pm.\n");
		return boot_override_clocksource("acpi_pm");
	}
	printk("Warning! clock= boot option is deprecated. "
		"Use clocksource=xyz\n");
	return boot_override_clocksource(str);
}

__setup("clock=", boot_override_clock);
