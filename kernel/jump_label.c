/*
 * jump label support
 *
 * Copyright (C) 2009 Jason Baron <jbaron@redhat.com>
 * Copyright (C) 2011 Peter Zijlstra <pzijlstr@redhat.com>
 *
 */
#include <linux/memory.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/err.h>
#include <linux/static_key.h>
#include <linux/jump_label_ratelimit.h>

#ifdef HAVE_JUMP_LABEL

/* mutex to protect coming/going of the the jump_label table */
static DEFINE_MUTEX(jump_label_mutex);

void jump_label_lock(void)
{
	mutex_lock(&jump_label_mutex);
}

void jump_label_unlock(void)
{
	mutex_unlock(&jump_label_mutex);
}

/* IAMROOT-12AB:
 * -------------
 * jump_entry의 key 값으로 비교하는 함수
 */
static int jump_label_cmp(const void *a, const void *b)
{
	const struct jump_entry *jea = a;
	const struct jump_entry *jeb = b;

	if (jea->key < jeb->key)
		return -1;

	if (jea->key > jeb->key)
		return 1;

	return 0;
}

/* IAMROOT-12AB:
 * -------------
 * __jump_table에 있는 엔트리들을 key 순으로 정렬한다.
 */
static void
jump_label_sort_entries(struct jump_entry *start, struct jump_entry *stop)
{
	unsigned long size;

	size = (((unsigned long)stop - (unsigned long)start)
					/ sizeof(struct jump_entry));
	sort(start, size, sizeof(struct jump_entry), jump_label_cmp, NULL);
}

static void jump_label_update(struct static_key *key, int enable);

/* IAMROOT-12AB:
 * -------------
 * static_key의 enabled가 0인 경우에만 1로 설정하고 해당 static key를 사용한
 * 조건 코드들을 모두 업데이트 한다.
 */
void static_key_slow_inc(struct static_key *key)
{
	STATIC_KEY_CHECK_USE();

/* IAMROOT-12AB:
 * -------------
 * key->enabled가 0이 아닐 때에만 증가시키고 함수를 빠져나간다.
 */
	if (atomic_inc_not_zero(&key->enabled))
		return;

/* IAMROOT-12AB:
 * -------------
 * key->enabled가 0인 상태에서 이 함수를 호출하는 코어들만 
 * racing 상태가 되므로 이를 처리하기 위해 동기화(lock)한다.
 */
	jump_label_lock();

/* IAMROOT-12AB:
 * -------------
 * 락을 걸고 다시 한 번 key->enabled가 0일때에만 update하게 한다.
 * 
 * - FALSE로 선언된 static_key의 경우 이 함수가 호출되면 jmp 코드를 생성한다.
 * - TRUE로 선언된 static_key의 경우 이 함수가 호출되면 nop 코드를 생성한다.
 *   (key->entries 1bit와 key->enabled가 서로 다른 상태의 경우 반대로 동작)
 */
	if (atomic_read(&key->enabled) == 0) {
		if (!jump_label_get_branch_default(key))
			jump_label_update(key, JUMP_LABEL_ENABLE);
		else
			jump_label_update(key, JUMP_LABEL_DISABLE);
	}
	atomic_inc(&key->enabled);
	jump_label_unlock();
}
EXPORT_SYMBOL_GPL(static_key_slow_inc);

/* IAMROOT-12AB:
 * -------------
 * static_key의 enabled가 1인 경우에만 0으로 설정하고 해당 static key를 사용한
 * 조건 코드들을 모두 업데이트 한다.
 */
static void __static_key_slow_dec(struct static_key *key,
		unsigned long rate_limit, struct delayed_work *work)
{

/* IAMROOT-12AB:
 * -------------
 * key->enabled를 감소시켜 0이되는 경우에만 lock을 획득하고 계속 진행한다.
 */
	if (!atomic_dec_and_mutex_lock(&key->enabled, &jump_label_mutex)) {
		WARN(atomic_read(&key->enabled) < 0,
		     "jump label: negative count!\n");
		return;
	}

/* IAMROOT-12AB:
 * -------------
 * key->enabled가 0이 된 core가 이 루틴을 수행하여 update 하게 한다.
 *
 * rate_limit가 0이 아닌 경우 key->enable 값을 1로 다시 변경하고 지연 시킨다.
 */
	if (rate_limit) {
		atomic_inc(&key->enabled);
		schedule_delayed_work(work, rate_limit);
	} else {
		if (!jump_label_get_branch_default(key))
			jump_label_update(key, JUMP_LABEL_DISABLE);
		else
			jump_label_update(key, JUMP_LABEL_ENABLE);
	}
	jump_label_unlock();
}

static void jump_label_update_timeout(struct work_struct *work)
{
	struct static_key_deferred *key =
		container_of(work, struct static_key_deferred, work.work);
	__static_key_slow_dec(&key->key, 0, NULL);
}

