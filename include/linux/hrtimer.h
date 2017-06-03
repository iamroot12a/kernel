/*
 *  include/linux/hrtimer.h
 *
 *  hrtimers - High-resolution kernel timers
 *
 *   Copyright(C) 2005, Thomas Gleixner <tglx@linutronix.de>
 *   Copyright(C) 2005, Red Hat, Inc., Ingo Molnar
 *
 *  data type definitions, declarations, prototypes
 *
 *  Started by: Thomas Gleixner and Ingo Molnar
 *
 *  For licencing details see kernel-base/COPYING
 */
#ifndef _LINUX_HRTIMER_H
#define _LINUX_HRTIMER_H

#include <linux/rbtree.h>
#include <linux/ktime.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/percpu.h>
#include <linux/timer.h>
#include <linux/timerqueue.h>

struct hrtimer_clock_base;
struct hrtimer_cpu_base;

/*
 * Mode arguments of xxx_hrtimer functions:
 */
enum hrtimer_mode {
	HRTIMER_MODE_ABS = 0x0,		/* Time value is absolute */
	HRTIMER_MODE_REL = 0x1,		/* Time value is relative to now */
	HRTIMER_MODE_PINNED = 0x02,	/* Timer is bound to CPU */
	HRTIMER_MODE_ABS_PINNED = 0x02,
	HRTIMER_MODE_REL_PINNED = 0x03,
};

/*
 * Return values for the callback function
 */
enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART,	/* Timer must be restarted */
};

/*
 * Values to track state of the timer
 *
 * Possible states:
 *
 * 0x00		inactive
 * 0x01		enqueued into rbtree
 * 0x02		callback function running
 * 0x04		timer is migrated to another cpu
 *
 * Special cases:
 * 0x03		callback function running and enqueued
 *		(was requeued on another CPU)
 * 0x05		timer was migrated on CPU hotunplug
 *
 * The "callback function running and enqueued" status is only possible on
 * SMP. It happens for example when a posix timer expired and the callback
 * queued a signal. Between dropping the lock which protects the posix timer
 * and reacquiring the base lock of the hrtimer, another CPU can deliver the
 * signal and rearm the timer. We have to preserve the callback running state,
 * as otherwise the timer could be removed before the softirq code finishes the
 * the handling of the timer.
 *
 * The HRTIMER_STATE_ENQUEUED bit is always or'ed to the current state
 * to preserve the HRTIMER_STATE_CALLBACK in the above scenario. This
 * also affects HRTIMER_STATE_MIGRATE where the preservation is not
 * necessary. HRTIMER_STATE_MIGRATE is cleared after the timer is
 * enqueued on the new cpu.
 *
 * All state transitions are protected by cpu_base->lock.
 */
#define HRTIMER_STATE_INACTIVE	0x00
#define HRTIMER_STATE_ENQUEUED	0x01
#define HRTIMER_STATE_CALLBACK	0x02
#define HRTIMER_STATE_MIGRATE	0x04

/**
 * struct hrtimer - the basic hrtimer structure
 * @node:	timerqueue node, which also manages node.expires,
 *		the absolute expiry time in the hrtimers internal
 *		representation. The time is related to the clock on
 *		which the timer is based. Is setup by adding
 *		slack to the _softexpires value. For non range timers
 *		identical to _softexpires.
 * @_softexpires: the absolute earliest expiry time of the hrtimer.
 *		The time which was given as expiry time when the timer
 *		was armed.
 * @function:	timer expiry callback function
 * @base:	pointer to the timer base (per cpu and per clock)
 * @state:	state information (See bit values above)
 * @start_pid: timer statistics field to store the pid of the task which
 *		started the timer
 * @start_site:	timer statistics field to store the site where the timer
 *		was started
 * @start_comm: timer statistics field to store the name of the process which
 *		started the timer
 *
 * The hrtimer structure must be initialized by hrtimer_init()
 */

/* IAMROOT-12:
 * -------------
 * hrtimer에서 사용하는 타이머 구조체
 * (1개의 타이머당 1개의 hrtimer를 할당하여 사용한다.)
 *
 * 이 hrtimer가 RB 트리로 구성된 hrtimer_clock_base에 등록되어 운영된다.
 *
 * hrtimer_clock_base는 cpu x 4개의 clock 만큼 사용된다.
 * (예 rpi2: cpu가 4개이므로 총 16개의 hrtimer_clock_base가 사용된다.)
 */
