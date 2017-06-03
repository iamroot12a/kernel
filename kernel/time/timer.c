/*
 *  linux/kernel/timer.c
 *
 *  Kernel internal timers
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-01-28  Modified by Finn Arne Gangstad to make timers scale better.
 *
 *  1997-09-10  Updated NTP code according to technical memorandum Jan '96
 *              "A Kernel Model for Precision Timekeeping" by Dave Mills
 *  1998-12-24  Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *              serialize accesses to xtime/lost_ticks).
 *                              Copyright (C) 1998  Andrea Arcangeli
 *  1999-03-10  Improved NTP compatibility by Ulrich Windl
 *  2002-05-31	Move sys_sysinfo here and make its locking sane, Robert Love
 *  2000-10-05  Implemented scalable SMP per-CPU timer handling.
 *                              Copyright (C) 2000, 2001, 2002  Ingo Molnar
 *              Designed by David S. Miller, Alexey Kuznetsov and Ingo Molnar
 */

#include <linux/kernel_stat.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/pid_namespace.h>
#include <linux/notifier.h>
#include <linux/thread_info.h>
#include <linux/time.h>
#include <linux/jiffies.h>
#include <linux/posix-timers.h>
#include <linux/cpu.h>
#include <linux/syscalls.h>
#include <linux/delay.h>
#include <linux/tick.h>
#include <linux/kallsyms.h>
#include <linux/irq_work.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/slab.h>
#include <linux/compat.h>

#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/div64.h>
#include <asm/timex.h>
#include <asm/io.h>

#define CREATE_TRACE_POINTS
#include <trace/events/timer.h>

__visible u64 jiffies_64 __cacheline_aligned_in_smp = INITIAL_JIFFIES;

EXPORT_SYMBOL(jiffies_64);

/*
 * per-CPU timer vector definitions:
 */

/* IAMROOT-12:
 * -------------
 * CONFIG_BASE_SMALL
 *	소형 커널 이미지 footprint로 빌드할 때 사용한다.
 *
 * TVN_SIZE=64
 * TVR_SIZE=256 
 * TVN_MASK=0x3f 
 * TVR_MASK=0xff
 *
 * MAX_TVAL=0xffff_ffff
 */
#define TVN_BITS (CONFIG_BASE_SMALL ? 4 : 6)
#define TVR_BITS (CONFIG_BASE_SMALL ? 6 : 8)
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)
#define MAX_TVAL ((unsigned long)((1ULL << (TVR_BITS + 4*TVN_BITS)) - 1))

/* IAMROOT-12:
 * -------------
 * lowres timer 들이 등록되는 리스트 어레이
 * (for tv2, tv3, tv4, tv5)
 */
struct tvec {
	struct list_head vec[TVN_SIZE];
};

/* IAMROOT-12:
 * -------------
 * lowres timer 들이 등록되는 리스트 어레이들 중 가장 하위 tick에서 동작 
 * (for tv1)
 */
struct tvec_root {
	struct list_head vec[TVR_SIZE];
};


/* IAMROOT-12:
 * -------------
 * *running_timer
 *	현재 만료되어 동작 중인 타이머 
 * timer_jiffies 
 *	현재 타이머 jiffies 
 * next_timer 
 *	타이머 휠내에서 가장 먼저 위치한 다음 타이머의 만료 시각(jiffies)
 *	타이머휠에 일반 타이머가 하나도 없는 경우 timer_jiffies와 동일한 시각
 * active_timers 
 *	타이머휠에 등록된 일반 타이머 수
 * all_timers
 *	타이머휠에 등록된 전체(일반 및 유예) 타이머 수
 * cpu
 *	타이머휠이 동작하는 cpu (타이머 휠은 cpu 마다 동작한다)
 * tv1 ~ tv5 
 *	타이머 리스트 어레이
 */
struct tvec_base {
	spinlock_t lock;
	struct timer_list *running_timer;
	unsigned long timer_jiffies;
	unsigned long next_timer;
	unsigned long active_timers;
	unsigned long all_timers;
	int cpu;
	struct tvec_root tv1;
	struct tvec tv2;
	struct tvec tv3;
	struct tvec tv4;
	struct tvec tv5;
} ____cacheline_aligned;

/* IAMROOT-12:
 * -------------
 * lowres 타이머 휠을 관리한다.
 */
struct tvec_base boot_tvec_bases;
EXPORT_SYMBOL(boot_tvec_bases);
static DEFINE_PER_CPU(struct tvec_base *, tvec_bases) = &boot_tvec_bases;

/* Functions below help us manage 'deferrable' flag */
static inline unsigned int tbase_get_deferrable(struct tvec_base *base)
{

/* IAMROOT-12:
 * -------------
 * 타이머 휠을 가리키는 base 주소의 1비트를 사용하여 유예 가능한 
 * 타이머 여부를 반환한다.
 *
 * 다음 함수로 생성된 타이머들
 *	- init_timer_deferrable() 
 *	- setup_deferrable_timer_on_stack() 
 *	- TIMER_DEFERRED_INITIALIZER
 */
	return ((unsigned int)(unsigned long)base & TIMER_DEFERRABLE);
}

static inline unsigned int tbase_get_irqsafe(struct tvec_base *base)
{
	return ((unsigned int)(unsigned long)base & TIMER_IRQSAFE);
}

static inline struct tvec_base *tbase_get_base(struct tvec_base *base)
{
	return ((struct tvec_base *)((unsigned long)base & ~TIMER_FLAG_MASK));
}

static inline void
timer_set_base(struct timer_list *timer, struct tvec_base *new_base)
{
	unsigned long flags = (unsigned long)timer->base & TIMER_FLAG_MASK;

	timer->base = (struct tvec_base *)((unsigned long)(new_base) | flags);
}

static unsigned long round_jiffies_common(unsigned long j, int cpu,
		bool force_up)
{
	int rem;
	unsigned long original = j;

	/*
	 * We don't want all cpus firing their timers at once hitting the
	 * same lock or cachelines, so we skew each extra cpu with an extra
	 * 3 jiffies. This 3 jiffies came originally from the mm/ code which
	 * already did this.
	 * The skew is done by adding 3*cpunr, then round, then subtract this
	 * extra offset again.
	 */
	j += cpu * 3;

	rem = j % HZ;

	/*
	 * If the target jiffie is just after a whole second (which can happen
	 * due to delays of the timer irq, long irq off times etc etc) then
	 * we should round down to the whole second, not up. Use 1/4th second
	 * as cutoff for this rounding as an extreme upper bound for this.
	 * But never round down if @force_up is set.
	 */
	if (rem < HZ/4 && !force_up) /* round down */
		j = j - rem;
	else /* round up */
		j = j - rem + HZ;

	/* now that we have rounded, subtract the extra skew again */
	j -= cpu * 3;

	/*
	 * Make sure j is still in the future. Otherwise return the
	 * unmodified value.
	 */
	return time_is_after_jiffies(j) ? j : original;
}

/**
 * __round_jiffies - function to round jiffies to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * __round_jiffies() rounds an absolute time in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The exact rounding is skewed for each processor to avoid all
 * processors firing at the exact same time, which could lead
 * to lock contention or spurious cache line bouncing.
 *
 * The return value is the rounded version of the @j parameter.
 */
unsigned long __round_jiffies(unsigned long j, int cpu)
{
	return round_jiffies_common(j, cpu, false);
}
EXPORT_SYMBOL_GPL(__round_jiffies);

/**
 * __round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * __round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The exact rounding is skewed for each processor to avoid all
 * processors firing at the exact same time, which could lead
 * to lock contention or spurious cache line bouncing.
 *
 * The return value is the rounded version of the @j parameter.
 */
unsigned long __round_jiffies_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	return round_jiffies_common(j + j0, cpu, false) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_relative);

