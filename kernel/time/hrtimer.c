/*
 *  linux/kernel/hrtimer.c
 *
 *  Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 *  Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 *  Copyright(C) 2006-2007  Timesys Corp., Thomas Gleixner
 *
 *  High-resolution kernel timers
 *
 *  In contrast to the low-resolution timeout API implemented in
 *  kernel/timer.c, hrtimers provide finer resolution and accuracy
 *  depending on system configuration and capabilities.
 *
 *  These timers are currently used for:
 *   - itimers
 *   - POSIX timers
 *   - nanosleep
 *   - precise in-kernel timing
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  Credits:
 *	based on kernel/timer.c
 *
 *	Help, testing, suggestions, bugfixes, improvements were
 *	provided by:
 *
 *	George Anzinger, Andrew Morton, Steven Rostedt, Roman Zippel
 *	et. al.
 *
 *  For licencing details see kernel-base/COPYING
 */

#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/notifier.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <linux/seq_file.h>
#include <linux/err.h>
#include <linux/debugobjects.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/sched/deadline.h>
#include <linux/timer.h>
#include <linux/freezer.h>

#include <asm/uaccess.h>

#include <trace/events/timer.h>

#include "timekeeping.h"

/*
 * The timer bases:
 *
 * There are more clockids then hrtimer bases. Thus, we index
 * into the timer bases by the hrtimer_base_type enum. When trying
 * to reach a base using a clockid, hrtimer_clockid_to_base()
 * is used to convert from clockid to the proper hrtimer_base_type.
 */
DEFINE_PER_CPU(struct hrtimer_cpu_base, hrtimer_bases) =
{

	.lock = __RAW_SPIN_LOCK_UNLOCKED(hrtimer_bases.lock),
	.clock_base =
	{

/* IAMROOT-12:
 * -------------
 * monotic 시계는 부팅 시 0부터 시작한다.
 * suspend 기간은 클럭이 동작하지 않는다.
 */
		{
			.index = HRTIMER_BASE_MONOTONIC,
			.clockid = CLOCK_MONOTONIC,
			.get_time = &ktime_get,
			.resolution = KTIME_LOW_RES,
		},
		{

/* IAMROOT-12:
 * -------------
 * 현실 시계와 동일하다.
 * (지정하지 않으면 1970년으로 동작한다. 그러나 커널 빌드업 타임이 설정되게 
 * 할 수도 있다. 그리고 RTC 등의 배터리가 내장된 클럭 디바이스를 통해서 
 * 시스템 종료 후에도 별도록 동작하는 클럭을 사용해서 설정된다.)
 */
			.index = HRTIMER_BASE_REALTIME,
			.clockid = CLOCK_REALTIME,
			.get_time = &ktime_get_real,
			.resolution = KTIME_LOW_RES,
		},

/* IAMROOT-12:
 * -------------
 * monotinic 시계와 동일하게 부팅 시 0부터 시작한다.
 * 단 suspend 한 후 restore 하면 그 동안 suspend 되었던 시간을 계산하여 
 * 더해준다.
 */
		{
			.index = HRTIMER_BASE_BOOTTIME,
			.clockid = CLOCK_BOOTTIME,
			.get_time = &ktime_get_boottime,
			.resolution = KTIME_LOW_RES,
		},

/* IAMROOT-12:
 * -------------
 * 우주 천문 시로 2016년 12월 31일 기점으로 UTC(realtime clock)보다 
 * 37초가 당겨진 시각
 */

		{
			.index = HRTIMER_BASE_TAI,
			.clockid = CLOCK_TAI,
			.get_time = &ktime_get_clocktai,
			.resolution = KTIME_LOW_RES,
		},
	}
};

/* IAMROOT-12:
 * -------------
 * hrtimer가 사용하는 시계는 4가지 이다.
 */
static const int hrtimer_clock_to_base_table[MAX_CLOCKS] = {
	[CLOCK_REALTIME]	= HRTIMER_BASE_REALTIME,
	[CLOCK_MONOTONIC]	= HRTIMER_BASE_MONOTONIC,
	[CLOCK_BOOTTIME]	= HRTIMER_BASE_BOOTTIME,
	[CLOCK_TAI]		= HRTIMER_BASE_TAI,
};

static inline int hrtimer_clockid_to_base(clockid_t clock_id)
{
	return hrtimer_clock_to_base_table[clock_id];
}


/*
 * Get the coarse grained time at the softirq based on xtime and
 * wall_to_monotonic.
 */
static void hrtimer_get_softirq_time(struct hrtimer_cpu_base *base)
{
	ktime_t xtim, mono, boot, tai;
	ktime_t off_real, off_boot, off_tai;

	mono = ktime_get_update_offsets_tick(&off_real, &off_boot, &off_tai);
	boot = ktime_add(mono, off_boot);
	xtim = ktime_add(mono, off_real);
	tai = ktime_add(mono, off_tai);

	base->clock_base[HRTIMER_BASE_REALTIME].softirq_time = xtim;
	base->clock_base[HRTIMER_BASE_MONOTONIC].softirq_time = mono;
	base->clock_base[HRTIMER_BASE_BOOTTIME].softirq_time = boot;
	base->clock_base[HRTIMER_BASE_TAI].softirq_time = tai;
}

/*
 * Functions and macros which are different for UP/SMP systems are kept in a
 * single place
 */
#ifdef CONFIG_SMP

/*
 * We are using hashed locking: holding per_cpu(hrtimer_bases)[n].lock
 * means that all timers which are tied to this base via timer->base are
 * locked, and the base itself is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found on the lists/queues.
 *
 * When the timer's base is locked, and the timer removed from list, it is
 * possible to set timer->base = NULL and drop the lock: the timer remains
 * locked.
 */
static
struct hrtimer_clock_base *lock_hrtimer_base(const struct hrtimer *timer,
					     unsigned long *flags)
{
	struct hrtimer_clock_base *base;

	for (;;) {
		base = timer->base;
		if (likely(base != NULL)) {
			raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);
			if (likely(base == timer->base))
				return base;
			/* The timer has migrated to another CPU: */
			raw_spin_unlock_irqrestore(&base->cpu_base->lock, *flags);
		}
		cpu_relax();
	}
}

/*
 * With HIGHRES=y we do not migrate the timer when it is expiring
 * before the next event on the target cpu because we cannot reprogram
 * the target cpu hardware and we would cause it to fire late.
 *
 * Called with cpu_base->lock of target cpu held.
 */
static int
hrtimer_check_target(struct hrtimer *timer, struct hrtimer_clock_base *new_base)
{
#ifdef CONFIG_HIGH_RES_TIMERS
	ktime_t expires;

/* IAMROOT-12:
 * -------------
 * hw high-resoulution timer가 있는 시스템인 경우 hrtimer가 hres_active가
 * 안되어 있는 경우 false(0)를 반환한다.
 */
	if (!new_base->cpu_base->hres_active)
		return 0;

/* IAMROOT-12:
 * -------------
 * 현재 클럭을 monotonic 클럭 기준의 만료시각으로 변경한다.
 */
	expires = ktime_sub(hrtimer_get_expires(timer), new_base->offset);

/* IAMROOT-12:
 * -------------
 * 현재 타이머의 monotonic으로 변환한 만료 시각이 새 cpu base의 만료 시각보다 
 * 더 이른 경우 true(1)를 반환한다. 이미 hw 타이머를 동작시켰다.
 */
	return expires.tv64 <= new_base->cpu_base->expires_next.tv64;
#else
	return 0;
#endif
}

/*
 * Switch the timer base to the current CPU when possible.
 */
