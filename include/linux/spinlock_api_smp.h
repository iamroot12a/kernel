#ifndef __LINUX_SPINLOCK_API_SMP_H
#define __LINUX_SPINLOCK_API_SMP_H

#ifndef __LINUX_SPINLOCK_H
# error "please don't include this file directly"
#endif

/*
 * include/linux/spinlock_api_smp.h
 *
 * spinlock API declarations on SMP (and debug)
 * (implemented in kernel/spinlock.c)
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 */

int in_lock_functions(unsigned long addr);

#define assert_raw_spin_locked(x)	BUG_ON(!raw_spin_is_locked(x))

/* IAMROOT-12A:
 * ------------
 * 아래 spin_lock 함수들은 inline되지 않는 함수들로 .spinlock.text 섹션에 저장된다.
 * __lockfunc: __attribute__((section(".spinlock.text")))
 */
void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)		__acquires(lock);
void __lockfunc _raw_spin_lock_nested(raw_spinlock_t *lock, int subclass)
								__acquires(lock);
void __lockfunc _raw_spin_lock_bh_nested(raw_spinlock_t *lock, int subclass)
								__acquires(lock);
void __lockfunc
_raw_spin_lock_nest_lock(raw_spinlock_t *lock, struct lockdep_map *map)
								__acquires(lock);
void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)		__acquires(lock);
void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
								__acquires(lock);

unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
								__acquires(lock);
unsigned long __lockfunc
_raw_spin_lock_irqsave_nested(raw_spinlock_t *lock, int subclass)
								__acquires(lock);
int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock);
int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock);
void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)		__releases(lock);
void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)	__releases(lock);
void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)	__releases(lock);
void __lockfunc
_raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
								__releases(lock);

/* IAMROOT-12A:
 * ------------
 * 아래 매크로는 코드 크기보다는 성능 향상을 바라는 목적으로
 * inline 함수를 호출하기 위한 spin_lock 매크로들
 */
#ifdef CONFIG_INLINE_SPIN_LOCK
#define _raw_spin_lock(lock) __raw_spin_lock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_BH
#define _raw_spin_lock_bh(lock) __raw_spin_lock_bh(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_IRQ
#define _raw_spin_lock_irq(lock) __raw_spin_lock_irq(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_LOCK_IRQSAVE
#define _raw_spin_lock_irqsave(lock) __raw_spin_lock_irqsave(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_TRYLOCK
#define _raw_spin_trylock(lock) __raw_spin_trylock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_TRYLOCK_BH
#define _raw_spin_trylock_bh(lock) __raw_spin_trylock_bh(lock)
#endif

#ifndef CONFIG_UNINLINE_SPIN_UNLOCK
#define _raw_spin_unlock(lock) __raw_spin_unlock(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_BH
#define _raw_spin_unlock_bh(lock) __raw_spin_unlock_bh(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_IRQ
#define _raw_spin_unlock_irq(lock) __raw_spin_unlock_irq(lock)
#endif

#ifdef CONFIG_INLINE_SPIN_UNLOCK_IRQRESTORE
#define _raw_spin_unlock_irqrestore(lock, flags) __raw_spin_unlock_irqrestore(lock, flags)
#endif

static inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
	preempt_disable();
	if (do_raw_spin_trylock(lock)) {
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
		return 1;
	}
	preempt_enable();
	return 0;
}

/*
 * If lockdep is enabled then we use the non-preemption spin-ops
 * even on CONFIG_PREEMPT, because lockdep assumes that interrupts are
 * not re-enabled during lock-acquire (which the preempt-spin-ops do):
 */

/* IAMROOT-12A:
 * ------------
 * CONFIG_GENERIC_LOCKBREAK:
 * 	이 방식은 raw_spinlock 구조체에 break_lock이라는 변수를 넣어 
 *      raw_spin_lock() 구현에서 추가된 내용으로 lock을 획득하지 못하고
 *      spinning 중인지에 대한 여부를 확인할 수 있는 field를 집어 넣었다.
 *      
 *      그러면서 int 하나면 구현되는 크기가 두 배로 커지는 문제도 있지만
 *      ticket이 구현되면서 추가로 spin_is_contended()라는 함수가 만들어
 *      lock이 spin중인지를 break_lock 변수 없이 알아낼 수 있게 되었다.
 *      따라서 이 커널 옵션은 필요 없어지게 되었다.
 *      2013년 12월 마지막으로 사용했었던 arm64 아키텍처 코드에서도
 *      삭제되면서 이제는 거의 필요 없어진 옵션이다.
 *
 * 참고: spinlock: lockbreak cleanups - https://lwn.net/Articles/241877/
 *       arm64: locks: Remove CONFIG_GENERIC_LOCKBREAK -
 * 	     http://marc.info/?l=git-commits-head&m=138418940913511&w=2
 */
#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)

static inline unsigned long __raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_disable();
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	/*
	 * On lockdep we dont want the hand-coded irq-enable of
	 * do_raw_spin_lock_flags() code, because lockdep assumes
	 * that interrupts are not re-enabled during lock-acquire:
	 */
#ifdef CONFIG_LOCKDEP
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
#else
	do_raw_spin_lock_flags(lock, &flags);
#endif
	return flags;
}

static inline void __raw_spin_lock_irq(raw_spinlock_t *lock)
{
	local_irq_disable();
	preempt_disable();
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

static inline void __raw_spin_lock_bh(raw_spinlock_t *lock)
{
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_LOCK_OFFSET);
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
/* IAMROOT-12A:
 * ------------
 * SMP + Ticket 방식의 raw_spin_lock 방식을 구현(최신 방식)
 *
 * 기존 방식의 SMP + LHP(Lock-Holder Preemption) 방식의 구현은
 *    	kernel/locking/spinlock.c에 구현되어 있음.
 *
 * preemption을 정지.
 */
	preempt_disable();

/* IAMROOT-12A:
 * ------------
 * lock 디버깅을 위한 코드
 */
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);

/* IAMROOT-12A:
 * ------------
 * 실제 lock을 수행하는 코드
 */
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}

#endif /* !CONFIG_GENERIC_LOCKBREAK || CONFIG_DEBUG_LOCK_ALLOC */

static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	preempt_enable();
}

static inline void __raw_spin_unlock_irqrestore(raw_spinlock_t *lock,
					    unsigned long flags)
{
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	local_irq_restore(flags);
	preempt_enable();
}

static inline void __raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	local_irq_enable();
	preempt_enable();
}

static inline void __raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	spin_release(&lock->dep_map, 1, _RET_IP_);
	do_raw_spin_unlock(lock);
	__local_bh_enable_ip(_RET_IP_, SOFTIRQ_LOCK_OFFSET);
}

static inline int __raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	__local_bh_disable_ip(_RET_IP_, SOFTIRQ_LOCK_OFFSET);
	if (do_raw_spin_trylock(lock)) {
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
		return 1;
	}
	__local_bh_enable_ip(_RET_IP_, SOFTIRQ_LOCK_OFFSET);
	return 0;
}

#include <linux/rwlock_api_smp.h>

#endif /* __LINUX_SPINLOCK_API_SMP_H */
