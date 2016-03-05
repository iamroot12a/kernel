#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 *
 * Using a single mcs node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 */

/* IAMROOT-12AB:
 * -------------
 * osq_node라는 이름으로 per-cpu 데이터를 선언한다.
 *	- optimistic spin에서 사용
 */

static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline int encode_cpu(int cpu_nr)
{
	return cpu_nr + 1;
}

/* IAMROOT-12AB:
 * -------------
 * 인수로 전달 받은 encode_cpu_val 번호(1부터 시작하는)에서 -1을 한 cpu 번호로
 * osq_node라는 per-cpu 데이터의 주소를 리턴 
 */
static inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val)
{
	int cpu_nr = encoded_cpu_val - 1;

	return per_cpu_ptr(&osq_node, cpu_nr);
}

/*
 * Get a stable @node->next pointer, either for unlock() or unqueue() purposes.
 * Can return NULL in case we were the last queued and we updated @lock instead.
 */
static inline struct optimistic_spin_node *
osq_wait_next(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      struct optimistic_spin_node *prev)
{
	struct optimistic_spin_node *next = NULL;
	int curr = encode_cpu(smp_processor_id());
	int old;

	/*
	 * If there is a prev node in queue, then the 'old' value will be
	 * the prev node's CPU #, else it's set to OSQ_UNLOCKED_VAL since if
	 * we're currently last in queue, then the queue will then become empty.
	 */
	old = prev ? prev->cpu : OSQ_UNLOCKED_VAL;

	for (;;) {

/* IAMROOT-12AB:
 * -------------
 * 현재 cpu가 OSQ의 꼬리이면 prev cpu 번호로 바꾸고 성공 시 break;
 * 내가 마지막이므로 next는 null로 리턴
 */
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg(&lock->tail, curr, old) == curr) {
			/*
			 * We were the last queued, we moved @lock back. @prev
			 * will now observe @lock and will complete its
			 * unlock()/unqueue().
			 */
			break;
		}

		/*
		 * We must xchg() the @node->next value, because if we were to
		 * leave it in, a concurrent unlock()/unqueue() from
		 * @node->next might complete Step-A and think its @prev is
		 * still valid.
		 *
		 * If the concurrent unlock()/unqueue() wins the race, we'll
		 * wait for either @lock to point to us, through its Step-B, or
		 * wait for a new @node->next from its Step-C.
		 */

/* IAMROOT-12AB:
 * -------------
 * node->next가 null인 경우는 내 앞에 노드의 cpu가 unqueue가 진행되고 있는
 * 순간이므로 spin을 한다. 그렇지 않고 값이 있는 경우는 안정화가 되었다고
 * 판단해서 null을 입력하여 다음 노드와의 연결을 끊는다.
 */
		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next)
				break;
		}

		cpu_relax_lowlatency();
	}

/* IAMROOT-12AB:
 * -------------
 * 내 뒤에 연결되었었던 노드가 리턴된다.
 */
	return next;
}