/**
 * round_jiffies - function to round jiffies to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * round_jiffies() rounds an absolute time in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
unsigned long round_jiffies(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), false);
}
EXPORT_SYMBOL_GPL(round_jiffies);

/**
 * round_jiffies_relative - function to round jiffies to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * round_jiffies_relative() rounds a time delta  in the future (in jiffies)
 * up or down to (approximately) full seconds. This is useful for timers
 * for which the exact time they fire does not matter too much, as long as
 * they fire approximately every X seconds.
 *
 * By rounding these timers to whole seconds, all such timers will fire
 * at the same time, rather than at various times spread out. The goal
 * of this is to have the CPU wake up less, which saves power.
 *
 * The return value is the rounded version of the @j parameter.
 */
unsigned long round_jiffies_relative(unsigned long j)
{
	return __round_jiffies_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_relative);

/**
 * __round_jiffies_up - function to round jiffies up to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * This is the same as __round_jiffies() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
unsigned long __round_jiffies_up(unsigned long j, int cpu)
{
	return round_jiffies_common(j, cpu, true);
}
EXPORT_SYMBOL_GPL(__round_jiffies_up);

/**
 * __round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 * @cpu: the processor number on which the timeout will happen
 *
 * This is the same as __round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu)
{
	unsigned long j0 = jiffies;

	/* Use j0 because jiffies might change while we run */
	return round_jiffies_common(j + j0, cpu, true) - j0;
}
EXPORT_SYMBOL_GPL(__round_jiffies_up_relative);

/**
 * round_jiffies_up - function to round jiffies up to a full second
 * @j: the time in (absolute) jiffies that should be rounded
 *
 * This is the same as round_jiffies() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
unsigned long round_jiffies_up(unsigned long j)
{
	return round_jiffies_common(j, raw_smp_processor_id(), true);
}
EXPORT_SYMBOL_GPL(round_jiffies_up);

/**
 * round_jiffies_up_relative - function to round jiffies up to a full second
 * @j: the time in (relative) jiffies that should be rounded
 *
 * This is the same as round_jiffies_relative() except that it will never
 * round down.  This is useful for timeouts for which the exact time
 * of firing does not matter too much, as long as they don't fire too
 * early.
 */
unsigned long round_jiffies_up_relative(unsigned long j)
{
	return __round_jiffies_up_relative(j, raw_smp_processor_id());
}
EXPORT_SYMBOL_GPL(round_jiffies_up_relative);

/**
 * set_timer_slack - set the allowed slack for a timer
 * @timer: the timer to be modified
 * @slack_hz: the amount of time (in jiffies) allowed for rounding
 *
 * Set the amount of time, in jiffies, that a certain timer has
 * in terms of slack. By setting this value, the timer subsystem
 * will schedule the actual timer somewhere between
 * the time mod_timer() asks for, and that time plus the slack.
 *
 * By setting the slack to -1, a percentage of the delay is used
 * instead.
 */
void set_timer_slack(struct timer_list *timer, int slack_hz)
{
	timer->slack = slack_hz;
}
EXPORT_SYMBOL_GPL(set_timer_slack);

/*
 * If the list is empty, catch up ->timer_jiffies to the current time.
 * The caller must hold the tvec_base lock.  Returns true if the list
 * was empty and therefore ->timer_jiffies was updated.
 */
static bool catchup_timer_jiffies(struct tvec_base *base)
{

/* IAMROOT-12:
 * -------------
 * 타이머휠이 빈 경우 timer_jiffies에 현재 시각을 대입하고 true를 반환한다.
 */
	if (!base->all_timers) {
		base->timer_jiffies = jiffies;
		return true;
	}
	return false;
}