static inline struct hrtimer_clock_base *
switch_hrtimer_base(struct hrtimer *timer, struct hrtimer_clock_base *base,
		    int pinned)
{
	struct hrtimer_clock_base *new_base;
	struct hrtimer_cpu_base *new_cpu_base;
	int this_cpu = smp_processor_id();

/* IAMROOT-12:
 * -------------
 * pinned 및 몇 가지 경우 현재 cpu를 사용하고, 
 * 그렇지 않은 경우 도메인에서 idle하지 않는 cpu를 찾아온다.
 */
	int cpu = get_nohz_timer_target(pinned);
	int basenum = base->index;

again:

/* IAMROOT-12:
 * -------------
 * hrtimer를 사용해야 하는 cpu가 변경될 수 있다. 따라서 아래에서는 
 * 해당하는 cpu base와 clock base를 다시 알아온다.
 */
	new_cpu_base = &per_cpu(hrtimer_bases, cpu);
	new_base = &new_cpu_base->clock_base[basenum];

/* IAMROOT-12:
 * -------------
 * hrtimer의 cpu가 바뀐 경우 
 */
	if (base != new_base) {
		/*
		 * We are trying to move timer to new_base.
		 * However we can't change timer's base while it is running,
		 * so we keep it on the same CPU. No hassle vs. reprogramming
		 * the event source in the high resolution case. The softirq
		 * code will take care of this when the timer function has
		 * completed. There is no conflict as we hold the lock until
		 * the timer is enqueued.
		 */

/* IAMROOT-12:
 * -------------
 * 이미 현재 처리가 되고 있는 중이므로 원래 base를 사용한다.
 */
		if (unlikely(hrtimer_callback_running(timer)))
			return base;

		/* See the comment in lock_timer_base() */
		timer->base = NULL;
		raw_spin_unlock(&base->cpu_base->lock);
		raw_spin_lock(&new_base->cpu_base->lock);


/* IAMROOT-12:
 * -------------
 * 현재 cpu가 아닌 다른 cpu를 결정할 때 그 cpu에서 동작시키는 만료시각보다 
 * 현재 요청한 hrtimer의 만료시각이 더 이른 경우 다시 cpu를 판단하도록
 * 루프를 돈다. (이미 h/w에 요청된 상황이라서 이를 cancel하고 재요청하지 
 * 않는다. 이런 경우에는 그냥 현재 cpu로 new_base를 사용한다.)
 */
		if (cpu != this_cpu && hrtimer_check_target(timer, new_base)) {
			cpu = this_cpu;
			raw_spin_unlock(&new_base->cpu_base->lock);
			raw_spin_lock(&base->cpu_base->lock);
			timer->base = base;
			goto again;
		}
		timer->base = new_base;
	} else {

/* IAMROOT-12:
 * -------------
 * hrtimer의 cpu가 변경되지 않았지만 현재 cpu는 아닌 경우이면서 new_base의 
 * 가장 이른 만료 시각보다 timer의 만료시각이 더 이른 경우 역시 현재 cpu를
 * 사용하도록 new_base를 변경하게 한다.
 */
		if (cpu != this_cpu && hrtimer_check_target(timer, new_base)) {
			cpu = this_cpu;
			goto again;
		}
	}
	return new_base;
}

#else /* CONFIG_SMP */

static inline struct hrtimer_clock_base *
lock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	struct hrtimer_clock_base *base = timer->base;

	raw_spin_lock_irqsave(&base->cpu_base->lock, *flags);

	return base;
}

# define switch_hrtimer_base(t, b, p)	(b)

#endif	/* !CONFIG_SMP */

/*
 * Functions for the union type storage format of ktime_t which are
 * too large for inlining:
 */
#if BITS_PER_LONG < 64
/*
 * Divide a ktime value by a nanosecond value
 */
s64 __ktime_divns(const ktime_t kt, s64 div)
{
	int sft = 0;
	s64 dclc;
	u64 tmp;

	dclc = ktime_to_ns(kt);
	tmp = dclc < 0 ? -dclc : dclc;

	/* Make sure the divisor is less than 2^32: */
	while (div >> 32) {
		sft++;
		div >>= 1;
	}
	tmp >>= sft;
	do_div(tmp, (unsigned long) div);
	return dclc < 0 ? -tmp : tmp;
}
EXPORT_SYMBOL_GPL(__ktime_divns);
#endif /* BITS_PER_LONG >= 64 */

/*
 * Add two ktime values and do a safety check for overflow:
 */
ktime_t ktime_add_safe(const ktime_t lhs, const ktime_t rhs)
{
	ktime_t res = ktime_add(lhs, rhs);

	/*
	 * We use KTIME_SEC_MAX here, the maximum timeout which we can
	 * return to user space in a timespec:
	 */
	if (res.tv64 < 0 || res.tv64 < lhs.tv64 || res.tv64 < rhs.tv64)
		res = ktime_set(KTIME_SEC_MAX, 0);

	return res;
}

EXPORT_SYMBOL_GPL(ktime_add_safe);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static struct debug_obj_descr hrtimer_debug_descr;