bool osq_lock(struct optimistic_spin_queue *lock)
{

/* IAMROOT-12AB:
 * -------------
 * osq_node: per-cpu 데이터 구조 사용
 */
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	struct optimistic_spin_node *prev, *next;

/* IAMROOT-12AB:
 * -------------
 * curr = 현재 cpu + 1
 */
	int curr = encode_cpu(smp_processor_id());
	int old;

	node->locked = 0;
	node->next = NULL;
	node->cpu = curr;

/* IAMROOT-12AB:
 * -------------
 * &lock->tail에 curr(현재 cpu + 1)을 대입하고 기존에 기록되어 있던 old 값을
 * 알아온다. 
 */
	old = atomic_xchg(&lock->tail, curr);

/* IAMROOT-12AB:
 * -------------
 * OSQ(MCS) lock이 unlock 상태이면, 즉 처음으로 OSQ에 진입한 경우 lock을 획득했기
 * 때문에 true로 빠져나간다.
 */
	if (old == OSQ_UNLOCKED_VAL)
		return true;

/* IAMROOT-12AB:
 * -------------
 * prev는 OSQ 에서 가장 마지막에 있는 노드
 */
	prev = decode_cpu(old);
	node->prev = prev;
	ACCESS_ONCE(prev->next) = node;

	/*
	 * Normally @prev is untouchable after the above store; because at that
	 * moment unlock can proceed and wipe the node element from stack.
	 *
	 * However, since our nodes are static per-cpu storage, we're
	 * guaranteed their existence -- this allows us to apply
	 * cmpxchg in an attempt to undo our queueing.
	 */

/* IAMROOT-12AB:
 * -------------
 * node->locked가 1이 되는 경우는 선두 노드 OSQ에서 빠져나가는 경우(osq_unlock)
 * 이 때 osq spin을 빠져나가고 마지막으로 mutex 얻기 위해 owner와 경쟁한다. 
 * (owner가 release할 때 mutex를 얻게된다.)
 */
	while (!ACCESS_ONCE(node->locked)) {
		/*
		 * If we need to reschedule bail... so we can block.
		 */

/* IAMROOT-12AB:
 * -------------
 * 현재 태스크보다 높은 우선 순위의 태스크가 요청을 하는 경우에는 midpath를 
 * 포기하고 unqueue 한다.
 */
		if (need_resched())
			goto unqueue;

/* IAMROOT-12AB:
 * -------------
 * Cortex-A7은 barrier()만 호출
 */
		cpu_relax_lowlatency();
	}
	return true;

unqueue:
	/*
	 * Step - A  -- stabilize @prev
	 *
	 * Undo our @prev->next assignment; this will make @prev's
	 * unlock()/unqueue() wait for a next pointer since @lock points to us
	 * (or later).
	 */

/* IAMROOT-12AB:
 * -------------
 * prev->next 가 현재 노드를 가리킬 때 null을 집어 넣어 단절을 시키되,
 * 결과 값이(prev->next의 old 값) 현재 노드인 경우 즉, 리스트 구조에서
 * 내 앞 노드가 unlink를 시도 하지 않고 있는 상태(변동이 없는 상태)만
 * 성공하여 break 된다. 
 */
	for (;;) {
		if (prev->next == node &&
		    cmpxchg(&prev->next, node, NULL) == node)
			break;

		/*
		 * We can only fail the cmpxchg() racing against an unlock(),
		 * in which case we should observe @node->locked becomming
		 * true.
		 */

/* IAMROOT-12AB:
 * -------------
 * OSQ_lock을 포기하고 나가려고 하는 상황에서 위 루틴이 실패해서 spin 하는 상황인데
 * 다시 한 번 node->locked=1인지 보고(OSQ lock 획득이 가능한지 확인)
 * 획득 가능하면 OSQ lock을 획득하고 리턴한다.
 */
		if (smp_load_acquire(&node->locked))
			return true;

		cpu_relax_lowlatency();

		/*
		 * Or we race against a concurrent unqueue()'s step-B, in which
		 * case its step-C will write us a new @node->prev pointer.
		 */
		prev = ACCESS_ONCE(node->prev);
	}

	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */

/* IAMROOT-12AB:
 * -------------
 * next가 null인 경우는 자신이 마지막 노드일 때만 null이므로 이 때는
 * prev <-----> next를 연결할 필요가 없다.
 */
	next = osq_wait_next(lock, node, prev);
	if (!next)
		return false;

	/*
	 * Step - C -- unlink
	 *
	 * @prev is stable because its still waiting for a new @prev->next
	 * pointer, @next is stable because our @node->next pointer is NULL and
	 * it will wait in Step-A.
	 */

/* IAMROOT-12AB:
 * -------------
 * 내 뒤 노드가 있기 때문에 prev <----> next를 연결해준다.
 */
	ACCESS_ONCE(next->prev) = prev;
	ACCESS_ONCE(prev->next) = next;

	return false;
}

void osq_unlock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next;
	int curr = encode_cpu(smp_processor_id());

	/*
	 * Fast path for the uncontended case.
	 */

/* IAMROOT-12AB:
 * -------------
 * 내가 마지막 노드인지 확인하여 마지막인 경우 lock->tail 에 0을 기록하여 
 * 더 이상 osq에 대기 태스크가 없음을 알린다.
 */
	if (likely(atomic_cmpxchg(&lock->tail, curr, OSQ_UNLOCKED_VAL) == curr))
		return;

	/*
	 * Second most likely case.
	 */

/* IAMROOT-12AB:
 * -------------
 * 이 루틴에 진입한 경우는 2개 이상의 노드가 존재하는 경우이다.
 * 현재 노드는 osq의 선두이다. 따라서 node->next에 null을 집어 넣어 연결을 끊는다.
 * 다음 노드 즉, 차선으로 대기하고 있던 노드의 locked 멤버변수에 1을 집어넣어
 * osq lock spin을 빠져나올 수 있게 도와준다.(선두에서 spin)
 */
	node = this_cpu_ptr(&osq_node);
	next = xchg(&node->next, NULL);
	if (next) {
		ACCESS_ONCE(next->locked) = 1;
		return;
	}

/* IAMROOT-12AB:
 * -------------
 * 2개 이상의 노드가 osq에 있었는데 next가 없는 경우는 next 노드에 대한 안정화가
 * 완료되지 않았던 것을 의미한다. (안정화: 리스트 연결이 확정) 
 * 이 때 node->next에 연결이 생길때 까지 대기하였다가 null을 집어 넣게 된다.
 * 그런 후 그 다음 노드의 locked 멤버변수에 1을 집어 넣어 osq lock spin을 
 * 빠져나올 수 있게 한다.
 */
	next = osq_wait_next(lock, node, NULL);
	if (next)
		ACCESS_ONCE(next->locked) = 1;
}
