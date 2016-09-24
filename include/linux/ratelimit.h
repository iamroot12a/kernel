#ifndef _LINUX_RATELIMIT_H
#define _LINUX_RATELIMIT_H

#include <linux/param.h>
#include <linux/spinlock.h>

#define DEFAULT_RATELIMIT_INTERVAL	(5 * HZ)
#define DEFAULT_RATELIMIT_BURST		10

struct ratelimit_state {
	raw_spinlock_t	lock;		/* protect the state */

	int		interval;
	int		burst;
	int		printed;
	int		missed;
	unsigned long	begin;
};

/* IAMROOT-12AB:
 * -------------
 * static하게 ratelimit_state 구조체를 선언하고 초기화한다.
 */
#define RATELIMIT_STATE_INIT(name, interval_init, burst_init) {		\
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(name.lock),	\
		.interval	= interval_init,			\
		.burst		= burst_init,				\
	}

#define RATELIMIT_STATE_INIT_DISABLED					\
	RATELIMIT_STATE_INIT(ratelimit_state, 0, DEFAULT_RATELIMIT_BURST)

/* IAMROOT-12AB:
 * -------------
 * interval_init 내에 burst_init 만큼 출력을 허용하는 
 * ratelimit_state 구조체를 선언한다.
 */
#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init)		\
									\
	struct ratelimit_state name =					\
		RATELIMIT_STATE_INIT(name, interval_init, burst_init)	\

/* IAMROOT-12AB:
 * -------------
 * 동적으로 ratelimit_state 초기화한다.
 */
static inline void ratelimit_state_init(struct ratelimit_state *rs,
					int interval, int burst)
{
	raw_spin_lock_init(&rs->lock);
	rs->interval = interval;
	rs->burst = burst;
	rs->printed = 0;
	rs->missed = 0;
	rs->begin = 0;
}

extern struct ratelimit_state printk_ratelimit_state;


/* IAMROOT-12AB:
 * -------------
 * state에서 지정한 interval 이내에 burst만큼의 호출을 초과한 경우 
 * missed가 증가되고 함수는 0을 반환한다.
 * (지정된 interval을 초과하는 경우 카운터는 다시 리셋된다.)
 */
extern int ___ratelimit(struct ratelimit_state *rs, const char *func);
#define __ratelimit(state) ___ratelimit(state, __func__)

#ifdef CONFIG_PRINTK

#define WARN_ON_RATELIMIT(condition, state)			\
		WARN_ON((condition) && __ratelimit(state))

#define WARN_RATELIMIT(condition, format, ...)			\
({								\
	static DEFINE_RATELIMIT_STATE(_rs,			\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);	\
	int rtn = !!(condition);				\
								\
	if (unlikely(rtn && __ratelimit(&_rs)))			\
		WARN(rtn, format, ##__VA_ARGS__);		\
								\
	rtn;							\
})

#else

#define WARN_ON_RATELIMIT(condition, state)			\
	WARN_ON(condition)

#define WARN_RATELIMIT(condition, format, ...)			\
({								\
	int rtn = WARN(condition, format, ##__VA_ARGS__);	\
	rtn;							\
})

#endif

#endif /* _LINUX_RATELIMIT_H */