static void
__internal_add_timer(struct tvec_base *base, struct timer_list *timer)
{
	unsigned long expires = timer->expires;

/* IAMROOT-12:
 * -------------
 * 만료 시각 - now jiffes(타이머휠에 마지막 접근한 시각) = idx
 *
 * idx: delta tick
 */
	unsigned long idx = expires - base->timer_jiffies;
	struct list_head *vec;

/* IAMROOT-12:
 * -------------
 * delta로 tv1 ~ tv5를 선택하고,
 * 만료시각으로 리스트의 인덱스를 결정한다.
 *
 * delta가 256 tick 미만인 경우 tv1에 추가한다.
 * tv1에 추가할 리스트 어레이의 인덱스는 만료시각 % 256으로 결정한다.
 *                                       ---------------
 *                                       tv1에 해당하는 8비트 
 */
	if (idx < TVR_SIZE) {
		int i = expires & TVR_MASK;
		vec = base->tv1.vec + i;

/* IAMROOT-12:
 * -------------
 * delta가 256*64 tick 미만인 경우 tv2에 추가한다. 
 * tv2에 추가할 리스트 어레이의 인덱스는 만료시각에서 tv2에 해당하는 6 비트만 
 * 취한다. (tv3 ~ tv6까지 동일하다)
 */
	} else if (idx < 1 << (TVR_BITS + TVN_BITS)) {
		int i = (expires >> TVR_BITS) & TVN_MASK;
		vec = base->tv2.vec + i;
	} else if (idx < 1 << (TVR_BITS + 2 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK;
		vec = base->tv3.vec + i;
	} else if (idx < 1 << (TVR_BITS + 3 * TVN_BITS)) {
		int i = (expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK;
		vec = base->tv4.vec + i;
	} else if ((signed long) idx < 0) {
		/*
		 * Can happen if you add a timer with expires == jiffies,
		 * or you set a timer to go off in the past
		 */

/* IAMROOT-12:
 * -------------
 * 만료 시각이 지난 경우 만료 시각 대신 현재 시각에 해당하는 tv1의 리스트를 
 * 선택하게 한다.
 */
		vec = base->tv1.vec + (base->timer_jiffies & TVR_MASK);
	} else {
		int i;
		/* If the timeout is larger than MAX_TVAL (on 64-bit
		 * architectures or with CONFIG_BASE_SMALL=1) then we
		 * use the maximum timeout.
		 */
		if (idx > MAX_TVAL) {
			idx = MAX_TVAL;
			expires = idx + base->timer_jiffies;
		}
		i = (expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK;
		vec = base->tv5.vec + i;
	}
	/*
	 * Timers are FIFO:
	 */

/* IAMROOT-12:
 * -------------
 * 해당한 리스트를 찾으면 후미에 추가한다.
 */
	list_add_tail(&timer->entry, vec);
}

static void internal_add_timer(struct tvec_base *base, struct timer_list *timer)
{

/* IAMROOT-12:
 * -------------
 * 대기중인 타이머가 없는 타이머휠인 경우 타이머휠의 시각을 현재 시각으로 설정.
 */
	(void)catchup_timer_jiffies(base);

/* IAMROOT-12:
 * -------------
 * 타이머 휠에 타이머를 추가한다.
 */
	__internal_add_timer(base, timer);
	/*
	 * Update base->active_timers and base->next_timer
	 */

/* IAMROOT-12:
 * -------------
 * 유예 타이머가 아닌 경우 첫 등록이거나 타이머 휠내에서 가장 앞서는 타이머인 
 * 경우 next_timer(jiffies)를 갱신한다.
 */
	if (!tbase_get_deferrable(timer->base)) {
		if (!base->active_timers++ ||
		    time_before(timer->expires, base->next_timer))
			base->next_timer = timer->expires;
	}
	base->all_timers++;

	/*
	 * Check whether the other CPU is in dynticks mode and needs
	 * to be triggered to reevaluate the timer wheel.
	 * We are protected against the other CPU fiddling
	 * with the timer by holding the timer base lock. This also
	 * makes sure that a CPU on the way to stop its tick can not
	 * evaluate the timer wheel.
	 *
	 * Spare the IPI for deferrable timers on idle targets though.
	 * The next busy ticks will take care of it. Except full dynticks
	 * require special care against races with idle_cpu(), lets deal
	 * with that later.
	 */

/* IAMROOT-12:
 * -------------
 * active 타이머이거나 타이머휠이 nohz로 동작하고 있는 cpu인 경우 
 * nohz 모드에서 벗어나게 한다. (다른 cpu에서 처음 등록한 타이머가 
 * 다른 cpu에 의해 만료 시각이 바뀌었는데 원래 처음 타이머를 설정한 cpu가 
 * nohz full로 동작하고 있으면 tick이 발생하지 않아 만료 시각 체크를 할 수 
 * 없다. 따라서 이러한 경우 tick을 발생하게 요청해야 한다.)
 */
	if (!tbase_get_deferrable(base) || tick_nohz_full_cpu(base->cpu))
		wake_up_nohz_cpu(base->cpu);
}

#ifdef CONFIG_TIMER_STATS
void __timer_stats_timer_set_start_info(struct timer_list *timer, void *addr)
{
	if (timer->start_site)
		return;

/* IAMROOT-12:
 * -------------
 * 디버그 정보로 주소, 태스크명, 태스크 pid 값을 지정한다.
 */
	timer->start_site = addr;
	memcpy(timer->start_comm, current->comm, TASK_COMM_LEN);
	timer->start_pid = current->pid;
}

static void timer_stats_account_timer(struct timer_list *timer)
{
	unsigned int flag = 0;

	if (likely(!timer->start_site))
		return;
	if (unlikely(tbase_get_deferrable(timer->base)))
		flag |= TIMER_STATS_FLAG_DEFERRABLE;

	timer_stats_update_stats(timer, timer->start_pid, timer->start_site,
				 timer->function, timer->start_comm, flag);
}

#else
static void timer_stats_account_timer(struct timer_list *timer) {}
#endif

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS

static struct debug_obj_descr timer_debug_descr;

static void *timer_debug_hint(void *addr)
{
	return ((struct timer_list *) addr)->function;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int timer_fixup_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		del_timer_sync(timer);
		debug_object_init(timer, &timer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/* Stub timer callback for improperly used timers. */
static void stub_timer(unsigned long data)
{
	WARN_ON(1);
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
static int timer_fixup_activate(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		/*
		 * This is not really a fixup. The timer was
		 * statically initialized. We just make sure that it
		 * is tracked in the object tracker.
		 */
		if (timer->entry.next == NULL &&
		    timer->entry.prev == TIMER_ENTRY_STATIC) {
			debug_object_init(timer, &timer_debug_descr);
			debug_object_activate(timer, &timer_debug_descr);
			return 0;
		} else {
			setup_timer(timer, stub_timer, 0);
			return 1;
		}
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
static int timer_fixup_free(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		del_timer_sync(timer);
		debug_object_free(timer, &timer_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_assert_init is called when:
 * - an untracked/uninit-ed object is found
 */
static int timer_fixup_assert_init(void *addr, enum debug_obj_state state)
{
	struct timer_list *timer = addr;

	switch (state) {
	case ODEBUG_STATE_NOTAVAILABLE:
		if (timer->entry.prev == TIMER_ENTRY_STATIC) {
			/*
			 * This is not really a fixup. The timer was
			 * statically initialized. We just make sure that it
			 * is tracked in the object tracker.
			 */
			debug_object_init(timer, &timer_debug_descr);
			return 0;
		} else {
			setup_timer(timer, stub_timer, 0);
			return 1;
		}
	default:
		return 0;
	}
}

static struct debug_obj_descr timer_debug_descr = {
	.name			= "timer_list",
	.debug_hint		= timer_debug_hint,
	.fixup_init		= timer_fixup_init,
	.fixup_activate		= timer_fixup_activate,
	.fixup_free		= timer_fixup_free,
	.fixup_assert_init	= timer_fixup_assert_init,
};

static inline void debug_timer_init(struct timer_list *timer)
{
	debug_object_init(timer, &timer_debug_descr);
}

static inline void debug_timer_activate(struct timer_list *timer)
{
	debug_object_activate(timer, &timer_debug_descr);
}

static inline void debug_timer_deactivate(struct timer_list *timer)
{
	debug_object_deactivate(timer, &timer_debug_descr);
}

static inline void debug_timer_free(struct timer_list *timer)
{
	debug_object_free(timer, &timer_debug_descr);
}

static inline void debug_timer_assert_init(struct timer_list *timer)
{
	debug_object_assert_init(timer, &timer_debug_descr);
}

static void do_init_timer(struct timer_list *timer, unsigned int flags,
			  const char *name, struct lock_class_key *key);

void init_timer_on_stack_key(struct timer_list *timer, unsigned int flags,
			     const char *name, struct lock_class_key *key)
{
	debug_object_init_on_stack(timer, &timer_debug_descr);
	do_init_timer(timer, flags, name, key);
}
EXPORT_SYMBOL_GPL(init_timer_on_stack_key);

void destroy_timer_on_stack(struct timer_list *timer)
{
	debug_object_free(timer, &timer_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_timer_on_stack);

#else
static inline void debug_timer_init(struct timer_list *timer) { }
static inline void debug_timer_activate(struct timer_list *timer) { }
static inline void debug_timer_deactivate(struct timer_list *timer) { }
static inline void debug_timer_assert_init(struct timer_list *timer) { }
#endif

static inline void debug_init(struct timer_list *timer)
{
	debug_timer_init(timer);
	trace_timer_init(timer);
}

static inline void
debug_activate(struct timer_list *timer, unsigned long expires)
{
	debug_timer_activate(timer);
	trace_timer_start(timer, expires);
}

static inline void debug_deactivate(struct timer_list *timer)
{
	debug_timer_deactivate(timer);
	trace_timer_cancel(timer);
}

static inline void debug_assert_init(struct timer_list *timer)
{
	debug_timer_assert_init(timer);
}

static void do_init_timer(struct timer_list *timer, unsigned int flags,
			  const char *name, struct lock_class_key *key)
{
	struct tvec_base *base = raw_cpu_read(tvec_bases);

	timer->entry.next = NULL;
	timer->base = (void *)((unsigned long)base | flags);
	timer->slack = -1;
#ifdef CONFIG_TIMER_STATS
	timer->start_site = NULL;
	timer->start_pid = -1;
	memset(timer->start_comm, 0, TASK_COMM_LEN);
#endif
	lockdep_init_map(&timer->lockdep_map, name, key, 0);
}

/**
 * init_timer_key - initialize a timer
 * @timer: the timer to be initialized
 * @flags: timer flags
 * @name: name of the timer
 * @key: lockdep class key of the fake lock used for tracking timer
 *       sync lock dependencies
 *
 * init_timer_key() must be done to a timer prior calling *any* of the
 * other timer functions.
 */
void init_timer_key(struct timer_list *timer, unsigned int flags,
		    const char *name, struct lock_class_key *key)
{
	debug_init(timer);
	do_init_timer(timer, flags, name, key);
}
EXPORT_SYMBOL(init_timer_key);

static inline void detach_timer(struct timer_list *timer, bool clear_pending)
{
	struct list_head *entry = &timer->entry;

	debug_deactivate(timer);

/* IAMROOT-12:
 * -------------
 * __list_del을 하면 
 *   A(head)  <------>   B(del)  <--->   C
 *
 * 다음과 같이 B 값은 변경을 하지 않는다.
 *   A(head)  <---------------------->   C 
 *   A(head)  <-------   B(del) ----->   C
 * 
 * 이 때 인수로 clear_pending이 1인 경우 B의 next를 null로 하여 뒤에 연결된 
 * 타이머가 확실히 없게 만든다.
 *
 * B의 prev는 poison을 설치한다.
 */
	__list_del(entry->prev, entry->next);
	if (clear_pending)
		entry->next = NULL;
	entry->prev = LIST_POISON2;
}

static inline void
detach_expired_timer(struct timer_list *timer, struct tvec_base *base)
{
	detach_timer(timer, true);
	if (!tbase_get_deferrable(timer->base))
		base->active_timers--;
	base->all_timers--;
	(void)catchup_timer_jiffies(base);
}

static int detach_if_pending(struct timer_list *timer, struct tvec_base *base,
			     bool clear_pending)
{

/* IAMROOT-12:
 * -------------
 * 타이머 휠에 등록되지 않은 타이머는 0을 반환한다.
 * (detach = 타이머휠에서 타이머를 분리한다)
 */
	if (!timer_pending(timer))
		return 0;

/* IAMROOT-12:
 * -------------
 * 타이머 휠에서 분리하여 외톨이 상태로 둔다.
 */
	detach_timer(timer, clear_pending);

/* IAMROOT-12:
 * -------------
 * 유예 가능한 타이머가 아닌 경우 active 타이머 수에서 1을 감소시키고 
 * detach할 타이머가 다음에 만료 실행될 타이머인 경우 타이머휠이 
 * 비게된다. 그러면 next_timer 만료 시각에 현재 timer_jiffies로 
 * 동일하게 설정한다.
 *
 * (타이머 휠이 비어있을 때 next_timer는 언제나 timer_jiffies와 동일하다)
 */

	if (!tbase_get_deferrable(timer->base)) {
		base->active_timers--;
		if (timer->expires == base->next_timer)
			base->next_timer = base->timer_jiffies;
	}

/* IAMROOT-12:
 * -------------
 * 일반 타이머나 유예 가능한 타이머인 모든 경우에 all_timers 수에서 1을 감소 
 */
	base->all_timers--;
	(void)catchup_timer_jiffies(base);
	return 1;
}

/*
 * We are using hashed locking: holding per_cpu(tvec_bases).lock
 * means that all timers which are tied to this base via timer->base are
 * locked, and the base itself is locked too.
 *
 * So __run_timers/migrate_timers can safely modify all timers which could
 * be found on ->tvX lists.
 *
 * When the timer's base is locked, and the timer removed from list, it is
 * possible to set timer->base = NULL and drop the lock: the timer remains
 * locked.
 */
static struct tvec_base *lock_timer_base(struct timer_list *timer,
					unsigned long *flags)
	__acquires(timer->base->lock)
{
	struct tvec_base *base;


/* IAMROOT-12:
 * -------------
 * 타이머의 base가 null로 되어 있는 경우 루프를 돌며 null이 아닌 값이 
 * 올 때 까지 기다렸다가 lock을 얻은 후 base를 반환한다.
 */
	for (;;) {
		struct tvec_base *prelock_base = timer->base;
		base = tbase_get_base(prelock_base);
		if (likely(base != NULL)) {
			spin_lock_irqsave(&base->lock, *flags);
			if (likely(prelock_base == timer->base))
				return base;
			/* The timer has migrated to another CPU */
			spin_unlock_irqrestore(&base->lock, *flags);
		}
		cpu_relax();
	}
}

static inline int
__mod_timer(struct timer_list *timer, unsigned long expires,
						bool pending_only, int pinned)
{
	struct tvec_base *base, *new_base;
	unsigned long flags;
	int ret = 0 , cpu;

/* IAMROOT-12:
 * -------------
 * "echo 1 > /proc/timer_stat" 한 후 부터 "cat" 명령으로 타이머 리스트의
 *  stat 정보를 확인할 수 있다.
 */
	timer_stats_timer_set_start_info(timer);
	BUG_ON(!timer->function);

	base = lock_timer_base(timer, &flags);


/* IAMROOT-12:
 * -------------
 * 타이머를 타이머휠에서 분리한다. 만일 실패하고 pending_only가 true인 경우 
 * 더이상 처리하지 않는다.
 *
 * pending_only=true인 것은 타이머휠에 등록된 타이머에 대해서만 modify 작업을
 * 하겠다는 의미이다.
 */
	ret = detach_if_pending(timer, base, false);
	if (!ret && pending_only)
		goto out_unlock;

	debug_activate(timer, expires);

/* IAMROOT-12:
 * -------------
 * nohz를 지원하지 않는 시스템은 현재 cpu를 그대로 사용해서 타이머를 등록하지만,
 * nohz를 지원하는 경우에는 현재 cpu의 상태에 따라 다른 cpu를 사용해야 할 수 
 * 있다. 이러한 경우 사용할 cpu를 알아온다.
 */
	cpu = get_nohz_timer_target(pinned);
	new_base = per_cpu(tvec_bases, cpu);

/* IAMROOT-12:
 * -------------
 * 만일 현재 cpu의 타이머휠이 아닌 다른 cpu의 타이머휠을 선택한 경우
 */
	if (base != new_base) {
		/*
		 * We are trying to schedule the timer on the local CPU.
		 * However we can't change timer's base while it is running,
		 * otherwise del_timer_sync() can't detect that the timer's
		 * handler yet has not finished. This also guarantees that
		 * the timer is serialized wrt itself.
		 */

/* IAMROOT-12:
 * -------------
 * 요청한 타이머가 만료 처리 진행 중인 타이머가 아닌 경우 
 * 새로운 타이머 휠을 가리키게한다.
 */
		if (likely(base->running_timer != timer)) {
			/* See the comment in lock_timer_base() */
			timer_set_base(timer, NULL);
			spin_unlock(&base->lock);
			base = new_base;
			spin_lock(&base->lock);
			timer_set_base(timer, base);
		}
	}

/* IAMROOT-12:
 * -------------
 * 만료 시각을 변경하고 타이머휠에 추가한다.
 */
	timer->expires = expires;

	internal_add_timer(base, timer);

out_unlock:
	spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}

/**
 * mod_timer_pending - modify a pending timer's timeout
 * @timer: the pending timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer_pending() is the same for pending timers as mod_timer(),
 * but will not re-activate and modify already deleted timers.
 *
 * It is useful for unserialized use of timers.
 */
int mod_timer_pending(struct timer_list *timer, unsigned long expires)
{
	return __mod_timer(timer, expires, true, TIMER_NOT_PINNED);
}
EXPORT_SYMBOL(mod_timer_pending);

/*
 * Decide where to put the timer while taking the slack into account
 *
 * Algorithm:
 *   1) calculate the maximum (absolute) time
 *   2) calculate the highest bit where the expires and new max are different
 *   3) use this bit to make a mask
 *   4) use the bitmask to round down the maximum time, so that all last
 *      bits are zeros
 */
static inline
unsigned long apply_slack(struct timer_list *timer, unsigned long expires)
{
	unsigned long expires_limit, mask;
	int bit;

	if (timer->slack >= 0) {
		expires_limit = expires + timer->slack;
	} else {

/* IAMROOT-12:
 * -------------
 * slack의 초기 값은 -1 이다.
 * 만료까지 남은 틱이 256보다 작은 경우 slack 적용하지 않는다. 
 */
		long delta = expires - jiffies;

		if (delta < 256)
			return expires;

/* IAMROOT-12:
 * -------------
 * 만료까지 남은 틱을 256으로 나눈 값을 만료 시각에 더해 최대치를 둔다.
 */
		expires_limit = expires + delta / 256;
	}

/* IAMROOT-12:
 * -------------
 * expires_limit와 expires 비트들을 eor 하여 다른 최초 비트이하를 마스크한 
 * 값을 반환한다.
 */
	mask = expires ^ expires_limit;
	if (mask == 0)
		return expires;

	bit = find_last_bit(&mask, BITS_PER_LONG);

	mask = (1UL << bit) - 1;

	expires_limit = expires_limit & ~(mask);

	return expires_limit;
}

/**
 * mod_timer - modify a timer's timeout
 * @timer: the timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer() is a more efficient way to update the expire field of an
 * active timer (if the timer is inactive it will be activated)
 *
 * mod_timer(timer, expires) is equivalent to:
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 *
 * Note that if there are multiple unserialized concurrent users of the
 * same timer, then mod_timer() is the only safe way to modify the timeout,
 * since add_timer() cannot modify an already running timer.
 *
 * The function returns whether it has modified a pending timer or not.
 * (ie. mod_timer() of an inactive timer returns 0, mod_timer() of an
 * active timer returns 1.)
 */
int mod_timer(struct timer_list *timer, unsigned long expires)
{

/* IAMROOT-12:
 * -------------
 * slack을 적용한 만료 시각을 산출한다.
 * (slack 설정이 -1인 경우는 남은 만료 시간이 254 tick 이내이면 slack이 
 * 적용되지 않고 큰 경우만 delta/256 범위내에서 적용된다.
 * (delta 기간에 대한 1/256 비율이 최대 slack으로 자동 지정된다)
 */
	expires = apply_slack(timer, expires);

	/*
	 * This is a common optimization triggered by the
	 * networking code - if the timer is re-modified
	 * to be the same thing then just return:
	 */

/* IAMROOT-12:
 * -------------
 * 타이머 휠에 등록되었고 동일 expires 설정인 경우 타이머를
 * modify하지 않는다.
 */
	if (timer_pending(timer) && timer->expires == expires)
		return 1;

	return __mod_timer(timer, expires, false, TIMER_NOT_PINNED);
}
EXPORT_SYMBOL(mod_timer);

/**
 * mod_timer_pinned - modify a timer's timeout
 * @timer: the timer to be modified
 * @expires: new timeout in jiffies
 *
 * mod_timer_pinned() is a way to update the expire field of an
 * active timer (if the timer is inactive it will be activated)
 * and to ensure that the timer is scheduled on the current CPU.
 *
 * Note that this does not prevent the timer from being migrated
 * when the current CPU goes offline.  If this is a problem for
 * you, use CPU-hotplug notifiers to handle it correctly, for
 * example, cancelling the timer when the corresponding CPU goes
 * offline.
 *
 * mod_timer_pinned(timer, expires) is equivalent to:
 *
 *     del_timer(timer); timer->expires = expires; add_timer(timer);
 */
int mod_timer_pinned(struct timer_list *timer, unsigned long expires)
{
	if (timer->expires == expires && timer_pending(timer))
		return 1;

	return __mod_timer(timer, expires, false, TIMER_PINNED);
}
EXPORT_SYMBOL(mod_timer_pinned);

/**
 * add_timer - start a timer
 * @timer: the timer to be added
 *
 * The kernel will do a ->function(->data) callback from the
 * timer interrupt at the ->expires point in the future. The
 * current time is 'jiffies'.
 *
 * The timer's ->expires, ->function (and if the handler uses it, ->data)
 * fields must be set prior calling this function.
 *
 * Timers with an ->expires field in the past will be executed in the next
 * timer tick.
 */
void add_timer(struct timer_list *timer)
{

/* IAMROOT-12:
 * -------------
 * tvec 리스트에 이미 등록된 경우 버그
 */
	BUG_ON(timer_pending(timer));

/* IAMROOT-12:
 * -------------
 * 요청한 타이머의 만료시각을 설정(변경)한다.
 */
	mod_timer(timer, timer->expires);
}
EXPORT_SYMBOL(add_timer);

/**
 * add_timer_on - start a timer on a particular CPU
 * @timer: the timer to be added
 * @cpu: the CPU to start it on
 *
 * This is not very scalable on SMP. Double adds are not possible.
 */
void add_timer_on(struct timer_list *timer, int cpu)
{
	struct tvec_base *base = per_cpu(tvec_bases, cpu);
	unsigned long flags;

	timer_stats_timer_set_start_info(timer);
	BUG_ON(timer_pending(timer) || !timer->function);
	spin_lock_irqsave(&base->lock, flags);
	timer_set_base(timer, base);
	debug_activate(timer, timer->expires);
	internal_add_timer(base, timer);
	spin_unlock_irqrestore(&base->lock, flags);
}
EXPORT_SYMBOL_GPL(add_timer_on);

/**
 * del_timer - deactive a timer.
 * @timer: the timer to be deactivated
 *
 * del_timer() deactivates a timer - this works on both active and inactive
 * timers.
 *
 * The function returns whether it has deactivated a pending timer or not.
 * (ie. del_timer() of an inactive timer returns 0, del_timer() of an
 * active timer returns 1.)
 */
int del_timer(struct timer_list *timer)
{
	struct tvec_base *base;
	unsigned long flags;
	int ret = 0;

	debug_assert_init(timer);

	timer_stats_timer_clear_start_info(timer);
	if (timer_pending(timer)) {
		base = lock_timer_base(timer, &flags);
		ret = detach_if_pending(timer, base, true);
		spin_unlock_irqrestore(&base->lock, flags);
	}

	return ret;
}
EXPORT_SYMBOL(del_timer);

/**
 * try_to_del_timer_sync - Try to deactivate a timer
 * @timer: timer do del
 *
 * This function tries to deactivate a timer. Upon successful (ret >= 0)
 * exit the timer is not queued and the handler is not running on any CPU.
 */
int try_to_del_timer_sync(struct timer_list *timer)
{
	struct tvec_base *base;
	unsigned long flags;
	int ret = -1;

	debug_assert_init(timer);

	base = lock_timer_base(timer, &flags);

	if (base->running_timer != timer) {
		timer_stats_timer_clear_start_info(timer);
		ret = detach_if_pending(timer, base, true);
	}
	spin_unlock_irqrestore(&base->lock, flags);

	return ret;
}
EXPORT_SYMBOL(try_to_del_timer_sync);

#ifdef CONFIG_SMP
/**
 * del_timer_sync - deactivate a timer and wait for the handler to finish.
 * @timer: the timer to be deactivated
 *
 * This function only differs from del_timer() on SMP: besides deactivating
 * the timer it also makes sure the handler has finished executing on other
 * CPUs.
 *
 * Synchronization rules: Callers must prevent restarting of the timer,
 * otherwise this function is meaningless. It must not be called from
 * interrupt contexts unless the timer is an irqsafe one. The caller must
 * not hold locks which would prevent completion of the timer's
 * handler. The timer's handler must not call add_timer_on(). Upon exit the
 * timer is not queued and the handler is not running on any CPU.
 *
 * Note: For !irqsafe timers, you must not hold locks that are held in
 *   interrupt context while calling this function. Even if the lock has
 *   nothing to do with the timer in question.  Here's why:
 *
 *    CPU0                             CPU1
 *    ----                             ----
 *                                   <SOFTIRQ>
 *                                   call_timer_fn();
 *                                     base->running_timer = mytimer;
 *  spin_lock_irq(somelock);
 *                                     <IRQ>
 *                                        spin_lock(somelock);
 *  del_timer_sync(mytimer);
 *   while (base->running_timer == mytimer);
 *
 * Now del_timer_sync() will never return and never release somelock.
 * The interrupt on the other CPU is waiting to grab somelock but
 * it has interrupted the softirq that CPU0 is waiting to finish.
 *
 * The function returns whether it has deactivated a pending timer or not.
 */
int del_timer_sync(struct timer_list *timer)
{
#ifdef CONFIG_LOCKDEP
	unsigned long flags;

	/*
	 * If lockdep gives a backtrace here, please reference
	 * the synchronization rules above.
	 */
	local_irq_save(flags);
	lock_map_acquire(&timer->lockdep_map);
	lock_map_release(&timer->lockdep_map);
	local_irq_restore(flags);
#endif
	/*
	 * don't use it in hardirq context, because it
	 * could lead to deadlock.
	 */
	WARN_ON(in_irq() && !tbase_get_irqsafe(timer->base));
	for (;;) {
		int ret = try_to_del_timer_sync(timer);
		if (ret >= 0)
			return ret;
		cpu_relax();
	}
}
EXPORT_SYMBOL(del_timer_sync);
#endif


/* IAMROOT-12:
 * -------------
 * cascade(base, tv5, INDEX(3))
 *	- timer_jiffies에서 tv5에 해당하는 인덱스
 */
static int cascade(struct tvec_base *base, struct tvec *tv, int index)
{
	/* cascade all the timers from tv up one level */
	struct timer_list *timer, *tmp;
	struct list_head tv_list;

/* IAMROOT-12:
 * -------------
 * tv->vec + index에 있는 리스트를 임시 tv_list로 옮긴다.
 * 옮기고 난 후 기존 리스트는 초기화한다.
 */
	list_replace_init(tv->vec + index, &tv_list);

	/*
	 * We are removing _all_ timers from the list, so we
	 * don't have to detach them individually.
	 */

/* IAMROOT-12:
 * -------------
 * 임시 tv_list에 있는 타이머들을 순회하며 다시 추가한다.
 */
	list_for_each_entry_safe(timer, tmp, &tv_list, entry) {
		BUG_ON(tbase_get_base(timer->base) != base);
		/* No accounting, while moving them */
		__internal_add_timer(base, timer);
	}

/* IAMROOT-12:
 * -------------
 * 인덱스 값을 반환한다. ( ~ TVN_SIZE(64))
 */
	return index;
}

static void call_timer_fn(struct timer_list *timer, void (*fn)(unsigned long),
			  unsigned long data)
{
	int count = preempt_count();

#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the timer from inside the
	 * function that is called from it, this we need to take into
	 * account for lockdep too. To avoid bogus "held lock freed"
	 * warnings as well as problems when looking into
	 * timer->lockdep_map, make a copy and use that here.
	 */
	struct lockdep_map lockdep_map;

	lockdep_copy_map(&lockdep_map, &timer->lockdep_map);
#endif
	/*
	 * Couple the lock chain with the lock chain at
	 * del_timer_sync() by acquiring the lock_map around the fn()
	 * call here and in del_timer_sync().
	 */
	lock_map_acquire(&lockdep_map);

	trace_timer_expire_entry(timer);
	fn(data);
	trace_timer_expire_exit(timer);

	lock_map_release(&lockdep_map);

	if (count != preempt_count()) {
		WARN_ONCE(1, "timer: %pF preempt leak: %08x -> %08x\n",
			  fn, count, preempt_count());
		/*
		 * Restore the preempt count. That gives us a decent
		 * chance to survive and extract information. If the
		 * callback kept a lock held, bad luck, but not worse
		 * than the BUG() we had.
		 */
		preempt_count_set(count);
	}
}


/* IAMROOT-12:
 * -------------
 * timer_jiffies에서 아래 비트에 해당하는 값만 반환한다.
 *
 * INDEX(0) -> tv2에 해당하는 비트.
 * INDEX(1) -> tv3 
 * INDEX(2) -> tv4 
 * INDEX(3) -> tv5
 */
#define INDEX(N) ((base->timer_jiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)

/**
 * __run_timers - run all expired timers (if any) on this CPU.
 * @base: the timer vector to be processed.
 *
 * This function cascades all vectors and executes all expired timer
 * vectors.
 */
static inline void __run_timers(struct tvec_base *base)
{
	struct timer_list *timer;

	spin_lock_irq(&base->lock);
	if (catchup_timer_jiffies(base)) {
		spin_unlock_irq(&base->lock);
		return;
	}

/* IAMROOT-12:
 * -------------
 * 현재 시각이 타이머휠의 시각을 지나간 경우
 */
	while (time_after_eq(jiffies, base->timer_jiffies)) {
		struct list_head work_list;
		struct list_head *head = &work_list;
		int index = base->timer_jiffies & TVR_MASK;

		/*
		 * Cascade timers:
		 */

/* IAMROOT-12:
 * -------------
 * cascade 작업은 최소 256 tick 단위마다 수행한다.
 *
 * 가장 가까이 있는 tv2 벡터부터 조사한다.
 * tv2의 timer_jiffes를 기준으로 인덱스를 찾아 타이머들을 re-add 한다.
 *
 * tv2 -> tv3 -> tv4 -> tv5 까지 cascade 한다.
 */
		if (!index &&
			(!cascade(base, &base->tv2, INDEX(0))) &&
				(!cascade(base, &base->tv3, INDEX(1))) &&
					!cascade(base, &base->tv4, INDEX(2)))
			cascade(base, &base->tv5, INDEX(3));
		++base->timer_jiffies;

/* IAMROOT-12:
 * -------------
 * tv1의 인덱스에 해당하는 타이머들을 임시 리스트인 work_list로 옮기고 
 * 기존 리스트는 초기화한다.
 *
 * tv1에 있는 어레이 리스트들 중 timer_jiffes를 256으로 나눈 나머지에 해당하는
 * 인덱스의 리스트들을 work_list로 옮겨서 여기에 있는 타이머들을 만료처리한다.
 */
		list_replace_init(base->tv1.vec + index, head);

/* IAMROOT-12:
 * -------------
 * work_list에 있는 타이머들에 대해 순회를 한다.
 */
		while (!list_empty(head)) {
			void (*fn)(unsigned long);
			unsigned long data;
			bool irqsafe;

			timer = list_first_entry(head, struct timer_list,entry);
			fn = timer->function;
			data = timer->data;

/* IAMROOT-12:
 * -------------
 * irq safe 여부를 알아온다.
 */
			irqsafe = tbase_get_irqsafe(timer->base);

/* IAMROOT-12:
 * -------------
 * 타이머 통계 정보 ("/proc/timer_stat"에서 사용)
 *
 * 여기서 타이머에 등록된 함수를 호출한다.
 */
			timer_stats_account_timer(timer);

			base->running_timer = timer;
			detach_expired_timer(timer, base);

/* IAMROOT-12:
 * -------------
 * 리스트내 2개 이상의 함수들에서 효과를 보기 위해 irqsafe 요청된 경우 
 * irq disable -> func A, func B, ... -> irq enable 과 같이 
 * irq disable/enable 횟수를 줄여준다.
 */
			if (irqsafe) {
				spin_unlock(&base->lock);
				call_timer_fn(timer, fn, data);
				spin_lock(&base->lock);
			} else {
				spin_unlock_irq(&base->lock);
				call_timer_fn(timer, fn, data);
				spin_lock_irq(&base->lock);
			}
		}
	}
	base->running_timer = NULL;
	spin_unlock_irq(&base->lock);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * Find out when the next timer event is due to happen. This
 * is used on S/390 to stop all activity when a CPU is idle.
 * This function needs to be called with interrupts disabled.
 */
static unsigned long __next_timer_interrupt(struct tvec_base *base)
{
	unsigned long timer_jiffies = base->timer_jiffies;
	unsigned long expires = timer_jiffies + NEXT_TIMER_MAX_DELTA;
	int index, slot, array, found = 0;
	struct timer_list *nte;
	struct tvec *varray[4];

	/* Look for timer events in tv1. */
	index = slot = timer_jiffies & TVR_MASK;
	do {
		list_for_each_entry(nte, base->tv1.vec + slot, entry) {
			if (tbase_get_deferrable(nte->base))
				continue;

			found = 1;
			expires = nte->expires;
			/* Look at the cascade bucket(s)? */
			if (!index || slot < index)
				goto cascade;
			return expires;
		}
		slot = (slot + 1) & TVR_MASK;
	} while (slot != index);

cascade:
	/* Calculate the next cascade event */
	if (index)
		timer_jiffies += TVR_SIZE - index;
	timer_jiffies >>= TVR_BITS;

	/* Check tv2-tv5. */
	varray[0] = &base->tv2;
	varray[1] = &base->tv3;
	varray[2] = &base->tv4;
	varray[3] = &base->tv5;

	for (array = 0; array < 4; array++) {
		struct tvec *varp = varray[array];

		index = slot = timer_jiffies & TVN_MASK;
		do {
			list_for_each_entry(nte, varp->vec + slot, entry) {
				if (tbase_get_deferrable(nte->base))
					continue;

				found = 1;
				if (time_before(nte->expires, expires))
					expires = nte->expires;
			}
			/*
			 * Do we still search for the first timer or are
			 * we looking up the cascade buckets ?
			 */
			if (found) {
				/* Look at the cascade bucket(s)? */
				if (!index || slot < index)
					break;
				return expires;
			}
			slot = (slot + 1) & TVN_MASK;
		} while (slot != index);

		if (index)
			timer_jiffies += TVN_SIZE - index;
		timer_jiffies >>= TVN_BITS;
	}
	return expires;
}

/*
 * Check, if the next hrtimer event is before the next timer wheel
 * event:
 */
static unsigned long cmp_next_hrtimer_event(unsigned long now,
					    unsigned long expires)
{
	ktime_t hr_delta = hrtimer_get_next_event();
	struct timespec tsdelta;
	unsigned long delta;

	if (hr_delta.tv64 == KTIME_MAX)
		return expires;

	/*
	 * Expired timer available, let it expire in the next tick
	 */
	if (hr_delta.tv64 <= 0)
		return now + 1;

	tsdelta = ktime_to_timespec(hr_delta);
	delta = timespec_to_jiffies(&tsdelta);

	/*
	 * Limit the delta to the max value, which is checked in
	 * tick_nohz_stop_sched_tick():
	 */
	if (delta > NEXT_TIMER_MAX_DELTA)
		delta = NEXT_TIMER_MAX_DELTA;

	/*
	 * Take rounding errors in to account and make sure, that it
	 * expires in the next tick. Otherwise we go into an endless
	 * ping pong due to tick_nohz_stop_sched_tick() retriggering
	 * the timer softirq
	 */
	if (delta < 1)
		delta = 1;
	now += delta;
	if (time_before(now, expires))
		return now;
	return expires;
}

/**
 * get_next_timer_interrupt - return the jiffy of the next pending timer
 * @now: current time (in jiffies)
 */
unsigned long get_next_timer_interrupt(unsigned long now)
{
	struct tvec_base *base = __this_cpu_read(tvec_bases);
	unsigned long expires = now + NEXT_TIMER_MAX_DELTA;

	/*
	 * Pretend that there is no timer pending if the cpu is offline.
	 * Possible pending timers will be migrated later to an active cpu.
	 */
	if (cpu_is_offline(smp_processor_id()))
		return expires;

	spin_lock(&base->lock);
	if (base->active_timers) {
		if (time_before_eq(base->next_timer, base->timer_jiffies))
			base->next_timer = __next_timer_interrupt(base);
		expires = base->next_timer;
	}
	spin_unlock(&base->lock);

	if (time_before_eq(expires, now))
		return now;

	return cmp_next_hrtimer_event(now, expires);
}
#endif

/*
 * Called from the timer interrupt handler to charge one tick to the current
 * process.  user_tick is 1 if the tick is user time, 0 for system.
 */
void update_process_times(int user_tick)
{
	struct task_struct *p = current;

	/* Note: this timer irq context must be accounted for as well. */
	account_process_tick(p, user_tick);
	run_local_timers();
	rcu_check_callbacks(user_tick);
#ifdef CONFIG_IRQ_WORK
	if (in_irq())
		irq_work_tick();
#endif
	scheduler_tick();
	run_posix_cpu_timers(p);
}

/*
 * This function runs timers and the timer-tq in bottom half context.
 */
static void run_timer_softirq(struct softirq_action *h)
{
	struct tvec_base *base = __this_cpu_read(tvec_bases);

/* IAMROOT-12:
 * -------------
 * 일반 lowres 타이머에 대한 bottom-half가 동작하는 부분이다. (softirq)
 *
 * lowres 타이머를 사용하다가 hrtimer가 다 준비되면 체제 이전을 하게된다.
 * 그 때까지 이 루틴을 사용한다. 
 */
	hrtimer_run_pending();

/* IAMROOT-12:
 * -------------
 * 현재 시각이 타이머휠의 시각을 지나간 경우 호출한다.
 */
	if (time_after_eq(jiffies, base->timer_jiffies))
		__run_timers(base);
}

/*
 * Called by the local, per-CPU timer interrupt on SMP.
 */
void run_local_timers(void)
{
	hrtimer_run_queues();
	raise_softirq(TIMER_SOFTIRQ);
}

#ifdef __ARCH_WANT_SYS_ALARM

/*
 * For backwards compatibility?  This can be done in libc so Alpha
 * and all newer ports shouldn't need it.
 */
SYSCALL_DEFINE1(alarm, unsigned int, seconds)
{
	return alarm_setitimer(seconds);
}

#endif

static void process_timeout(unsigned long __data)
{
	wake_up_process((struct task_struct *)__data);
}

/**
 * schedule_timeout - sleep until timeout
 * @timeout: timeout value in jiffies
 *
 * Make the current task sleep until @timeout jiffies have
 * elapsed. The routine will return immediately unless
 * the current task state has been set (see set_current_state()).
 *
 * You can set the task state as follows -
 *
 * %TASK_UNINTERRUPTIBLE - at least @timeout jiffies are guaranteed to
 * pass before the routine returns. The routine will return 0
 *
 * %TASK_INTERRUPTIBLE - the routine may return early if a signal is
 * delivered to the current task. In this case the remaining time
 * in jiffies will be returned, or 0 if the timer expired in time
 *
 * The current task state is guaranteed to be TASK_RUNNING when this
 * routine returns.
 *
 * Specifying a @timeout value of %MAX_SCHEDULE_TIMEOUT will schedule
 * the CPU away without a bound on the timeout. In this case the return
 * value will be %MAX_SCHEDULE_TIMEOUT.
 *
 * In all cases the return value is guaranteed to be non-negative.
 */
signed long __sched schedule_timeout(signed long timeout)
{
	struct timer_list timer;
	unsigned long expire;

	switch (timeout)
	{
	case MAX_SCHEDULE_TIMEOUT:
		/*
		 * These two special cases are useful to be comfortable
		 * in the caller. Nothing more. We could take
		 * MAX_SCHEDULE_TIMEOUT from one of the negative value
		 * but I' d like to return a valid offset (>=0) to allow
		 * the caller to do everything it want with the retval.
		 */
		schedule();
		goto out;
	default:
		/*
		 * Another bit of PARANOID. Note that the retval will be
		 * 0 since no piece of kernel is supposed to do a check
		 * for a negative retval of schedule_timeout() (since it
		 * should never happens anyway). You just have the printk()
		 * that will tell you if something is gone wrong and where.
		 */
		if (timeout < 0) {
			printk(KERN_ERR "schedule_timeout: wrong timeout "
				"value %lx\n", timeout);
			dump_stack();
			current->state = TASK_RUNNING;
			goto out;
		}
	}

	expire = timeout + jiffies;

	setup_timer_on_stack(&timer, process_timeout, (unsigned long)current);
	__mod_timer(&timer, expire, false, TIMER_NOT_PINNED);
	schedule();
	del_singleshot_timer_sync(&timer);

	/* Remove the timer from the object tracker */
	destroy_timer_on_stack(&timer);

	timeout = expire - jiffies;

 out:
	return timeout < 0 ? 0 : timeout;
}
EXPORT_SYMBOL(schedule_timeout);

/*
 * We can use __set_current_state() here because schedule_timeout() calls
 * schedule() unconditionally.
 */
signed long __sched schedule_timeout_interruptible(signed long timeout)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_interruptible);

signed long __sched schedule_timeout_killable(signed long timeout)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_killable);

signed long __sched schedule_timeout_uninterruptible(signed long timeout)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(timeout);
}
EXPORT_SYMBOL(schedule_timeout_uninterruptible);


/* IAMROOT-12:
 * -------------
 * 요청한 cpu의 lowres 타이머를 초기화한다.
 */
static int init_timers_cpu(int cpu)
{
	int j;
	struct tvec_base *base;

/* IAMROOT-12:
 * -------------
 * static 변수로 tvec의 base 할당 여부를 설정한다.
 */
	static char tvec_base_done[NR_CPUS];

	if (!tvec_base_done[cpu]) {
		static char boot_done;

/* IAMROOT-12:
 * -------------
 * 처음 cpu 초기화 시에는 tvec_bases 구조체를 기존 static 메모리를 사용하고
 * 다음 cpu 초기화할 때에는 별도로 할당을 받은 후 대입하여 사용한다.
 *
 * 차후 커널에서는 첫 번째 cpu에 대한 lowres 타이머를 초기화할 때에도 
 * per-cpu로 할당한다.
 */
		if (boot_done) {
			/*
			 * The APs use this path later in boot
			 */
			base = kzalloc_node(sizeof(*base), GFP_KERNEL,
					    cpu_to_node(cpu));
			if (!base)
				return -ENOMEM;

			/* Make sure tvec_base has TIMER_FLAG_MASK bits free */
			if (WARN_ON(base != tbase_get_base(base))) {
				kfree(base);
				return -ENOMEM;
			}
			per_cpu(tvec_bases, cpu) = base;
		} else {
			/*
			 * This is for the boot CPU - we use compile-time
			 * static initialisation because per-cpu memory isn't
			 * ready yet and because the memory allocators are not
			 * initialised either.
			 */
			boot_done = 1;
			base = &boot_tvec_bases;
		}
		spin_lock_init(&base->lock);

/* IAMROOT-12:
 * -------------
 * 해당 cpu에 대한 lowres 타이머를 초기화했음을 기록한다.
 */
		tvec_base_done[cpu] = 1;
		base->cpu = cpu;
	} else {
		base = per_cpu(tvec_bases, cpu);
	}

/* IAMROOT-12:
 * -------------
 * 4+1개의 tvec의 리스트 어레이들을 초기화한다.
 */
	for (j = 0; j < TVN_SIZE; j++) {
		INIT_LIST_HEAD(base->tv5.vec + j);
		INIT_LIST_HEAD(base->tv4.vec + j);
		INIT_LIST_HEAD(base->tv3.vec + j);
		INIT_LIST_HEAD(base->tv2.vec + j);
	}
	for (j = 0; j < TVR_SIZE; j++)
		INIT_LIST_HEAD(base->tv1.vec + j);

/* IAMROOT-12:
 * -------------
 * timer_jiffes 및 next_timer에 현재 시각(jiffies)을 대입하여 초기화한다.
 */
	base->timer_jiffies = jiffies;
	base->next_timer = base->timer_jiffies;

/* IAMROOT-12:
 * -------------
 * 현재 동작 중인 타이머는 하나도 없는 상태로 초기화한다.
 */
	base->active_timers = 0;
	base->all_timers = 0;
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static void migrate_timer_list(struct tvec_base *new_base, struct list_head *head)
{
	struct timer_list *timer;

	while (!list_empty(head)) {
		timer = list_first_entry(head, struct timer_list, entry);
		/* We ignore the accounting on the dying cpu */
		detach_timer(timer, false);
		timer_set_base(timer, new_base);
		internal_add_timer(new_base, timer);
	}
}

static void migrate_timers(int cpu)
{
	struct tvec_base *old_base;
	struct tvec_base *new_base;
	int i;

	BUG_ON(cpu_online(cpu));
	old_base = per_cpu(tvec_bases, cpu);
	new_base = get_cpu_var(tvec_bases);
	/*
	 * The caller is globally serialized and nobody else
	 * takes two locks at once, deadlock is not possible.
	 */
	spin_lock_irq(&new_base->lock);
	spin_lock_nested(&old_base->lock, SINGLE_DEPTH_NESTING);

	BUG_ON(old_base->running_timer);

	for (i = 0; i < TVR_SIZE; i++)
		migrate_timer_list(new_base, old_base->tv1.vec + i);
	for (i = 0; i < TVN_SIZE; i++) {
		migrate_timer_list(new_base, old_base->tv2.vec + i);
		migrate_timer_list(new_base, old_base->tv3.vec + i);
		migrate_timer_list(new_base, old_base->tv4.vec + i);
		migrate_timer_list(new_base, old_base->tv5.vec + i);
	}

	spin_unlock(&old_base->lock);
	spin_unlock_irq(&new_base->lock);
	put_cpu_var(tvec_bases);
}
#endif /* CONFIG_HOTPLUG_CPU */

static int timer_cpu_notify(struct notifier_block *self,
				unsigned long action, void *hcpu)
{
	long cpu = (long)hcpu;
	int err;

	switch(action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
/* IAMROOT-12:
 * -------------
 * cpu가 on되는 경우 해당 cpu의 lowres 타이머를 초기화한다.
 */
		err = init_timers_cpu(cpu);
		if (err < 0)
			return notifier_from_errno(err);
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:

/* IAMROOT-12:
 * -------------
 * cpu가 off되는 경우 다른 cpu로 현재 lowres 타이머들을 옮긴다.
 */
		migrate_timers(cpu);
		break;
#endif
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block timers_nb = {
	.notifier_call	= timer_cpu_notify,
};

/* IAMROOT-12:
 * -------------
 * lowres 타이머 초기화
 */
void __init init_timers(void)
{
	int err;

	/* ensure there are enough low bits for flags in timer->base pointer */
	BUILD_BUG_ON(__alignof__(struct tvec_base) & TIMER_FLAG_MASK);

/* IAMROOT-12:
 * -------------
 * 현재 cpu에 대한 lowres 타이머 초기화 
 * (현재 cpu만 곧장 notifier 함수를 직접 호출한다)
 */
	err = timer_cpu_notify(&timers_nb, (unsigned long)CPU_UP_PREPARE,
			       (void *)(long)smp_processor_id());
	BUG_ON(err != NOTIFY_OK);

	init_timer_stats();

/* IAMROOT-12:
 * -------------
 * cpu 상태가 바뀔 때마다 호출되는 notifier 함수 등록 
 * (나머지 cpu가 on 될 때마다 해당 cpu의 lowres 타이머를 초기화한다)
 */
	register_cpu_notifier(&timers_nb);

/* IAMROOT-12:
 * -------------
 * lowres 타이머에 bottom half 처리를 softirq에서 처리한다.
 * (hires 타이머 동작 전까지는 사용)
 */
	open_softirq(TIMER_SOFTIRQ, run_timer_softirq);
}

/**
 * msleep - sleep safely even with waitqueue interruptions
 * @msecs: Time in milliseconds to sleep for
 */
void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout)
		timeout = schedule_timeout_uninterruptible(timeout);
}

EXPORT_SYMBOL(msleep);

/**
 * msleep_interruptible - sleep waiting for signals
 * @msecs: Time in milliseconds to sleep for
 */
unsigned long msleep_interruptible(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs) + 1;

	while (timeout && !signal_pending(current))
		timeout = schedule_timeout_interruptible(timeout);
	return jiffies_to_msecs(timeout);
}

EXPORT_SYMBOL(msleep_interruptible);

static int __sched do_usleep_range(unsigned long min, unsigned long max)
{
	ktime_t kmin;
	unsigned long delta;

	kmin = ktime_set(0, min * NSEC_PER_USEC);
	delta = (max - min) * NSEC_PER_USEC;
	return schedule_hrtimeout_range(&kmin, delta, HRTIMER_MODE_REL);
}

/**
 * usleep_range - Drop in replacement for udelay where wakeup is flexible
 * @min: Minimum time in usecs to sleep
 * @max: Maximum time in usecs to sleep
 */
void usleep_range(unsigned long min, unsigned long max)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	do_usleep_range(min, max);
}
EXPORT_SYMBOL(usleep_range);