void static_key_slow_dec(struct static_key *key)
{
	STATIC_KEY_CHECK_USE();
	__static_key_slow_dec(key, 0, NULL);
}
EXPORT_SYMBOL_GPL(static_key_slow_dec);

void static_key_slow_dec_deferred(struct static_key_deferred *key)
{
	STATIC_KEY_CHECK_USE();
	__static_key_slow_dec(&key->key, key->timeout, &key->work);
}
EXPORT_SYMBOL_GPL(static_key_slow_dec_deferred);

void jump_label_rate_limit(struct static_key_deferred *key,
		unsigned long rl)
{
	STATIC_KEY_CHECK_USE();
	key->timeout = rl;
	INIT_DELAYED_WORK(&key->work, jump_label_update_timeout);
}
EXPORT_SYMBOL_GPL(jump_label_rate_limit);

static int addr_conflict(struct jump_entry *entry, void *start, void *end)
{
	if (entry->code <= (unsigned long)end &&
		entry->code + JUMP_LABEL_NOP_SIZE > (unsigned long)start)
		return 1;

	return 0;
}

static int __jump_label_text_reserved(struct jump_entry *iter_start,
		struct jump_entry *iter_stop, void *start, void *end)
{
	struct jump_entry *iter;

	iter = iter_start;
	while (iter < iter_stop) {
		if (addr_conflict(iter, start, end))
			return 1;
		iter++;
	}

	return 0;
}

/* 
 * Update code which is definitely not currently executing.
 * Architectures which need heavyweight synchronization to modify
 * running code can override this to make the non-live update case
 * cheaper.
 */
void __weak __init_or_module arch_jump_label_transform_static(struct jump_entry *entry,
					    enum jump_label_type type)
{
/* IAMROOT-12AB:
 * -------------
 * ARM에서는 arch/arm/kernel/jump_label.c에 이 함수 대신 사용한다.
 */
	arch_jump_label_transform(entry, type);	
}

static void __jump_label_update(struct static_key *key,
				struct jump_entry *entry,
				struct jump_entry *stop, int enable)
{

/* IAMROOT-12AB:
 * -------------
 * __jump_table 섹션에 있는 jump_entry의 key가 같은 엔트리들에 대해 
 * entry->code 주소가 커널 영역에 있는 항목을 업데이트하게 한다.
 */
	for (; (entry < stop) &&
	      (entry->key == (jump_label_t)(unsigned long)key);
	      entry++) {
		/*
		 * entry->code set to 0 invalidates module init text sections
		 * kernel_text_address() verifies we are not in core kernel
		 * init code, see jump_label_invalidate_module_init().
		 */

/* IAMROOT-12AB:
 * -------------
 * enable=0인 경우 nop, 1인 경우 b(branch) 명령으로 update한다.
 */
		if (entry->code && kernel_text_address(entry->code))
			arch_jump_label_transform(entry, enable);
	}
}


/* IAMROOT-12AB:
 * -------------
 * key->enabled 값과 key->entries 값이 다른 경우에만 JUMP_LABEL_ENABLED를 리턴하여 
 * jmp 코드를 만들도록 하게 한다.
 */
static enum jump_label_type jump_label_type(struct static_key *key)
{

/* IAMROOT-12AB:
 * -------------
 * true_branch: key->entries의 lsb 1비트 값
 */
	bool true_branch = jump_label_get_branch_default(key);

/* IAMROOT-12AB:
 * -------------
 * state:  key->enabled 값이 0보다 크면 1
 */
	bool state = static_key_enabled(key);

	if ((!true_branch && state) || (true_branch && !state))
		return JUMP_LABEL_ENABLE;

	return JUMP_LABEL_DISABLE;
}

