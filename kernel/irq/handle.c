/*
 * linux/kernel/irq/handle.c
 *
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code.
 *
 * Detailed information is available in Documentation/DocBook/genericirq
 *
 */

#include <linux/irq.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>

#include <trace/events/irq.h>

#include "internals.h"

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @irq:       the interrupt number
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
void handle_bad_irq(unsigned int irq, struct irq_desc *desc)
{
/* IAMROOT-12:
 * -------------
 * 등록(setup)되지 않은 인터럽트가 호출된 경우 irq 디스크립터 내용을 출력하고
 * 인터럽트 호출 카운터를 1 증가시킨다.
 */
	print_irq_desc(irq, desc);
	kstat_incr_irqs_this_cpu(irq, desc);
	ack_bad_irq(irq);
}

/*
 * Special, empty irq handler:
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;
}
EXPORT_SYMBOL_GPL(no_action);

static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))
		return;

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "
	       "but no thread function available.", irq, action->name);
}

void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action)
{
	/*
	 * In case the thread crashed and was killed we just pretend that
	 * we handled the interrupt. The hardirq handler has disabled the
	 * device interrupt, so no irq storm is lurking.
	 */
	if (action->thread->flags & PF_EXITING)
		return;

	/*
	 * Wake up the handler thread for this action. If the
	 * RUNTHREAD bit is already set, nothing to do.
	 */
	if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		return;

	/*
	 * It's safe to OR the mask lockless here. We have only two
	 * places which write to threads_oneshot: This code and the
	 * irq thread.
	 *
	 * This code is the hard irq context and can never run on two
	 * cpus in parallel. If it ever does we have more serious
	 * problems than this bitmask.
	 *
	 * The irq threads of this irq which clear their "running" bit
	 * in threads_oneshot are serialized via desc->lock against
	 * each other and they are serialized against this code by
	 * IRQS_INPROGRESS.
	 *
	 * Hard irq handler:
	 *
	 *	spin_lock(desc->lock);
	 *	desc->state |= IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *	set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
	 *	desc->threads_oneshot |= mask;
	 *	spin_lock(desc->lock);
	 *	desc->state &= ~IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *
	 * irq thread:
	 *
	 * again:
	 *	spin_lock(desc->lock);
	 *	if (desc->state & IRQS_INPROGRESS) {
	 *		spin_unlock(desc->lock);
	 *		while(desc->state & IRQS_INPROGRESS)
	 *			cpu_relax();
	 *		goto again;
	 *	}
	 *	if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
	 *		desc->threads_oneshot &= ~mask;
	 *	spin_unlock(desc->lock);
	 *
	 * So either the thread waits for us to clear IRQS_INPROGRESS
	 * or we are waiting in the flow handler for desc->lock to be
	 * released before we reach this point. The thread also checks
	 * IRQTF_RUNTHREAD under desc->lock. If set it leaves
	 * threads_oneshot untouched and runs the thread another time.
	 */
	desc->threads_oneshot |= action->thread_mask;

	/*
	 * We increment the threads_active counter in case we wake up
	 * the irq thread. The irq thread decrements the counter when
	 * it returns from the handler or in the exit path and wakes
	 * up waiters which are stuck in synchronize_irq() when the
	 * active count becomes zero. synchronize_irq() is serialized
	 * against this code (hard irq handler) via IRQS_INPROGRESS
	 * like the finalize_oneshot() code. See comment above.
	 */
	atomic_inc(&desc->threads_active);

	wake_up_process(action->thread);
}

irqreturn_t
handle_irq_event_percpu(struct irq_desc *desc, struct irqaction *action)
{
	irqreturn_t retval = IRQ_NONE;
	unsigned int flags = 0, irq = desc->irq_data.irq;

	do {
		irqreturn_t res;

		trace_irq_handler_entry(irq, action);

/* IAMROOT-12:
 * -------------
 * 2차 action 인터럽트 핸들러를 호출한다. 
 * (스레드 타입인 경우 top-half 처리 로직만 호출한다)
 */
		res = action->handler(irq, action->dev_id);
		trace_irq_handler_exit(irq, action, res);

		if (WARN_ONCE(!irqs_disabled(),"irq %u handler %pF enabled interrupts\n",
			      irq, action->handler))
			local_irq_disable();

		switch (res) {
		case IRQ_WAKE_THREAD:
			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */

/* IAMROOT-12:
 * -------------
 * bottom-half 처리를 위한 스레드 함수가 없는 경우 경고 메시지와 함께
 * 루프를 탈출한다.
 */
			if (unlikely(!action->thread_fn)) {
				warn_no_thread(irq, action);
				break;
			}

/* IAMROOT-12:
 * -------------
 * 스레드 타입인 경우 bottom-half 처리를 위해 해당 스레드를 깨운다.
 */
			__irq_wake_thread(desc, action);

			/* Fall through to add to randomness */
		case IRQ_HANDLED:
			flags |= action->flags;
			break;

		default:
			break;
		}

		retval |= res;

/* IAMROOT-12:
 * -------------
 * 2차 action 인터럽트 핸들러는 1개 이상의 핸들러를 가질 수 있다.
 * (1개의 인터럽트에 대해 여러 개의 디바이스 핸들러가 동작할 수 있다.)
 */
		action = action->next;
	} while (action);

/* IAMROOT-12:
 * -------------
 * 인터럽트 latency를 이용해서 랜덤값을 더 랜덤답게 만들어준다. ^^;
 */
	add_interrupt_randomness(irq, flags);

	if (!noirqdebug)
		note_interrupt(irq, desc, retval);
	return retval;
}

irqreturn_t handle_irq_event(struct irq_desc *desc)
{
	struct irqaction *action = desc->action;
	irqreturn_t ret;

	desc->istate &= ~IRQS_PENDING;
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	raw_spin_unlock(&desc->lock);

/* IAMROOT-12:
 * -------------
 * irq 처리중 inprogress 상태 플래그가 설정된다.
 */
	ret = handle_irq_event_percpu(desc, action);

	raw_spin_lock(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	return ret;
}