struct hrtimer {
	struct timerqueue_node		node;
	ktime_t				_softexpires;
	enum hrtimer_restart		(*function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;
	unsigned long			state;
#ifdef CONFIG_TIMER_STATS
	int				start_pid;
	void				*start_site;
	char				start_comm[16];
#endif
};

/**
 * struct hrtimer_sleeper - simple sleeper structure
 * @timer:	embedded timer structure
 * @task:	task to wake up
 *
 * task is set to NULL, when the timer expires.
 */
struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};

/**
 * struct hrtimer_clock_base - the timer base for a specific clock
 * @cpu_base:		per cpu clock base
 * @index:		clock type index for per_cpu support when moving a
 *			timer to a base on another cpu.
 * @clockid:		clock id for per_cpu support
 * @active:		red black tree root node for the active timers
 * @resolution:		the resolution of the clock, in nanoseconds
 * @get_time:		function to retrieve the current time of the clock
 * @softirq_time:	the time when running the hrtimer queue in the softirq
 * @offset:		offset of this clock to the monotonic base
 */

/* IAMROOT-12:
 * -------------
 * active 
 *      타이머큐로 RB 트리로 구성된다.
 * offset 
 *      - monotic 타이머인 경우 항상 0
 *      - realtime 타이머인 경우 현재 시각이 설정되면 
 *        현재 시각 - 1970년 1월 1일을 나노초 단위로 대입한 offset 값이다.
 *      - boottime 타이머인 경우 suspend된 시간 총합 
 */
struct hrtimer_clock_base {
	struct hrtimer_cpu_base	*cpu_base;
	int			index;
	clockid_t		clockid;
	struct timerqueue_head	active;
	ktime_t			resolution;
	ktime_t			(*get_time)(void);
	ktime_t			softirq_time;
	ktime_t			offset;
};

enum  hrtimer_base_type {
	HRTIMER_BASE_MONOTONIC,
	HRTIMER_BASE_REALTIME,
	HRTIMER_BASE_BOOTTIME,
	HRTIMER_BASE_TAI,
	HRTIMER_MAX_CLOCK_BASES,
};

/*
 * struct hrtimer_cpu_base - the per cpu clock bases
 * @lock:		lock protecting the base and associated clock bases
 *			and timers
 * @cpu:		cpu number
 * @active_bases:	Bitfield to mark bases with active timers
 * @clock_was_set:	Indicates that clock was set from irq context.
 * @expires_next:	absolute time of the next event which was scheduled
 *			via clock_set_next_event()
 * @in_hrtirq:		hrtimer_interrupt() is currently executing
 * @hres_active:	State of high resolution mode
 * @hang_detected:	The last hrtimer interrupt detected a hang
 * @nr_events:		Total number of hrtimer interrupt events
 * @nr_retries:		Total number of hrtimer interrupt retries
 * @nr_hangs:		Total number of hrtimer interrupt hangs
 * @max_hang_time:	Maximum time spent in hrtimer_interrupt
 * @clock_base:		array of clock bases for this cpu
 */

/* IAMROOT-12:
 * -------------
 * active_bases
 *      4개의 비트로 각 비트는 클럭종류를 가리키며 해당 클럭에 타이머가 등록
 *      된 경우 1로 설정된다.
 */

struct hrtimer_cpu_base {
	raw_spinlock_t			lock;
	unsigned int			cpu;
	unsigned int			active_bases;
	unsigned int			clock_was_set;

/* IAMROOT-12:
 * -------------
 * high-res 타이머가 지원되는 h/w 타이머를 사용하는 경우
 */
#ifdef CONFIG_HIGH_RES_TIMERS

/* IAMROOT-12:
 * -------------
 * expires_next 
 *      4개의 시각중에서 가장 빠른 만료시각을 가진 hrtimer의 만료 시각으로 
 *      monotonic 기준으로 저장되어 있다. (각 클럭의 expires - offset)
 */
	ktime_t				expires_next;
	int				in_hrtirq;
	int				hres_active;
	int				hang_detected;
	unsigned long			nr_events;
	unsigned long			nr_retries;
	unsigned long			nr_hangs;
	ktime_t				max_hang_time;
#endif
	struct hrtimer_clock_base	clock_base[HRTIMER_MAX_CLOCK_BASES];
};

