#ifndef _LINUX_BH_H
#define _LINUX_BH_H

#include <linux/preempt.h>
#include <linux/preempt_mask.h>

#ifdef CONFIG_TRACE_IRQFLAGS
extern void __local_bh_disable_ip(unsigned long ip, unsigned int cnt);
#else
static __always_inline void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
/* IAMROOT-12:
 * -------------
 * preempt 카운터를 cnt 만큼 증가시킨다.
 */
	preempt_count_add(cnt);
	barrier();
}
#endif

static inline void local_bh_disable(void)
{
/* IAMROOT-12:
 * -------------
 * interrupt context에서 직접 호출을 막는다. 그리고 raise_softirq() 함수를 
 * 호출하더라도 ksoftirqd 스레드를 깨우지 않으므로 softirq가 호출되지 않고 
 * pending 된다.
 *
 * 짝수 단위로 증가시킨다. 이러한 경우 
 *  in_softirq():           true를 반환한다.
 *  in_serving_softirq():   softirq 처리 중 여부를 반환한다
 */
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

extern void _local_bh_enable(void);
extern void __local_bh_enable_ip(unsigned long ip, unsigned int cnt);

static inline void local_bh_enable_ip(unsigned long ip)
{
	__local_bh_enable_ip(ip, SOFTIRQ_DISABLE_OFFSET);
}

static inline void local_bh_enable(void)
{
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

#endif /* _LINUX_BH_H */
