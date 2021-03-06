#ifndef __LINUX_OSQ_LOCK_H
#define __LINUX_OSQ_LOCK_H

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 */

/* IAMROOT-12AB:
 * -------------
 * *next *prev: optimistics_spin_node를 리스트로 연결해준다.
 * locked: 초기값은 0이고, 1을 설정하는 경우 osq_lock()을 빠져나가게 한다.
 *         즉, OSQ lock을 획득하고 mutex 소유자가 release 할 때까지 spin.(경쟁)
 */

struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev;
	int locked; /* 1 if lock acquired */
	int cpu; /* encoded CPU # + 1 value */
};


/* IAMROOT-12AB:
 * -------------
 * optimistic_spin_node들이 연결되어 있을 때 가장 마지막에 위치한 노드의
 * cpu 값을 가지고 있다. 즉. last 노드에 대한 cpu 번호(based 1)를 담고 있다.
 */
struct optimistic_spin_queue {
	/*
	 * Stores an encoded value of the CPU # of the tail node in the queue.
	 * If the queue is empty, then it's set to OSQ_UNLOCKED_VAL.
	 */
	atomic_t tail;
};


/* IAMROOT-12AB:
 * -------------
 * osq에 대기자가 없는 경우로 unlock 상태이다.
 */
#define OSQ_UNLOCKED_VAL (0)

/* Init macro and function. */
#define OSQ_LOCK_UNLOCKED { ATOMIC_INIT(OSQ_UNLOCKED_VAL) }

static inline void osq_lock_init(struct optimistic_spin_queue *lock)
{
	atomic_set(&lock->tail, OSQ_UNLOCKED_VAL);
}

extern bool osq_lock(struct optimistic_spin_queue *lock);
extern void osq_unlock(struct optimistic_spin_queue *lock);

#endif