static void *hrtimer_debug_hint(void *addr)
{
	return ((struct hrtimer *) addr)->function;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int hrtimer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_init(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
static int hrtimer_fixup_activate(void *addr, enum debug_obj_state state)
{
	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		WARN_ON_ONCE(1);
		return 0;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);

	default:
		return 0;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static int hrtimer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct hrtimer *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		hrtimer_cancel(timer);
		debug_object_free(timer, &hrtimer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

static struct debug_obj_descr hrtimer_debug_descr = {
	.name		= "hrtimer",
	.debug_hint	= hrtimer_debug_hint,
	.fixup_init	= hrtimer_fixup_init,
	.fixup_activate	= hrtimer_fixup_activate,
	.fixup_free	= hrtimer_fixup_free,
};

static inline void debug_hrtimer_init(struct hrtimer *timer)
{
	debug_object_init(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_activate(struct hrtimer *timer)
{
	debug_object_activate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_deactivate(struct hrtimer *timer)
{
	debug_object_deactivate(timer, &hrtimer_debug_descr);
}

static inline void debug_hrtimer_free(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode);

void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	debug_object_init_on_stack(timer, &hrtimer_debug_descr);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init_on_stack);

void destroy_hrtimer_on_stack(struct hrtimer *timer)
{
	debug_object_free(timer, &hrtimer_debug_descr);
}

#else
static inline void debug_hrtimer_init(struct hrtimer *timer) { }
static inline void debug_hrtimer_activate(struct hrtimer *timer) { }
static inline void debug_hrtimer_deactivate(struct hrtimer *timer) { }
#endif

static inline void
debug_init(struct hrtimer *timer, clockid_t clockid,
	   enum hrtimer_mode mode)
{
	debug_hrtimer_init(timer);
	trace_hrtimer_init(timer, clockid, mode);
}

static inline void debug_activate(struct hrtimer *timer)
{
	debug_hrtimer_activate(timer);
	trace_hrtimer_start(timer);
}

static inline void debug_deactivate(struct hrtimer *timer)
{
	debug_hrtimer_deactivate(timer);
	trace_hrtimer_cancel(timer);
}

#if defined(CONFIG_NO_HZ_COMMON) || defined(CONFIG_HIGH_RES_TIMERS)
static ktime_t __hrtimer_get_next_event(struct hrtimer_cpu_base *cpu_base)
{
	struct hrtimer_clock_base *base = cpu_base->clock_base;
	ktime_t expires, expires_next = { .tv64 = KTIME_MAX };
	int i;

/* IAMROOT-12:
 * -------------
 * 요청한 cpu_base에서 4개의 클럭을 검색하여 가장 이른 만료 시각을 알아온다.
 */
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++, base++) {
		struct timerqueue_node *next;
		struct hrtimer *timer;

		next = timerqueue_getnext(&base->active);
		if (!next)
			continue;

		timer = container_of(next, struct hrtimer, node);
		expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
		if (expires.tv64 < expires_next.tv64)
			expires_next = expires;
	}
	/*
	 * clock_was_set() might have changed base->offset of any of
	 * the clock bases so the result might be negative. Fix it up
	 * to prevent a false positive in clockevents_program_event().
	 */
	if (expires_next.tv64 < 0)
		expires_next.tv64 = 0;
	return expires_next;
}
#endif

/* High resolution timer related functions */
#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer enabled ?
 */
static int hrtimer_hres_enabled __read_mostly  = 1;

/*
 * Enable / Disable high resolution mode
 */
static int __init setup_hrtimer_hres(char *str)
{
	if (!strcmp(str, "off"))
		hrtimer_hres_enabled = 0;
	else if (!strcmp(str, "on"))
		hrtimer_hres_enabled = 1;
	else
		return 0;
	return 1;
}

__setup("highres=", setup_hrtimer_hres);

/*
 * hrtimer_high_res_enabled - query, if the highres mode is enabled
 */
static inline int hrtimer_is_hres_enabled(void)
{
	return hrtimer_hres_enabled;
}

/*
 * Is the high resolution mode active ?
 */
static inline int hrtimer_hres_active(void)
{
	return __this_cpu_read(hrtimer_bases.hres_active);
}

/*
 * Reprogram the event source with checking both queues for the
 * next event
 * Called with interrupts disabled and base->lock held
 */
static void
hrtimer_force_reprogram(struct hrtimer_cpu_base *cpu_base, int skip_equal)
{
/* IAMROOT-12:
 * -------------
 * 요청한 cpu_base에서 4개의 클럭을 검색하여 가장 이른 만료 시각을 알아온다.
 */
	ktime_t expires_next = __hrtimer_get_next_event(cpu_base);

/* IAMROOT-12:
 * -------------
 * cpu_base의 가장 이른 만료 시각과 조금 전에 구해 온 가장 이른 만료 시각이 
 * 동일한 경우 skip_equal=1이면 리프로그램 없이 빠져나온다.
 */
	if (skip_equal && expires_next.tv64 == cpu_base->expires_next.tv64)
		return;

/* IAMROOT-12:
 * -------------
 * 다른 경우에라도 cpu_base에 갱신한다.
 */
	cpu_base->expires_next.tv64 = expires_next.tv64;

	/*
	 * If a hang was detected in the last timer interrupt then we
	 * leave the hang delay active in the hardware. We want the
	 * system to make progress. That also prevents the following
	 * scenario:
	 * T1 expires 50ms from now
	 * T2 expires 5s from now
	 *
	 * T1 is removed, so this code is called and would reprogram
	 * the hardware to 5s from now. Any hrtimer_start after that
	 * will not reprogram the hardware due to hang_detected being
	 * set. So we'd effectivly block all timers until the T2 event
	 * fires.
	 */
	if (cpu_base->hang_detected)
		return;

/* IAMROOT-12:
 * -------------
 * 하드웨어 타이머를 프로그램한다.
 */
	if (cpu_base->expires_next.tv64 != KTIME_MAX)
		tick_program_event(cpu_base->expires_next, 1);
}

/*
 * Shared reprogramming for clock_realtime and clock_monotonic
 *
 * When a timer is enqueued and expires earlier than the already enqueued
 * timers, we have to check, whether it expires earlier than the timer for
 * which the clock event device was armed.
 *
 * Note, that in case the state has HRTIMER_STATE_CALLBACK set, no reprogramming
 * and no expiry check happens. The timer gets enqueued into the rbtree. The
 * reprogramming and expiry check is done in the hrtimer_interrupt or in the
 * softirq.
 *
 * Called with interrupts disabled and base->cpu_base.lock held
 */
static int hrtimer_reprogram(struct hrtimer *timer,
			     struct hrtimer_clock_base *base)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);

/* IAMROOT-12:
 * -------------
 * monotonic 기준의 만료 시각
 */
	ktime_t expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
	int res;

	WARN_ON_ONCE(hrtimer_get_expires_tv64(timer) < 0);

	/*
	 * When the callback is running, we do not reprogram the clock event
	 * device. The timer callback is either running on a different CPU or
	 * the callback is executed in the hrtimer_interrupt context. The
	 * reprogramming is handled either by the softirq, which called the
	 * callback or at the end of the hrtimer_interrupt.
	 */

/* IAMROOT-12:
 * -------------
 * 이미 hrtimer에 등록된 callback 함수가 실행되는 중이면 빠져나간다.
 */
	if (hrtimer_callback_running(timer))
		return 0;

	/*
	 * CLOCK_REALTIME timer might be requested with an absolute
	 * expiry time which is less than base->offset. Nothing wrong
	 * about that, just avoid to call into the tick code, which
	 * has now objections against negative expiry values.
	 */

/* IAMROOT-12:
 * -------------
 * monotonic의 만료 시각 가장작은 수가 0(부팅하자 마자 시각이 0)인데 
 * 음수로 나오는 경우는 오류!!!
 */
	if (expires.tv64 < 0)
		return -ETIME;

/* IAMROOT-12:
 * -------------
 * 만료 시각이 cpu에서 가장 이른 만료 시각보다 느리거나 같은 경우 
 * 별도로 hw 타이머를 조작할 필요 없다.
 */
	if (expires.tv64 >= cpu_base->expires_next.tv64)
		return 0;

	/*
	 * When the target cpu of the timer is currently executing
	 * hrtimer_interrupt(), then we do not touch the clock event
	 * device. hrtimer_interrupt() will reevaluate all clock bases
	 * before reprogramming the device.
	 */

/* IAMROOT-12:
 * -------------
 * hrtimer 인터럽트 처리중인 경우 이미 만료된 타이머를 처리하는 중이므로 
 * 함수를 빠져나간다.
 */
	if (cpu_base->in_hrtirq)
		return 0;

	/*
	 * If a hang was detected in the last timer interrupt then we
	 * do not schedule a timer which is earlier than the expiry
	 * which we enforced in the hang detection. We want the system
	 * to make progress.
	 */

/* IAMROOT-12:
 * -------------
 * 요청 cpu의 hrtimer의 인터럽트 처리에 문제가 있는 경우
 */
	if (cpu_base->hang_detected)
		return 0;

	/*
	 * Clockevents returns -ETIME, when the event was in the past.
	 */

/* IAMROOT-12:
 * -------------
 * high-res 타이머 장치에 oneshot 타이머 요청을 한다.
 * (만료 시간에 timer에 대한 인터럽트가 도착한다)
 */
	res = tick_program_event(expires, 0);
	if (!IS_ERR_VALUE(res))
		cpu_base->expires_next = expires;
	return res;
}

/*
 * Initialize the high resolution related parts of cpu_base
 */
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base)
{
/* IAMROOT-12:
 * -------------
 * 다음 만료 시각을 무한대로 지정한다. (다음 타이머가 없으므로)
 */
	base->expires_next.tv64 = KTIME_MAX;

/* IAMROOT-12:
 * -------------
 * hrtimer가 등록된 개수
 */
	base->hres_active = 0;
}

static inline ktime_t hrtimer_update_base(struct hrtimer_cpu_base *base)
{

/* IAMROOT-12:
 * -------------
 * monotonic을 제외한 나머지 클럭의 offset 주소를 인수로 담아간다.
 */
	ktime_t *offs_real = &base->clock_base[HRTIMER_BASE_REALTIME].offset;
	ktime_t *offs_boot = &base->clock_base[HRTIMER_BASE_BOOTTIME].offset;
	ktime_t *offs_tai = &base->clock_base[HRTIMER_BASE_TAI].offset;

	return ktime_get_update_offsets_now(offs_real, offs_boot, offs_tai);
}

/*
 * Retrigger next event is called after clock was set
 *
 * Called with interrupts disabled via on_each_cpu()
 */
static void retrigger_next_event(void *arg)
{
	struct hrtimer_cpu_base *base = this_cpu_ptr(&hrtimer_bases);

	if (!hrtimer_hres_active())
		return;

	raw_spin_lock(&base->lock);
	hrtimer_update_base(base);
	hrtimer_force_reprogram(base, 0);
	raw_spin_unlock(&base->lock);
}

/*
 * Switch to high resolution mode
 */
static int hrtimer_switch_to_hres(void)
{
	int i, cpu = smp_processor_id();
	struct hrtimer_cpu_base *base = &per_cpu(hrtimer_bases, cpu);
	unsigned long flags;

	if (base->hres_active)
		return 1;

	local_irq_save(flags);

	if (tick_init_highres()) {
		local_irq_restore(flags);
		printk(KERN_WARNING "Could not switch to high resolution "
				    "mode on CPU %d\n", cpu);
		return 0;
	}
	base->hres_active = 1;
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++)
		base->clock_base[i].resolution = KTIME_HIGH_RES;

	tick_setup_sched_timer();
	/* "Retrigger" the interrupt to get things going */
	retrigger_next_event(NULL);
	local_irq_restore(flags);
	return 1;
}

static void clock_was_set_work(struct work_struct *work)
{
	clock_was_set();
}