static inline void hrtimer_set_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = time;
	timer->_softexpires = time;
}

static inline void hrtimer_set_expires_range(struct hrtimer *timer, ktime_t time, ktime_t delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, delta);
}

static inline void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time, unsigned long delta)
{
/* IAMROOT-12:
 * -------------
 * _softexpires: 델타(slack)가 제외된 시각 
 * node.expires: 델타(slack)가 포함된 시각
 */
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, ns_to_ktime(delta));
}

static inline void hrtimer_set_expires_tv64(struct hrtimer *timer, s64 tv64)
{
	timer->node.expires.tv64 = tv64;
	timer->_softexpires.tv64 = tv64;
}

static inline void hrtimer_add_expires(struct hrtimer *timer, ktime_t time)
{
	timer->node.expires = ktime_add_safe(timer->node.expires, time);
	timer->_softexpires = ktime_add_safe(timer->_softexpires, time);
}

static inline void hrtimer_add_expires_ns(struct hrtimer *timer, u64 ns)
{
	timer->node.expires = ktime_add_ns(timer->node.expires, ns);
	timer->_softexpires = ktime_add_ns(timer->_softexpires, ns);
}

static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
/* IAMROOT-12:
 * -------------
 * RB 트리에 연결될 노드로 slack이 적용된 실제 만료 시간을 가지고 있다
 */
	return timer->node.expires;
}

static inline ktime_t hrtimer_get_softexpires(const struct hrtimer *timer)
{
/* IAMROOT-12:
 * -------------
 * slack이 적용되지 않은 사용자 요청 만료 시각
 */
	return timer->_softexpires;
}

static inline s64 hrtimer_get_expires_tv64(const struct hrtimer *timer)
{
	return timer->node.expires.tv64;
}
static inline s64 hrtimer_get_softexpires_tv64(const struct hrtimer *timer)
{
	return timer->_softexpires.tv64;
}

static inline s64 hrtimer_get_expires_ns(const struct hrtimer *timer)
{
	return ktime_to_ns(timer->node.expires);
}

static inline ktime_t hrtimer_expires_remaining(const struct hrtimer *timer)
{
	return ktime_sub(timer->node.expires, timer->base->get_time());
}

#ifdef CONFIG_HIGH_RES_TIMERS
struct clock_event_device;

extern void hrtimer_interrupt(struct clock_event_device *dev);

/*
 * In high resolution mode the time reference must be read accurate
 */
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->get_time();
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return timer->base->cpu_base->hres_active;
}

extern void hrtimer_peek_ahead_timers(void);

/*
 * The resolution of the clocks. The resolution value is returned in
 * the clock_getres() system call to give application programmers an
 * idea of the (in)accuracy of timers. Timer values are rounded up to
 * this resolution values.
 */
# define HIGH_RES_NSEC		1
# define KTIME_HIGH_RES		(ktime_t) { .tv64 = HIGH_RES_NSEC }
# define MONOTONIC_RES_NSEC	HIGH_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_HIGH_RES

extern void clock_was_set_delayed(void);

#else

# define MONOTONIC_RES_NSEC	LOW_RES_NSEC
# define KTIME_MONOTONIC_RES	KTIME_LOW_RES

static inline void hrtimer_peek_ahead_timers(void) { }

/*
 * In non high resolution mode the time reference is taken from
 * the base softirq time variable.
 */
static inline ktime_t hrtimer_cb_get_time(struct hrtimer *timer)
{
	return timer->base->softirq_time;
}

static inline int hrtimer_is_hres_active(struct hrtimer *timer)
{
	return 0;
}

static inline void clock_was_set_delayed(void) { }

#endif

extern void clock_was_set(void);
#ifdef CONFIG_TIMERFD
extern void timerfd_clock_was_set(void);
#else
static inline void timerfd_clock_was_set(void) { }
#endif
extern void hrtimers_resume(void);

DECLARE_PER_CPU(struct tick_device, tick_cpu_device);


/* Exported timer functions: */

/* Initialize timers: */
extern void hrtimer_init(struct hrtimer *timer, clockid_t which_clock,
			 enum hrtimer_mode mode);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
