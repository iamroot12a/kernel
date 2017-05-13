#ifndef LINUX_PREEMPT_MASK_H
#define LINUX_PREEMPT_MASK_H

#include <linux/preempt.h>

/*
 * We put the hardirq and softirq counter into the preemption
 * counter. The bitmask has the following meaning:
 *
 * - bits 0-7 are the preemption count (max preemption depth: 256)
 * - bits 8-15 are the softirq count (max # of softirqs: 256)
 *
 * The hardirq count could in theory be the same as the number of
 * interrupts in the system, but we run all interrupt handlers with
 * interrupts disabled, so we cannot have nesting interrupts. Though
 * there are a few palaeontologic drivers which reenable interrupts in
 * the handler, so we need more than one bit here.
 *
 * PREEMPT_MASK:	0x000000ff
 * SOFTIRQ_MASK:	0x0000ff00
 * HARDIRQ_MASK:	0x000f0000
 *     NMI_MASK:	0x00100000
 * PREEMPT_ACTIVE:	0x00200000
 */
#define PREEMPT_BITS	8
#define SOFTIRQ_BITS	8
#define HARDIRQ_BITS	4
#define NMI_BITS	1

/* IAMROOT-12:
 * -------------
 * PREEMPT_SHIFT:           0
 * SOFTIRQ_SHIFT:           8
 * HARDIRQ_SHIFT:           16
 * NMI_SHIFT:               20
 * PREEMPT_ACTIVE_SHIFT:    21
 */
#define PREEMPT_SHIFT	0
#define SOFTIRQ_SHIFT	(PREEMPT_SHIFT + PREEMPT_BITS)
#define HARDIRQ_SHIFT	(SOFTIRQ_SHIFT + SOFTIRQ_BITS)
#define NMI_SHIFT	(HARDIRQ_SHIFT + HARDIRQ_BITS)

#define __IRQ_MASK(x)	((1UL << (x))-1)


/* IAMROOT-12:
 * -------------
 * PREEMPT_MASK:	0x000000ff
 * SOFTIRQ_MASK:	0x0000ff00
 * HARDIRQ_MASK:	0x000f0000
 *     NMI_MASK:	0x00100000
 * PREEMPT_ACTIVE_MASK:	0x00200000
 */
#define PREEMPT_MASK	(__IRQ_MASK(PREEMPT_BITS) << PREEMPT_SHIFT)
#define SOFTIRQ_MASK	(__IRQ_MASK(SOFTIRQ_BITS) << SOFTIRQ_SHIFT)
#define HARDIRQ_MASK	(__IRQ_MASK(HARDIRQ_BITS) << HARDIRQ_SHIFT)
#define NMI_MASK	(__IRQ_MASK(NMI_BITS)     << NMI_SHIFT)

/* IAMROOT-12:
 * -------------
 * PREEMPT_OFFSET:      0x0000_0001
 * SOFTIRQ_OFFSET:      0x0000_0100
 * HARDIRQ_OFFSET:      0x0001_0000
 * NMI_OFFSET:          0x0010_0000
 */

#define PREEMPT_OFFSET	(1UL << PREEMPT_SHIFT)
#define SOFTIRQ_OFFSET	(1UL << SOFTIRQ_SHIFT)
#define HARDIRQ_OFFSET	(1UL << HARDIRQ_SHIFT)
#define NMI_OFFSET	(1UL << NMI_SHIFT)

/* IAMROOT-12:
 * -------------
 * 아래 disable을 사용하는 경우 0x0000_0200과 같이 항상 짝수단위로 
 * 증가된다.
 */
#define SOFTIRQ_DISABLE_OFFSET	(2 * SOFTIRQ_OFFSET)

#define PREEMPT_ACTIVE_BITS	1
#define PREEMPT_ACTIVE_SHIFT	(NMI_SHIFT + NMI_BITS)
#define PREEMPT_ACTIVE	(__IRQ_MASK(PREEMPT_ACTIVE_BITS) << PREEMPT_ACTIVE_SHIFT)

#define hardirq_count()	(preempt_count() & HARDIRQ_MASK)
#define softirq_count()	(preempt_count() & SOFTIRQ_MASK)
#define irq_count()	(preempt_count() & (HARDIRQ_MASK | SOFTIRQ_MASK \
				 | NMI_MASK))

/*
 * Are we doing bottom half or hardware interrupt processing?
 * Are we in a softirq context? Interrupt context?
 * in_softirq - Are we currently processing softirq or have bh disabled?
 * in_serving_softirq - Are we currently processing softirq?
 */

/* IAMROOT-12:
 * -------------
 * in_irq():                top half(interrupt context)
 * in_softirq():            bottom half(softirq, bh_disable시 true)
 * in_interrupt():          위 두 가지 포함 + NMI 
 * in_serving_softirq():    softirq 처리 중 여부 (홀수에서만 true)
 * in_nmi():                nmi
 */
#define in_irq()		(hardirq_count())
#define in_softirq()		(softirq_count())
#define in_interrupt()		(irq_count())
#define in_serving_softirq()	(softirq_count() & SOFTIRQ_OFFSET)

/*
 * Are we in NMI context?
 */
#define in_nmi()	(preempt_count() & NMI_MASK)

#if defined(CONFIG_PREEMPT_COUNT)
# define PREEMPT_CHECK_OFFSET 1
#else
# define PREEMPT_CHECK_OFFSET 0
#endif

/*
 * The preempt_count offset needed for things like:
 *
 *  spin_lock_bh()
 *
 * Which need to disable both preemption (CONFIG_PREEMPT_COUNT) and
 * softirqs, such that unlock sequences of:
 *
 *  spin_unlock();
 *  local_bh_enable();
 *
 * Work as expected.
 */
#define SOFTIRQ_LOCK_OFFSET (SOFTIRQ_DISABLE_OFFSET + PREEMPT_CHECK_OFFSET)

/*
 * Are we running in atomic context?  WARNING: this macro cannot
 * always detect atomic context; in particular, it cannot know about
 * held spinlocks in non-preemptible kernels.  Thus it should not be
 * used in the general case to determine whether sleeping is possible.
 * Do not use in_atomic() in driver code.
 */
#define in_atomic()	((preempt_count() & ~PREEMPT_ACTIVE) != 0)

/*
 * Check whether we were atomic before we did preempt_disable():
 * (used by the scheduler, *after* releasing the kernel lock)
 */
#define in_atomic_preempt_off() \
		((preempt_count() & ~PREEMPT_ACTIVE) != PREEMPT_CHECK_OFFSET)

#ifdef CONFIG_PREEMPT_COUNT
# define preemptible()	(preempt_count() == 0 && !irqs_disabled())
#else
# define preemptible()	0
#endif

#endif /* LINUX_PREEMPT_MASK_H */