static DECLARE_WORK(hrtimer_work, clock_was_set_work);

/*
 * Called from timekeeping and resume code to reprogramm the hrtimer
 * interrupt device on all cpus.
 */
void clock_was_set_delayed(void)
{
	schedule_work(&hrtimer_work);
}

#else

static inline int hrtimer_hres_active(void) { return 0; }
static inline int hrtimer_is_hres_enabled(void) { return 0; }
static inline int hrtimer_switch_to_hres(void) { return 0; }
static inline void
hrtimer_force_reprogram(struct hrtimer_cpu_base *base, int skip_equal) { }
static inline int hrtimer_reprogram(struct hrtimer *timer,
				    struct hrtimer_clock_base *base)
{
	return 0;
}
static inline void hrtimer_init_hres(struct hrtimer_cpu_base *base) { }
static inline void retrigger_next_event(void *arg) { }

#endif /* CONFIG_HIGH_RES_TIMERS */

/*
 * Clock realtime was set
 *
 * Change the offset of the realtime clock vs. the monotonic
 * clock.
 *
 * We might have to reprogram the high resolution timer interrupt. On
 * SMP we call the architecture specific code to retrigger _all_ high
 * resolution timer interrupts. On UP we just disable interrupts and
 * call the high resolution interrupt code.
 */
void clock_was_set(void)
{
#ifdef CONFIG_HIGH_RES_TIMERS
	/* Retrigger the CPU local events everywhere */
	on_each_cpu(retrigger_next_event, NULL, 1);
#endif
	timerfd_clock_was_set();
}

/*
 * During resume we might have to reprogram the high resolution timer
 * interrupt on all online CPUs.  However, all other CPUs will be
 * stopped with IRQs interrupts disabled so the clock_was_set() call
 * must be deferred.
 */
void hrtimers_resume(void)
{
	WARN_ONCE(!irqs_disabled(),
		  KERN_INFO "hrtimers_resume() called with IRQs enabled!");

	/* Retrigger on the local CPU */
	retrigger_next_event(NULL);
	/* And schedule a retrigger for all others */
	clock_was_set_delayed();
}

static inline void timer_stats_hrtimer_set_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (timer->start_site)
		return;
	timer->start_site = __builtin_return_address(0);
	memcpy(timer->start_comm, current->comm, TASK_COMM_LEN);
	timer->start_pid = current->pid;
#endif
}

static inline void timer_stats_hrtimer_clear_start_info(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
#endif
}

static inline void timer_stats_account_hrtimer(struct hrtimer *timer)
{
#ifdef CONFIG_TIMER_STATS
	if (likely(!timer_stats_active))
		return;
	timer_stats_update_stats(timer, timer->start_pid, timer->start_site,
				 timer->function, timer->start_comm, 0);
#endif
}

/*
 * Counterpart to lock_hrtimer_base above:
 */
static inline
void unlock_hrtimer_base(const struct hrtimer *timer, unsigned long *flags)
{
	raw_spin_unlock_irqrestore(&timer->base->cpu_base->lock, *flags);
}

/**
 * hrtimer_forward - forward the timer expiry
 * @timer:	hrtimer to forward
 * @now:	forward past this time
 * @interval:	the interval to forward
 *
 * Forward the timer expiry so it will expire in the future.
 * Returns the number of overruns.
 */
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta;

	delta = ktime_sub(now, hrtimer_get_expires(timer));

	if (delta.tv64 < 0)
		return 0;

	if (interval.tv64 < timer->base->resolution.tv64)
		interval.tv64 = timer->base->resolution.tv64;

	if (unlikely(delta.tv64 >= interval.tv64)) {
		s64 incr = ktime_to_ns(interval);

		orun = ktime_divns(delta, incr);
		hrtimer_add_expires_ns(timer, incr * orun);
		if (hrtimer_get_expires_tv64(timer) > now.tv64)
			return orun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		orun++;
	}
	hrtimer_add_expires(timer, interval);

	return orun;
}
EXPORT_SYMBOL_GPL(hrtimer_forward);

/*
 * enqueue_hrtimer - internal function to (re)start a timer
 *
 * The timer is inserted in expiry order. Insertion into the
 * red black tree is O(log(n)). Must hold the base lock.
 *
 * Returns 1 when the new timer is the leftmost timer in the tree.
 */
static int enqueue_hrtimer(struct hrtimer *timer,
			   struct hrtimer_clock_base *base)
{
	debug_activate(timer);

/* IAMROOT-12:
 * -------------
 * RB 트리에 hrtimer를 추가한다.
 */
	timerqueue_add(&base->active, &timer->node);

/* IAMROOT-12:
 * -------------
 * 요청한 hrtimer의 클럭 종류에 따른 bit를 설정하여 해당 클럭에 hrtimer가 
 * 등록되었음을 나타낸다.
 */
	base->cpu_base->active_bases |= 1 << base->index;

	/*
	 * HRTIMER_STATE_ENQUEUED is or'ed to the current state to preserve the
	 * state of a possibly running callback.
	 */

/* IAMROOT-12:
 * -------------
 * 타이머가 hrtimer 큐에 등록되었음을 나타낸다.
 */
	timer->state |= HRTIMER_STATE_ENQUEUED;


/* IAMROOT-12:
 * -------------
 * 등록 시킨 hrtimer가 요청 클럭에서 가장 좌측(만료시각이 가장 빠른)인 경우 
 * true를 반환한다.
 */
	return (&timer->node == base->active.next);
}

/*
 * __remove_hrtimer - internal function to remove a timer
 *
 * Caller must hold the base lock.
 *
 * High resolution timer mode reprograms the clock event device when the
 * timer is the one which expires next. The caller can disable this by setting
 * reprogram to zero. This is useful, when the context does a reprogramming
 * anyway (e.g. timer interrupt)
 */
static void __remove_hrtimer(struct hrtimer *timer,
			     struct hrtimer_clock_base *base,
			     unsigned long newstate, int reprogram)
{
	struct timerqueue_node *next_timer;

/* IAMROOT-12:
 * -------------
 * 엔큐 플래그가 없으면 이미 RB 트리에 없는 상태이므로 빠져나간다.
 */
	if (!(timer->state & HRTIMER_STATE_ENQUEUED))
		goto out;

	next_timer = timerqueue_getnext(&base->active);

/* IAMROOT-12:
 * -------------
 * 해당 클럭의 RB 트리에서 타이머를 제거하고 해당 클럭 내에서 가장 이른 
 * 타이머도 업데이트한다.
 */
	timerqueue_del(&base->active, &timer->node);

/* IAMROOT-12:
 * -------------
 * 제거한 타이머가 가장 이른 만료 시각인 경우
 */
	if (&timer->node == next_timer) {
#ifdef CONFIG_HIGH_RES_TIMERS
		/* Reprogram the clock event device. if enabled */
		if (reprogram && hrtimer_hres_active()) {
			ktime_t expires;

			expires = ktime_sub(hrtimer_get_expires(timer),
					    base->offset);

/* IAMROOT-12:
 * -------------
 * reprogram 요청이 있는 경우 삭제한 타이머가 cpu 내에서(4개의 클럭) 가장
 *  이른 만료 시각인 경우 그 다음 순서의 타이머를 프로그램한다.
 */
			if (base->cpu_base->expires_next.tv64 == expires.tv64)
				hrtimer_force_reprogram(base->cpu_base, 1);
		}
#endif
	}

/* IAMROOT-12:
 * -------------
 * RB 트리에 타이머가 없으면 4가지 클럭 중 해당 클럭의 비트를 클리어한다.
 */
	if (!timerqueue_getnext(&base->active))
		base->cpu_base->active_bases &= ~(1 << base->index);
out:

/* IAMROOT-12:
 * -------------
 * 종료 전에 인수로 받은 state를 설정한다.
 */
	timer->state = newstate;
}

/*
 * remove hrtimer, called with base lock held
 */