void __init jump_label_init(void)
{
	struct jump_entry *iter_start = __start___jump_table;
	struct jump_entry *iter_stop = __stop___jump_table;
	struct static_key *key = NULL;
	struct jump_entry *iter;

	jump_label_lock();

/* IAMROOT-12AB:
 * -------------
 * 각 jump entry의 key 값으로 sorting한다.
 */
	jump_label_sort_entries(iter_start, iter_stop);

/* IAMROOT-12AB:
 * -------------
 * 컴파일 시 사용하던 조건 코드(static_key_false() & static_key_true())의 주소에
 * 무조건 nop 코드가 위치하는데 처음 초기 값에 따라서 그냥 nop로 놔둘지 아니면 
 * 조건 함수로 jump 해야하는지를 결정하도록 한다.
 */
	for (iter = iter_start; iter < iter_stop; iter++) {
		struct static_key *iterk;

		iterk = (struct static_key *)(unsigned long)iter->key;

/* IAMROOT-12AB:
 * -------------
 * 처음 컴파일 타임에 nop으로 되어있는데 여기서 key->enabled 및 key->entries 값이
 * 변한 적이 없기 때문에 결과는 다시 nop으로 생성되는데 다시 update 하려고 하는
 * 이유는? 
 *	-> STATIC_KEY_INIT_TRUE() 또는 STATIC_KEY_INIT_FALSE()로 선언하지 않고 
 *	   사용자가 struct static_key를 직접 만들고 멤버를 조작한 경우 
 *	   그 경우에 따라 jump 코드를 생성했을 가능성이 있기 때문에 다시 
 *	   update를 할 필요성이 있다고 판단한다.
 *
 * jump_label_type() 결과로 
 *	- JUMP_LABEL_ENABLE(1): jump 코드를 생성 
 *	- JUMP_LABEL_DISABLE(0): nop 코드를 생성
 */
		arch_jump_label_transform_static(iter, jump_label_type(iterk));
		if (iterk == key)
			continue;

		key = iterk;
		/*
		 * Set key->entries to iter, but preserve JUMP_LABEL_TRUE_BRANCH.
		 */

/* IAMROOT-12AB:
 * -------------
 * 동일한 static_key를 사용하는 엔트리들이 소팅되었으므로 그 중 가장 처음에 
 * 위치한 엔트리 주소 + 컴파일 타임에 지정된 default 값(0/1)을 더해서 
 * key->entries에 저장한다.
 */
		*((unsigned long *)&key->entries) += (unsigned long)iter;
#ifdef CONFIG_MODULES
		key->next = NULL;
#endif
	}
	static_key_initialized = true;
	jump_label_unlock();
}

#ifdef CONFIG_MODULES

struct static_key_mod {
	struct static_key_mod *next;
	struct jump_entry *entries;
	struct module *mod;
};

static int __jump_label_mod_text_reserved(void *start, void *end)
{
	struct module *mod;

	mod = __module_text_address((unsigned long)start);
	if (!mod)
		return 0;

	WARN_ON_ONCE(__module_text_address((unsigned long)end) != mod);

	return __jump_label_text_reserved(mod->jump_entries,
				mod->jump_entries + mod->num_jump_entries,
				start, end);
}

static void __jump_label_mod_update(struct static_key *key, int enable)
{
	struct static_key_mod *mod = key->next;

	while (mod) {
		struct module *m = mod->mod;

		__jump_label_update(key, mod->entries,
				    m->jump_entries + m->num_jump_entries,
				    enable);
		mod = mod->next;
	}
}

/***
 * apply_jump_label_nops - patch module jump labels with arch_get_jump_label_nop()
 * @mod: module to patch
 *
 * Allow for run-time selection of the optimal nops. Before the module
 * loads patch these with arch_get_jump_label_nop(), which is specified by
 * the arch specific jump label code.
 */
void jump_label_apply_nops(struct module *mod)
{
	struct jump_entry *iter_start = mod->jump_entries;
	struct jump_entry *iter_stop = iter_start + mod->num_jump_entries;
	struct jump_entry *iter;

	/* if the module doesn't have jump label entries, just return */
	if (iter_start == iter_stop)
		return;

	for (iter = iter_start; iter < iter_stop; iter++) {
		arch_jump_label_transform_static(iter, JUMP_LABEL_DISABLE);
	}
}

static int jump_label_add_module(struct module *mod)
{
	struct jump_entry *iter_start = mod->jump_entries;
	struct jump_entry *iter_stop = iter_start + mod->num_jump_entries;
	struct jump_entry *iter;
	struct static_key *key = NULL;
	struct static_key_mod *jlm;

	/* if the module doesn't have jump label entries, just return */
	if (iter_start == iter_stop)
		return 0;

	jump_label_sort_entries(iter_start, iter_stop);

	for (iter = iter_start; iter < iter_stop; iter++) {
		struct static_key *iterk;

		iterk = (struct static_key *)(unsigned long)iter->key;
		if (iterk == key)
			continue;

		key = iterk;

/* IAMROOT-12AB:
 * -------------
 * static_key를 모듈에서 생성한 경우는 static_key_module 구조체를 만들 필요없다.
 */
		if (__module_address(iter->key) == mod) {
			/*
			 * Set key->entries to iter, but preserve JUMP_LABEL_TRUE_BRANCH.
			 */
			*((unsigned long *)&key->entries) += (unsigned long)iter;
			key->next = NULL;
			continue;
		}
/* IAMROOT-12AB:
 * -------------
 * static_key를 모듈에서 선언하지 않고 사용만 하는 경우에는 커널 코어에 있는 
 * static_key 구조체에서 모듈에서 사용하고 있는 static_key_module 정보를 연결한다.
 * (static_key_module을 만드는 이유는 key가 변경되는 경우 nop/jmp 코드를 
 * 커널 코어 및 사용하는 모듈을 검색하여 모두 update하기 위함)
 */
		jlm = kzalloc(sizeof(struct static_key_mod), GFP_KERNEL);
		if (!jlm)
			return -ENOMEM;
		jlm->mod = mod;
		jlm->entries = iter;
		jlm->next = key->next;
		key->next = jlm;

/* IAMROOT-12AB:
 * -------------
 * 커널 코어에서 생성한 static_key가 JUMP_LABEL_ENABLE(jmp 코드 생성)인 경우 
 * 모듈도 거기에 맞게 update 한다.
 */
		if (jump_label_type(key) == JUMP_LABEL_ENABLE)
			__jump_label_update(key, iter, iter_stop, JUMP_LABEL_ENABLE);
	}

	return 0;
}