extern void hrtimer_init_on_stack(struct hrtimer *timer, clockid_t which_clock,
				  enum hrtimer_mode mode);

extern void destroy_hrtimer_on_stack(struct hrtimer *timer);
#else
static inline void hrtimer_init_on_stack(struct hrtimer *timer,
					 clockid_t which_clock,
					 enum hrtimer_mode mode)
{
	hrtimer_init(timer, which_clock, mode);
}
static inline void destroy_hrtimer_on_stack(struct hrtimer *timer) { }
#endif

/* Basic timer operations: */
extern int hrtimer_start(struct hrtimer *timer, ktime_t tim,
			 const enum hrtimer_mode mode);
extern int hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			unsigned long range_ns, const enum hrtimer_mode mode);
extern int
__hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
			 unsigned long delta_ns,
			 const enum hrtimer_mode mode, int wakeup);

extern int hrtimer_cancel(struct hrtimer *timer);
extern int hrtimer_try_to_cancel(struct hrtimer *timer);

static inline int hrtimer_start_expires(struct hrtimer *timer,
						enum hrtimer_mode mode)
{
	unsigned long delta;
	ktime_t soft, hard;

/* IAMROOT-12:
 * -------------
 * soft: slack 적용 전 만료 시각
 * hard: slack 적용 후 만료 시각
 */
	soft = hrtimer_get_softexpires(timer);
	hard = hrtimer_get_expires(timer);
	delta = ktime_to_ns(ktime_sub(hard, soft));
	return hrtimer_start_range_ns(timer, soft, delta, mode);
}

static inline int hrtimer_restart(struct hrtimer *timer)
{
	return hrtimer_start_expires(timer, HRTIMER_MODE_ABS);
}

/* Query timers: */
extern ktime_t hrtimer_get_remaining(const struct hrtimer *timer);
extern int hrtimer_get_res(const clockid_t which_clock, struct timespec *tp);

extern ktime_t hrtimer_get_next_event(void);

/*
 * A timer is active, when it is enqueued into the rbtree or the
 * callback function is running or it's in the state of being migrated
 * to another cpu.
 */
static inline int hrtimer_active(const struct hrtimer *timer)
{
	return timer->state != HRTIMER_STATE_INACTIVE;
}

/*
 * Helper function to check, whether the timer is on one of the queues
 */
static inline int hrtimer_is_queued(struct hrtimer *timer)
{
/* IAMROOT-12:
 * -------------
 * state에 큐잉 여부를 본다.
 */
	return timer->state & HRTIMER_STATE_ENQUEUED;
}

/*
 * Helper function to check, whether the timer is running the callback
 * function
 */
static inline int hrtimer_callback_running(struct hrtimer *timer)
{

/* IAMROOT-12:
 * -------------
 * hrtimer가 만료되어 현재 callback 함수를 호출하는 중 여부를 반환한다.
 */
	return timer->state & HRTIMER_STATE_CALLBACK;
}

/* Forward a hrtimer so it expires after now: */
extern u64
hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval);

/* Forward a hrtimer so it expires after the hrtimer's current now */
static inline u64 hrtimer_forward_now(struct hrtimer *timer,
				      ktime_t interval)
{
	return hrtimer_forward(timer, timer->base->get_time(), interval);
}

/* Precise sleep: */
extern long hrtimer_nanosleep(struct timespec *rqtp,
			      struct timespec __user *rmtp,
			      const enum hrtimer_mode mode,
			      const clockid_t clockid);
extern long hrtimer_nanosleep_restart(struct restart_block *restart_block);

extern void hrtimer_init_sleeper(struct hrtimer_sleeper *sl,
				 struct task_struct *tsk);

extern int schedule_hrtimeout_range(ktime_t *expires, unsigned long delta,
						const enum hrtimer_mode mode);
extern int schedule_hrtimeout_range_clock(ktime_t *expires,
		unsigned long delta, const enum hrtimer_mode mode, int clock);
extern int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);

/* Soft interrupt function to run the hrtimer queues: */
extern void hrtimer_run_queues(void);
extern void hrtimer_run_pending(void);

/* Bootup initialization: */
extern void __init hrtimers_init(void);

/* Show pending timers: */
extern void sysrq_timer_list_show(void);

#endif