static inline int
remove_hrtimer(struct hrtimer *timer, struct hrtimer_clock_base *base)
{
	if (hrtimer_is_queued(timer)) {
		unsigned long state;
		int reprogram;

		/*
		 * Remove the timer and force reprogramming when high
		 * resolution mode is active and the timer is on the current
		 * CPU. If we remove a timer on another CPU, reprogramming is
		 * skipped. The interrupt event on this CPU is fired and
		 * reprogramming happens in the interrupt handler. This is a
		 * rare case and less expensive than a smp call.
		 */
		debug_deactivate(timer);
		timer_stats_hrtimer_clear_start_info(timer);
		reprogram = base->cpu_base == this_cpu_ptr(&hrtimer_bases);
		/*
		 * We must preserve the CALLBACK state flag here,
		 * otherwise we could move the timer base in
		 * switch_hrtimer_base.
		 */
		state = timer->state & HRTIMER_STATE_CALLBACK;
		__remove_hrtimer(timer, base, state, reprogram);
		return 1;
	}
	return 0;
}

int __hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode,
		int wakeup)
{
	struct hrtimer_clock_base *base, *new_base;
	unsigned long flags;
	int ret, leftmost;

	base = lock_hrtimer_base(timer, &flags);

	/* Remove an active timer from the queue: */
	ret = remove_hrtimer(timer, base);

	if (mode & HRTIMER_MODE_REL) {

/* IAMROOT-12:
 * -------------
 * 상대 모드인 경우 요청한 현재 클럭시간에 time을 더한다.
 */
		tim = ktime_add_safe(tim, base->get_time());
		/*
		 * CONFIG_TIME_LOW_RES is a temporary way for architectures
		 * to signal that they simply return xtime in
		 * do_gettimeoffset(). In this case we want to round up by
		 * resolution when starting a relative timer, to avoid short
		 * timeouts. This will go away with the GTOD framework.
		 */
#ifdef CONFIG_TIME_LOW_RES
		tim = ktime_add_safe(tim, base->resolution);
#endif
	}

/* IAMROOT-12:
 * -------------
 * hrtimer의 RB 트리 유지는 항상 expires로 절대시각으로 관리된다.
 */
	hrtimer_set_expires_range_ns(timer, tim, delta_ns);

	/* Switch the timer base, if necessary: */

/* IAMROOT-12:
 * -------------
 * 여러 조건으로 판단하여 new_base를 결정한다. (cpu가 바뀔 수 있다)
 */
	new_base = switch_hrtimer_base(timer, base, mode & HRTIMER_MODE_PINNED);

	timer_stats_hrtimer_set_start_info(timer);

/* IAMROOT-12:
 * -------------
 * 요청한 클럭 base에 hrtimer를 추가한다. 그 중 가장 빠른 것인 경우 1을 반환
 */
	leftmost = enqueue_hrtimer(timer, new_base);

	if (!leftmost) {
		unlock_hrtimer_base(timer, &flags);
		return ret;
	}

	if (!hrtimer_is_hres_active(timer)) {
		/*
		 * Kick to reschedule the next tick to handle the new timer
		 * on dynticks target.
		 */

/* IAMROOT-12:
 * -------------
 * nohz idle 상태의 cpu를 깨운다.
 */
		wake_up_nohz_cpu(new_base->cpu_base->cpu);
	} else if (new_base->cpu_base == this_cpu_ptr(&hrtimer_bases) &&
			hrtimer_reprogram(timer, new_base)) {

/* IAMROOT-12:
 * -------------
 * 변경되었든 안되었든 new_base의 cpu가 현재 cpu인 경우 hrtimer에 대한 
 * 프로그램 요청을 시도한다.
 */
		/*
		 * Only allow reprogramming if the new base is on this CPU.
		 * (it might still be on another CPU if the timer was pending)
		 *
		 * XXX send_remote_softirq() ?
		 */

/* IAMROOT-12:
 * -------------
 * 이미 지나간걸로 추정하고 직접 hrtimer에 대한 softirq를 호출한다.
 *
 * softirqd를 wakeup=1일 때 시킨다.
 */
		if (wakeup) {
			/*
			 * We need to drop cpu_base->lock to avoid a
			 * lock ordering issue vs. rq->lock.
			 */
			raw_spin_unlock(&new_base->cpu_base->lock);
			raise_softirq_irqoff(HRTIMER_SOFTIRQ);
			local_irq_restore(flags);
			return ret;
		} else {
			__raise_softirq_irqoff(HRTIMER_SOFTIRQ);
		}
	}

	unlock_hrtimer_base(timer, &flags);

	return ret;
}
EXPORT_SYMBOL_GPL(__hrtimer_start_range_ns);

/**
 * hrtimer_start_range_ns - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @delta_ns:	"slack" range for the timer
 * @mode:	expiry mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		unsigned long delta_ns, const enum hrtimer_mode mode)
{
	return __hrtimer_start_range_ns(timer, tim, delta_ns, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start_range_ns);

/**
 * hrtimer_start - (re)start an hrtimer on the current CPU
 * @timer:	the timer to be added
 * @tim:	expiry time
 * @mode:	expiry mode: absolute (HRTIMER_MODE_ABS) or
 *		relative (HRTIMER_MODE_REL)
 *
 * Returns:
 *  0 on success
 *  1 when the timer was active
 */
int
hrtimer_start(struct hrtimer *timer, ktime_t tim, const enum hrtimer_mode mode)
{

/* IAMROOT-12:
 * -------------
 * hrtimer를 큐에 등록하고 시작
 */

	return __hrtimer_start_range_ns(timer, tim, 0, mode, 1);
}
EXPORT_SYMBOL_GPL(hrtimer_start);


/**
 * hrtimer_try_to_cancel - try to deactivate a timer
 * @timer:	hrtimer to stop
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 * -1 when the timer is currently excuting the callback function and
 *    cannot be stopped
 */
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	int ret = -1;

	base = lock_hrtimer_base(timer, &flags);

	if (!hrtimer_callback_running(timer))
		ret = remove_hrtimer(timer, base);

	unlock_hrtimer_base(timer, &flags);

	return ret;

}
EXPORT_SYMBOL_GPL(hrtimer_try_to_cancel);

/**
 * hrtimer_cancel - cancel a timer and wait for the handler to finish.
 * @timer:	the timer to be cancelled
 *
 * Returns:
 *  0 when the timer was not active
 *  1 when the timer was active
 */
int hrtimer_cancel(struct hrtimer *timer)
{
	for (;;) {
		int ret = hrtimer_try_to_cancel(timer);

		if (ret >= 0)
			return ret;
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(hrtimer_cancel);

/**
 * hrtimer_get_remaining - get remaining time for the timer
 * @timer:	the timer to read
 */
ktime_t hrtimer_get_remaining(const struct hrtimer *timer)
{
	unsigned long flags;
	ktime_t rem;

	lock_hrtimer_base(timer, &flags);
	rem = hrtimer_expires_remaining(timer);
	unlock_hrtimer_base(timer, &flags);

	return rem;
}
EXPORT_SYMBOL_GPL(hrtimer_get_remaining);

#ifdef CONFIG_NO_HZ_COMMON
/**
 * hrtimer_get_next_event - get the time until next expiry event
 *
 * Returns the delta to the next expiry event or KTIME_MAX if no timer
 * is pending.
 */
ktime_t hrtimer_get_next_event(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t mindelta = { .tv64 = KTIME_MAX };
	unsigned long flags;

	raw_spin_lock_irqsave(&cpu_base->lock, flags);

	if (!hrtimer_hres_active())
		mindelta = ktime_sub(__hrtimer_get_next_event(cpu_base),
				     ktime_get());

	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);

	if (mindelta.tv64 < 0)
		mindelta.tv64 = 0;
	return mindelta;
}
#endif

static void __hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
			   enum hrtimer_mode mode)
{
	struct hrtimer_cpu_base *cpu_base;
	int base;

	memset(timer, 0, sizeof(struct hrtimer));

	cpu_base = raw_cpu_ptr(&hrtimer_bases);

/* IAMROOT-12:
 * -------------
 * realtime 클럭이면서 상대 시각으로 타이머를 요청한 경우 monotic을 
 * 사용한 방법과 동일하므로 monotic 클럭으로 처리하게 변경한다.
 */
	if (clock_id == CLOCK_REALTIME && mode != HRTIMER_MODE_ABS)
		clock_id = CLOCK_MONOTONIC;

/* IAMROOT-12:
 * -------------
 * hrtimer가 사용하지 않는 clockid를 요청하는 경우 배열에 지정된 것이 없어서 
 * 그냥 0번으로 계산되어 monotonic 클럭을 사용한다.
 */
	base = hrtimer_clockid_to_base(clock_id);

/* IAMROOT-12:
 * -------------
 * 사용하는 클럭에 대한 타이머의 base를 지정한다.
 */
	timer->base = &cpu_base->clock_base[base];
	timerqueue_init(&timer->node);

#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
	timer->start_pid = -1;
	memset(timer->start_comm, 0, TASK_COMM_LEN);
#endif
}

