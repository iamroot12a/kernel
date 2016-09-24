/*
 * ratelimit.c - Do something with rate limit.
 *
 * Isolated from kernel/printk.c by Dave Young <hidave.darkstar@gmail.com>
 *
 * 2008-05-01 rewrite the function and use a ratelimit_state data struct as
 * parameter. Now every user can use their own standalone ratelimit_state.
 *
 * This file is released under the GPLv2.
 */

#include <linux/ratelimit.h>
#include <linux/jiffies.h>
#include <linux/export.h>

/*
 * __ratelimit - rate limiting
 * @rs: ratelimit_state data
 * @func: name of calling function
 *
 * This enforces a rate limit: not more than @rs->burst callbacks
 * in every @rs->interval
 *
 * RETURNS:
 * 0 means callbacks will be suppressed.
 * 1 means go ahead and do it.
 */

/* IAMROOT-12AB:
 * -------------
 * 함수 결과가 1인 경우 계속 진행한다는 의미
 */
int ___ratelimit(struct ratelimit_state *rs, const char *func)
{
	unsigned long flags;
	int ret;

/* IAMROOT-12AB:
 * -------------
 * interval이 0으로 설정되면 제한 없이 계속 진행한다는 의미
 */
	if (!rs->interval)
		return 1;

	/*
	 * If we contend on this state's lock then almost
	 * by definition we are too busy to print a message,
	 * in addition to the one that will be printed by
	 * the entity that is holding the lock already:
	 */
	if (!raw_spin_trylock_irqsave(&rs->lock, flags))
		return 0;

/* IAMROOT-12AB:
 * -------------
 * begin이 0인 경우 bigin에 현재 jiffies를 기록한다.
 */
	if (!rs->begin)
		rs->begin = jiffies;

/* IAMROOT-12AB:
 * ------------ -
 * interval 이후에는 missed 카운트가 발생한 경우 정산하여 출력한다.
 * (출력시에 더 이상 호출을 포기한 callback 함수명과 missed 카운트를 나타낸다)
 */
	if (time_is_before_jiffies(rs->begin + rs->interval)) {
		if (rs->missed)
			printk(KERN_WARNING "%s: %d callbacks suppressed\n",
				func, rs->missed);
		rs->begin   = 0;
		rs->printed = 0;
		rs->missed  = 0;
	}

/* IAMROOT-12AB:
 * -------------
 * 정상 호출 시 printed가 증가되고, 그렇지 않은 경우 missed가 증가된다.
 */
	if (rs->burst && rs->burst > rs->printed) {
		rs->printed++;
		ret = 1;
	} else {
		rs->missed++;
		ret = 0;
	}
	raw_spin_unlock_irqrestore(&rs->lock, flags);

	return ret;
}
EXPORT_SYMBOL(___ratelimit);
