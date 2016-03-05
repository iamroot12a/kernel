/*
 * include/asm-generic/mutex-dec.h
 *
 * Generic implementation of the mutex fastpath, based on atomic
 * decrement/increment.
 */
#ifndef _ASM_GENERIC_MUTEX_DEC_H
#define _ASM_GENERIC_MUTEX_DEC_H

/**
 *  __mutex_fastpath_lock - try to take the lock by moving the count
 *                          from 1 to a 0 value
 *  @count: pointer of type atomic_t
 *  @fail_fn: function to call if the original value was not 1
 *
 * Change the count from 1 to a value lower than 1, and call <fail_fn> if
 * it wasn't 1 originally. This function MUST leave the value lower than
 * 1 even when the "1" assertion wasn't true.
 */
static inline void
__mutex_fastpath_lock(atomic_t *count, void (*fail_fn)(atomic_t *))
{

/* IAMROOT-12A:
 * ------------
 * atomic 하게 lock count를 1 감소 시키고 감소 시킨 결과를 return 한다.
 * 만일 감소 시킨 결과가 0보다 작다면 fastpath가 성립되지 않아
 * __mutex_lock_slowpath() 함수를 호출하게 된다.
 *
 * 결국 성공 조건은 lock count가 1(unlock)일 때만 가능하다.
 * lock count: 1(unlock), 0(lock), -1(-n, lock with waiter)
 *
 * unlikely를 사용한 이유:
 *      fastpath가 성립되는 경우 compiler optimization에 현재 코드 근처에
 *      만들어지는 코드를 수행하게 하여 cache 효율성을 높인다.
 */
	if (unlikely(atomic_dec_return(count) < 0))
		fail_fn(count);
}

/**
 *  __mutex_fastpath_lock_retval - try to take the lock by moving the count
 *                                 from 1 to a 0 value
 *  @count: pointer of type atomic_t
 *
 * Change the count from 1 to a value lower than 1. This function returns 0
 * if the fastpath succeeds, or -1 otherwise.
 */
static inline int
__mutex_fastpath_lock_retval(atomic_t *count)
{
	if (unlikely(atomic_dec_return(count) < 0))
		return -1;
	return 0;
}

/**
 *  __mutex_fastpath_unlock - try to promote the count from 0 to 1
 *  @count: pointer of type atomic_t
 *  @fail_fn: function to call if the original value was not 0
 *
 * Try to promote the count from 0 to 1. If it wasn't 0, call <fail_fn>.
 * In the failure case, this function is allowed to either set the value to
 * 1, or to set it to a value lower than 1.
 *
 * If the implementation sets it to a value of lower than 1, then the
 * __mutex_slowpath_needs_to_unlock() macro needs to return 1, it needs
 * to return 0 otherwise.
 */
static inline void
__mutex_fastpath_unlock(atomic_t *count, void (*fail_fn)(atomic_t *))
{
/* IAMROOT-12AB:
 * -------------
 * 적은 확률로 count가 1(unlock)인 경우는 더 이상 후속 처리작업이 필요 없기 때문에
 * atomic operation만으로 끝난다.
 *
 * 하지만 0이하인 경우 후속 처리 작업을 진행하기 위해 __mutex_unlock_slowpath()함수로
 * 진입한다.
 */
	if (unlikely(atomic_inc_return(count) <= 0))
		fail_fn(count);
}

#define __mutex_slowpath_needs_to_unlock()		1

/**
 * __mutex_fastpath_trylock - try to acquire the mutex, without waiting
 *
 *  @count: pointer of type atomic_t
 *  @fail_fn: fallback function
 *
 * Change the count from 1 to a value lower than 1, and return 0 (failure)
 * if it wasn't 1 originally, or return 1 (success) otherwise. This function
 * MUST leave the value lower than 1 even when the "1" assertion wasn't true.
 * Additionally, if the value was < 0 originally, this function must not leave
 * it to 0 on failure.
 *
 * If the architecture has no effective trylock variant, it should call the
 * <fail_fn> spinlock-based trylock variant unconditionally.
 */
static inline int
__mutex_fastpath_trylock(atomic_t *count, int (*fail_fn)(atomic_t *))
{
	if (likely(atomic_cmpxchg(count, 1, 0) == 1))
		return 1;
	return 0;
}

#endif