/**
 * hrtimer_init - initialize a timer to the given clock
 * @timer:	the timer to be initialized
 * @clock_id:	the clock to be used
 * @mode:	timer mode abs/rel
 */
void hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
		  enum hrtimer_mode mode)
{
	debug_init(timer, clock_id, mode);
	__hrtimer_init(timer, clock_id, mode);
}
EXPORT_SYMBOL_GPL(hrtimer_init);

/**
 * hrtimer_get_res - get the timer resolution for a clock
 * @which_clock: which clock to query
 * @tp:		 pointer to timespec variable to store the resolution
 *
 * Store the resolution of the clock selected by @which_clock in the
 * variable pointed to by @tp.
 */
int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp)
{
	struct hrtimer_cpu_base *cpu_base;
	int base = hrtimer_clockid_to_base(which_clock);

	cpu_base = raw_cpu_ptr(&hrtimer_bases);
	*tp = ktime_to_timespec(cpu_base->clock_base[base].resolution);

	return 0;
}
EXPORT_SYMBOL_GPL(hrtimer_get_res);

static void __run_hrtimer(struct hrtimer *timer, ktime_t *now)
{
	struct hrtimer_clock_base *base = timer->base;
	struct hrtimer_cpu_base *cpu_base = base->cpu_base;
	enum hrtimer_restart (*fn)(struct hrtimer *);
	int restart;

	WARN_ON(!irqs_disabled());

	debug_deactivate(timer);

/* IAMROOT-12:
 * -------------
 * 해당 클럭의 RB 트리에서 타이머를 제거한다.
 * (인수를 0을 건네 차 순위 타이머는 프로그램하지 않는다.)
 */
	__remove_hrtimer(timer, base, HRTIMER_STATE_CALLBACK, 0);
	timer_stats_account_hrtimer(timer);

	fn = timer->function;

	/*
	 * Because we run timers from hardirq context, there is no chance
	 * they get migrated to another cpu, therefore its safe to unlock
	 * the timer base.
	 */
	raw_spin_unlock(&cpu_base->lock);
	trace_hrtimer_expire_entry(timer, now);

	/* IAMROOT-12:
	 * -------------
	 * 타이머 콜백 함수 호출하고 결과가 HRTIMER_RESTART(1)를 반환하는 
	 * 경우 다시 똑같이 재프로그램한다.
	 */
	restart = fn(timer);
	trace_hrtimer_expire_exit(timer);
	raw_spin_lock(&cpu_base->lock);

	/*
	 * Note: We clear the CALLBACK bit after enqueue_hrtimer and
	 * we do not reprogramm the event hardware. Happens either in
	 * hrtimer_start_range_ns() or in hrtimer_interrupt()
	 */
	if (restart != HRTIMER_NORESTART) {
		BUG_ON(timer->state != HRTIMER_STATE_CALLBACK);
		enqueue_hrtimer(timer, base);
	}

	WARN_ON_ONCE(!(timer->state & HRTIMER_STATE_CALLBACK));

/* IAMROOT-12:
 * -------------
 * 타이머 처리가 완료되면 콜백 함수 처리중 플래그를 제거한다.
 */
	timer->state &= ~HRTIMER_STATE_CALLBACK;
}

#ifdef CONFIG_HIGH_RES_TIMERS

/*
 * High resolution timer interrupt
 * Called with interrupts disabled
 */

/* IAMROOT-12:
 * -------------
 * hrtimer의 softirq 처리 함수 순서
 *
 * run_hrtimer_softirq() -> 
 *	hrtimer_pick_ahead_timers() ->
 *		__hrtimer_pick_ahead_timers() ->
 *			hrtimer_interrupt()
 */
void hrtimer_interrupt(struct clock_event_device *dev)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires_next, now, entry_time, delta;
	int i, retries = 0;

	BUG_ON(!cpu_base->hres_active);

/* IAMROOT-12:
 * -------------
 * 해당 cpu에 hrtimer 인터럽트가 들어온 횟수
 */
	cpu_base->nr_events++;
	dev->next_event.tv64 = KTIME_MAX;

	raw_spin_lock(&cpu_base->lock);

/* IAMROOT-12:
 * -------------
 * cpu_base의 3가지 offset를 갱신하고 tk의 monotonic 시각을 알아온다.
 */
	entry_time = now = hrtimer_update_base(cpu_base);
retry:

/* IAMROOT-12:
 * -------------
 * hrtimer 처리 시작
 */
	cpu_base->in_hrtirq = 1;
	/*
	 * We set expires_next to KTIME_MAX here with cpu_base->lock
	 * held to prevent that a timer is enqueued in our queue via
	 * the migration code. This does not affect enqueueing of
	 * timers which run their callback and need to be requeued on
	 * this CPU.
	 */
	cpu_base->expires_next.tv64 = KTIME_MAX;

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		struct hrtimer_clock_base *base;
		struct timerqueue_node *node;
		ktime_t basenow;

/* IAMROOT-12:
 * -------------
 * hrtimer가 등록되지 않은 클럭은 skip 한다.
 */
		if (!(cpu_base->active_bases & (1 << i)))
			continue;

/* IAMROOT-12:
 * -------------
 * basenow: 해당 클럭의 현재 시각
 */
		base = cpu_base->clock_base + i;
		basenow = ktime_add(now, base->offset);

		while ((node = timerqueue_getnext(&base->active))) {
			struct hrtimer *timer;

			timer = container_of(node, struct hrtimer, node);

			/*
			 * The immediate goal for using the softexpires is
			 * minimizing wakeups, not running timers at the
			 * earliest interrupt after their soft expiration.
			 * This allows us to avoid using a Priority Search
			 * Tree, which can answer a stabbing querry for
			 * overlapping intervals and instead use the simple
			 * BST we already have.
			 * We don't add extra wakeups by delaying timers that
			 * are right-of a not yet expired timer, because that
			 * timer will have to trigger a wakeup anyway.
			 */

/* IAMROOT-12:
 * -------------
 * 참고: Greedy hrtimer expiration - LWN.net
 *       https://lwn.net/Articles/461592/
 *
 * 1 개의 hrtimer 처리 시 이미 지나간 soft expire hrtimer까지 한꺼번에 
 * 처리하므로 결과적으로 처리할 타이머 프로그램 횟수를 줄인다.
 */
			if (basenow.tv64 < hrtimer_get_softexpires_tv64(timer))
				break;

/* IAMROOT-12:
 * -------------
 * RB 트리에서 타이머를 제거하고 그 타이머에 등록된 콜백 함수를 수행한다.
 * 수행 결과가 HRTIMER_RESTART(1) 경우에 한해서 다시 RB 트리에 엔큐한다.
 */
			__run_hrtimer(timer, &basenow);
		}
	}
	/* Reevaluate the clock bases for the next expiry */

/* IAMROOT-12:
 * -------------
 * 요청한 cpu_base에서 4개의 클럭을 검색하여 가장 이른 만료 시각을 알아온다.
 */
	expires_next = __hrtimer_get_next_event(cpu_base);
	/*
	 * Store the new expiry value so the migration code can verify
	 * against it.
	 */
	cpu_base->expires_next = expires_next;
	cpu_base->in_hrtirq = 0;
	raw_spin_unlock(&cpu_base->lock);

	/* Reprogramming necessary ? */