static void jump_label_del_module(struct module *mod)
{
	struct jump_entry *iter_start = mod->jump_entries;
	struct jump_entry *iter_stop = iter_start + mod->num_jump_entries;
	struct jump_entry *iter;
	struct static_key *key = NULL;
	struct static_key_mod *jlm, **prev;

	for (iter = iter_start; iter < iter_stop; iter++) {
		if (iter->key == (jump_label_t)(unsigned long)key)
			continue;

		key = (struct static_key *)(unsigned long)iter->key;

		if (__module_address(iter->key) == mod)
			continue;

		prev = &key->next;
		jlm = key->next;

		while (jlm && jlm->mod != mod) {
			prev = &jlm->next;
			jlm = jlm->next;
		}

		if (jlm) {
			*prev = jlm->next;
			kfree(jlm);
		}
	}
}

static void jump_label_invalidate_module_init(struct module *mod)
{
	struct jump_entry *iter_start = mod->jump_entries;
	struct jump_entry *iter_stop = iter_start + mod->num_jump_entries;
	struct jump_entry *iter;

	for (iter = iter_start; iter < iter_stop; iter++) {
		if (within_module_init(iter->code, mod))
			iter->code = 0;
	}
}

static int
jump_label_module_notify(struct notifier_block *self, unsigned long val,
			 void *data)
{
	struct module *mod = data;
	int ret = 0;

	switch (val) {
	case MODULE_STATE_COMING:
		jump_label_lock();

/* IAMROOT-12AB:
 * -------------
 * static key에 대한 초기화를 수행한다.
 */
		ret = jump_label_add_module(mod);
		if (ret)
			jump_label_del_module(mod);
		jump_label_unlock();
		break;
	case MODULE_STATE_GOING:
		jump_label_lock();
		jump_label_del_module(mod);
		jump_label_unlock();
		break;
	case MODULE_STATE_LIVE:
		jump_label_lock();
		jump_label_invalidate_module_init(mod);
		jump_label_unlock();
		break;
	}

	return notifier_from_errno(ret);
}

struct notifier_block jump_label_module_nb = {
	.notifier_call = jump_label_module_notify,
	.priority = 1, /* higher than tracepoints */
};

/* IAMROOT-12AB:
 * -------------
 * &module_notify_list에 notifier_block을 등록한다.
 * (모듈 state가 변화될 때마다 module chain에 등록된 
 * jump_label_module_notify() 함수가 호출된다.)
 */
static __init int jump_label_init_module(void)
{
	return register_module_notifier(&jump_label_module_nb);
}
early_initcall(jump_label_init_module);

#endif /* CONFIG_MODULES */

/***
 * jump_label_text_reserved - check if addr range is reserved
 * @start: start text addr
 * @end: end text addr
 *
 * checks if the text addr located between @start and @end
 * overlaps with any of the jump label patch addresses. Code
 * that wants to modify kernel text should first verify that
 * it does not overlap with any of the jump label addresses.
 * Caller must hold jump_label_mutex.
 *
 * returns 1 if there is an overlap, 0 otherwise
 */
int jump_label_text_reserved(void *start, void *end)
{
	int ret = __jump_label_text_reserved(__start___jump_table,
			__stop___jump_table, start, end);

	if (ret)
		return ret;

#ifdef CONFIG_MODULES
	ret = __jump_label_mod_text_reserved(start, end);
#endif
	return ret;
}

static void jump_label_update(struct static_key *key, int enable)
{
	struct jump_entry *stop = __stop___jump_table;
	struct jump_entry *entry = jump_label_get_entries(key);

#ifdef CONFIG_MODULES
	struct module *mod = __module_address((unsigned long)key);

	__jump_label_mod_update(key, enable);

	if (mod)
		stop = mod->jump_entries + mod->num_jump_entries;
#endif
	/* if there are no users, entry can be NULL */

/* IAMROOT-12AB:
 * -------------
 * enable=1인 경우 jump(b 명령), 0인 경우 nop로 치환된다.
 */
	if (entry)
		__jump_label_update(key, entry, stop, enable);
}

#endif