/* IAMROOT-12:
 * -------------
 * cpu에서(4개의 클럭 포함) 가장 이른 expires 타이머를 프로그램한다.
 * 
 * 프로그램할 타이머가 없거나 다음 타이머 프로그램을 성공한 경우 
 * 함수를 빠져나간다.
 */
	if (expires_next.tv64 == KTIME_MAX ||
	    !tick_program_event(expires_next, 0)) {
		cpu_base->hang_detected = 0;
		return;
	}

	/*
	 * The next timer was already expired due to:
	 * - tracing
	 * - long lasting callbacks
	 * - being scheduled away when running in a VM
	 *
	 * We need to prevent that we loop forever in the hrtimer
	 * interrupt routine. We give it 3 attempts to avoid
	 * overreacting on some spurious event.
	 *
	 * Acquire base lock for updating the offsets and retrieving
	 * the current time.
	 */

/* IAMROOT-12:
 * -------------
 * 여러 이유로 타이머 프로그램이 실패한 경우 3회에 한해 다시 타이머를 
 * 뒤져 expire 된 타이머들을 수행한다.
 */
	raw_spin_lock(&cpu_base->lock);
	now = hrtimer_update_base(cpu_base);
	cpu_base->nr_retries++;
	if (++retries < 3)
		goto retry;
	/*
	 * Give the system a chance to do something else than looping
	 * here. We stored the entry time, so we know exactly how long
	 * we spent here. We schedule the next event this amount of
	 * time away.
	 */

/* IAMROOT-12:
 * -------------
 * 여기에 도착했다는 것은 타이머 프로그램이 3회 이상 실패하는 경우이다.
 * 이 때에는 hang 횟수를 증가시키고, 관련된 stat을 갱신한다.
 */
	cpu_base->nr_hangs++;
	cpu_base->hang_detected = 1;
	raw_spin_unlock(&cpu_base->lock);
	delta = ktime_sub(now, entry_time);
	if (delta.tv64 > cpu_base->max_hang_time.tv64)
		cpu_base->max_hang_time = delta;
	/*
	 * Limit it to a sensible value as we enforce a longer
	 * delay. Give the CPU at least 100ms to catch up.
	 */

/* IAMROOT-12:
 * -------------
 * 100ms의 여유 있는 만료 시각으로 프로그램해본다.
 */
	if (delta.tv64 > 100 * NSEC_PER_MSEC)
		expires_next = ktime_add_ns(now, 100 * NSEC_PER_MSEC);
	else
		expires_next = ktime_add(now, delta);
	tick_program_event(expires_next, 1);
	printk_once(KERN_WARNING "hrtimer: interrupt took %llu ns\n",
		    ktime_to_ns(delta));
}

/*
 * local version of hrtimer_peek_ahead_timers() called with interrupts
 * disabled.
 */
static void __hrtimer_peek_ahead_timers(void)
{
	struct tick_device *td;

/* IAMROOT-12:
 * -------------
 * hrtimer 인터럽트가 들어왔는데 실제 high-res 모드가 준비되지 않은 경우 return
 */
	if (!hrtimer_hres_active())
		return;

	td = this_cpu_ptr(&tick_cpu_device);
	if (td && td->evtdev)
		hrtimer_interrupt(td->evtdev);
}

/**
 * hrtimer_peek_ahead_timers -- run soft-expired timers now
 *
 * hrtimer_peek_ahead_timers will peek at the timer queue of
 * the current cpu and check if there are any timers for which
 * the soft expires time has passed. If any such timers exist,
 * they are run immediately and then removed from the timer queue.
 *
 */
void hrtimer_peek_ahead_timers(void)
{
	unsigned long flags;

	local_irq_save(flags);
	__hrtimer_peek_ahead_timers();
	local_irq_restore(flags);
}


/* IAMROOT-12:
 * -------------
 * hrtimer의 softirq 핸들러 루틴
 */
static void run_hrtimer_softirq(struct softirq_action *h)
{
	hrtimer_peek_ahead_timers();
}

#else /* CONFIG_HIGH_RES_TIMERS */

static inline void __hrtimer_peek_ahead_timers(void) { }

#endif	/* !CONFIG_HIGH_RES_TIMERS */

/*
 * Called from timer softirq every jiffy, expire hrtimers:
 *
 * For HRT its the fall back code to run the softirq in the timer
 * softirq context in case the hrtimer initialization failed or has
 * not been done yet.
 */
void hrtimer_run_pending(void)
{
	if (hrtimer_hres_active())
		return;

	/*
	 * This _is_ ugly: We have to check in the softirq context,
	 * whether we can switch to highres and / or nohz mode. The
	 * clocksource switch happens in the timer interrupt with
	 * xtime_lock held. Notification from there only sets the
	 * check bit in the tick_oneshot code, otherwise we might
	 * deadlock vs. xtime_lock.
	 */
	if (tick_check_oneshot_change(!hrtimer_is_hres_enabled()))
		hrtimer_switch_to_hres();
}

/*
 * Called from hardirq context every jiffy
 */
void hrtimer_run_queues(void)
{
	struct timerqueue_node *node;
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	struct hrtimer_clock_base *base;
	int index, gettime = 1;

	if (hrtimer_hres_active())
		return;

	for (index = 0; index < HRTIMER_MAX_CLOCK_BASES; index++) {
		base = &cpu_base->clock_base[index];
		if (!timerqueue_getnext(&base->active))
			continue;

		if (gettime) {
			hrtimer_get_softirq_time(cpu_base);
			gettime = 0;
		}

		raw_spin_lock(&cpu_base->lock);

		while ((node = timerqueue_getnext(&base->active))) {
			struct hrtimer *timer;

			timer = container_of(node, struct hrtimer, node);
			if (base->softirq_time.tv64 <=
					hrtimer_get_expires_tv64(timer))
				break;

			__run_hrtimer(timer, &base->softirq_time);
		}
		raw_spin_unlock(&cpu_base->lock);
	}
}

/*
 * Sleep related functions:
 */
static enum hrtimer_restart hrtimer_wakeup(struct hrtimer *timer)
{
	struct hrtimer_sleeper *t =
		container_of(timer, struct hrtimer_sleeper, timer);
	struct task_struct *task = t->task;

	t->task = NULL;
	if (task)
		wake_up_process(task);

	return HRTIMER_NORESTART;
}

void hrtimer_init_sleeper(struct hrtimer_sleeper *sl, struct task_struct *task)
{
	sl->timer.function = hrtimer_wakeup;
	sl->task = task;
}
EXPORT_SYMBOL_GPL(hrtimer_init_sleeper);

static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
	hrtimer_init_sleeper(t, current);

	do {
		set_current_state(TASK_INTERRUPTIBLE);
		hrtimer_start_expires(&t->timer, mode);
		if (!hrtimer_active(&t->timer))
			t->task = NULL;

		if (likely(t->task))
			freezable_schedule();

		hrtimer_cancel(&t->timer);
		mode = HRTIMER_MODE_ABS;

	} while (t->task && !signal_pending(current));

	__set_current_state(TASK_RUNNING);

	return t->task == NULL;
}

static int update_rmtp(struct hrtimer *timer, struct timespec __user *rmtp)
{
	struct timespec rmt;
	ktime_t rem;

	rem = hrtimer_expires_remaining(timer);
	if (rem.tv64 <= 0)
		return 0;
	rmt = ktime_to_timespec(rem);

	if (copy_to_user(rmtp, &rmt, sizeof(*rmtp)))
		return -EFAULT;

	return 1;
}

long __sched hrtimer_nanosleep_restart(struct restart_block *restart)
{
	struct hrtimer_sleeper t;
	struct timespec __user  *rmtp;
	int ret = 0;

	hrtimer_init_on_stack(&t.timer, restart->nanosleep.clockid,
				HRTIMER_MODE_ABS);
	hrtimer_set_expires_tv64(&t.timer, restart->nanosleep.expires);

	if (do_nanosleep(&t, HRTIMER_MODE_ABS))
		goto out;

	rmtp = restart->nanosleep.rmtp;
	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	/* The other values in restart are already filled in */
	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

long hrtimer_nanosleep(struct timespec *rqtp, struct timespec __user *rmtp,
		       const enum hrtimer_mode mode, const clockid_t clockid)
{
	struct restart_block *restart;
	struct hrtimer_sleeper t;
	int ret = 0;
	unsigned long slack;

	slack = current->timer_slack_ns;
	if (dl_task(current) || rt_task(current))
		slack = 0;

	hrtimer_init_on_stack(&t.timer, clockid, mode);
	hrtimer_set_expires_range_ns(&t.timer, timespec_to_ktime(*rqtp), slack);
	if (do_nanosleep(&t, mode))
		goto out;

	/* Absolute timers do not update the rmtp value and restart: */
	if (mode == HRTIMER_MODE_ABS) {
		ret = -ERESTARTNOHAND;
		goto out;
	}

	if (rmtp) {
		ret = update_rmtp(&t.timer, rmtp);
		if (ret <= 0)
			goto out;
	}

	restart = &current->restart_block;
	restart->fn = hrtimer_nanosleep_restart;
	restart->nanosleep.clockid = t.timer.base->clockid;
	restart->nanosleep.rmtp = rmtp;
	restart->nanosleep.expires = hrtimer_get_expires_tv64(&t.timer);

	ret = -ERESTART_RESTARTBLOCK;
out:
	destroy_hrtimer_on_stack(&t.timer);
	return ret;
}

SYSCALL_DEFINE2(nanosleep, struct timespec __user *, rqtp,
		struct timespec __user *, rmtp)
{
	struct timespec tu;

	if (copy_from_user(&tu, rqtp, sizeof(tu)))
		return -EFAULT;

	if (!timespec_valid(&tu))
		return -EINVAL;

	return hrtimer_nanosleep(&tu, rmtp, HRTIMER_MODE_REL, CLOCK_MONOTONIC);
}

/*
 * Functions related to boot-time initialization:
 */
static void init_hrtimers_cpu(int cpu)
{
	struct hrtimer_cpu_base *cpu_base = &per_cpu(hrtimer_bases, cpu);
	int i;

/* IAMROOT-12:
 * -------------
 * hrtimer가 사용하는 4개의 클럭만큼 초기화한다.
 */
	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		cpu_base->clock_base[i].cpu_base = cpu_base;
		timerqueue_init_head(&cpu_base->clock_base[i].active);
	}

	cpu_base->cpu = cpu;
	hrtimer_init_hres(cpu_base);
}

#ifdef CONFIG_HOTPLUG_CPU

static void migrate_hrtimer_list(struct hrtimer_clock_base *old_base,
				struct hrtimer_clock_base *new_base)
{
	struct hrtimer *timer;
	struct timerqueue_node *node;

	while ((node = timerqueue_getnext(&old_base->active))) {
		timer = container_of(node, struct hrtimer, node);
		BUG_ON(hrtimer_callback_running(timer));
		debug_deactivate(timer);

		/*
		 * Mark it as STATE_MIGRATE not INACTIVE otherwise the
		 * timer could be seen as !active and just vanish away
		 * under us on another CPU
		 */
		__remove_hrtimer(timer, old_base, HRTIMER_STATE_MIGRATE, 0);
		timer->base = new_base;
		/*
		 * Enqueue the timers on the new cpu. This does not
		 * reprogram the event device in case the timer
		 * expires before the earliest on this CPU, but we run
		 * hrtimer_interrupt after we migrated everything to
		 * sort out already expired timers and reprogram the
		 * event device.
		 */
		enqueue_hrtimer(timer, new_base);

		/* Clear the migration state bit */
		timer->state &= ~HRTIMER_STATE_MIGRATE;
	}
}

static void migrate_hrtimers(int scpu)
{
	struct hrtimer_cpu_base *old_base, *new_base;
	int i;

	BUG_ON(cpu_online(scpu));
	tick_cancel_sched_timer(scpu);

	local_irq_disable();
	old_base = &per_cpu(hrtimer_bases, scpu);
	new_base = this_cpu_ptr(&hrtimer_bases);
	/*
	 * The caller is globally serialized and nobody else
	 * takes two locks at once, deadlock is not possible.
	 */
	raw_spin_lock(&new_base->lock);
	raw_spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

	for (i = 0; i < HRTIMER_MAX_CLOCK_BASES; i++) {
		migrate_hrtimer_list(&old_base->clock_base[i],
				     &new_base->clock_base[i]);
	}

	raw_spin_unlock(&old_base->lock);
	raw_spin_unlock(&new_base->lock);

	/* Check, if we got expired work to do */
	__hrtimer_peek_ahead_timers();
	local_irq_enable();
}

#endif /* CONFIG_HOTPLUG_CPU */

static int hrtimer_cpu_notify(struct notifier_block *self,
					unsigned long action, void *hcpu)
{
	int scpu = (long)hcpu;

	switch (action) {

	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:

/* IAMROOT-12:
 * -------------
 * 요청 cpu에 대한 hrtimer 초기화
 */
		init_hrtimers_cpu(scpu);
		break;

#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DYING, &scpu);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	{

/* IAMROOT-12:
 * -------------
 * hot-plug가 지원되는 시스템에서 cpu off되는 경우 기존 cpu의 hrtimer를 
 * 다른 cpu의 hrtimer 관리체제로 옮긴다.
 */
		clockevents_notify(CLOCK_EVT_NOTIFY_CPU_DEAD, &scpu);
		migrate_hrtimers(scpu);
		break;
	}
#endif

	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block hrtimers_nb = {
	.notifier_call = hrtimer_cpu_notify,
};

void __init hrtimers_init(void)
{

/* IAMROOT-12:
 * -------------
 * boot-up cpu에 대한 hrtimer를 초기화한다.
 */
	hrtimer_cpu_notify(&hrtimers_nb, (unsigned long)CPU_UP_PREPARE,
			  (void *)(long)smp_processor_id());

/* IAMROOT-12:
 * -------------
 * 추후 online 상태가 되는 cpu들에 대해 hrtimer 초기화 루틴이 동작되도록 
 * notifier_block에 함수를 지정하여 등록한다.
 */
	register_cpu_notifier(&hrtimers_nb);
#ifdef CONFIG_HIGH_RES_TIMERS

/* IAMROOT-12:
 * -------------
 * hrtimer에 대한 벡터 핸들러 함수를 등록한다.
 */
	open_softirq(HRTIMER_SOFTIRQ, run_hrtimer_softirq);
#endif
}

/**
 * schedule_hrtimeout_range_clock - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 * @clock:	timer clock, CLOCK_MONOTONIC or CLOCK_REALTIME
 */
int __sched
schedule_hrtimeout_range_clock(ktime_t *expires, unsigned long delta,
			       const enum hrtimer_mode mode, int clock)
{
	struct hrtimer_sleeper t;

	/*
	 * Optimize when a zero timeout value is given. It does not
	 * matter whether this is an absolute or a relative time.
	 */
	if (expires && !expires->tv64) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	/*
	 * A NULL parameter means "infinite"
	 */
	if (!expires) {
		schedule();
		return -EINTR;
	}

	hrtimer_init_on_stack(&t.timer, clock, mode);
	hrtimer_set_expires_range_ns(&t.timer, *expires, delta);

	hrtimer_init_sleeper(&t, current);

	hrtimer_start_expires(&t.timer, mode);
	if (!hrtimer_active(&t.timer))
		t.task = NULL;

	if (likely(t.task))
		schedule();

	hrtimer_cancel(&t.timer);
	destroy_hrtimer_on_stack(&t.timer);

	__set_current_state(TASK_RUNNING);

	return !t.task ? 0 : -EINTR;
}

/**
 * schedule_hrtimeout_range - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @delta:	slack in expires timeout (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * The @delta argument gives the kernel the freedom to schedule the
 * actual wakeup to a time that is both power and performance friendly.
 * The kernel give the normal best effort behavior for "@expires+@delta",
 * but may decide to fire the timer earlier, but no earlier than @expires.
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
int __sched schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
				     const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range_clock(expires, delta, mode,
					      CLOCK_MONOTONIC);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout_range);

/**
 * schedule_hrtimeout - sleep until timeout
 * @expires:	timeout value (ktime_t)
 * @mode:	timer mode, HRTIMER_MODE_ABS or HRTIMER_MODE_REL
 *
 * Make the current task sleep until the given expiry time has
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout time is guaranteed to
 * pass before the routine returns.
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task.
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Returns 0 when the timer has expired otherwise -EINTR
 */
int __sched schedule_hrtimeout(ktime_t *expires,
			       const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range(expires, 0, mode);
}
EXPORT_SYMBOL_GPL(schedule_hrtimeout);
