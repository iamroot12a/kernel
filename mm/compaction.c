/*
 * linux/mm/compaction.c
 *
 * Memory compaction for the reduction of external fragmentation. Note that
 * this heavily depends upon page migration to do all the real heavy
 * lifting
 *
 * Copyright IBM Corp. 2007-2010 Mel Gorman <mel@csn.ul.ie>
 */
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/backing-dev.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/balloon_compaction.h>
#include <linux/page-isolation.h>
#include <linux/kasan.h>
#include "internal.h"

#ifdef CONFIG_COMPACTION
static inline void count_compact_event(enum vm_event_item item)
{
	count_vm_event(item);
}

static inline void count_compact_events(enum vm_event_item item, long delta)
{
	count_vm_events(item, delta);
}
#else
#define count_compact_event(item) do { } while (0)
#define count_compact_events(item, delta) do { } while (0)
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA
#ifdef CONFIG_TRACEPOINTS
static const char *const compaction_status_string[] = {
	"deferred",
	"skipped",
	"continue",
	"partial",
	"complete",
	"no_suitable_page",
	"not_suitable_zone",
};
#endif

#define CREATE_TRACE_POINTS
#include <trace/events/compaction.h>

static unsigned long release_freepages(struct list_head *freelist)
{
	struct page *page, *next;
	unsigned long high_pfn = 0;

/* IAMROOT-12:
 * -------------
 * freelist(isolation된)에 있는 order 0 페이지들 모두를 다시 버디시스템으로 돌려보낸다.
 */
	list_for_each_entry_safe(page, next, freelist, lru) {
		unsigned long pfn = page_to_pfn(page);
		list_del(&page->lru);
		__free_page(page);
		if (pfn > high_pfn)
			high_pfn = pfn;
	}

/* IAMROOT-12:
 * -------------
 * 반환한 페이지 중 가장 상위 pfn을 반환한다.
 */
	return high_pfn;
}

static void map_pages(struct list_head *list)
{
	struct page *page;

	list_for_each_entry(page, list, lru) {

		arch_alloc_page(page, 0);
/* IAMROOT-12:
 * -------------
 * 아래 2개 함수는 메모리 디버그 관련 함수
 */
		kernel_map_pages(page, 1, 1);
		kasan_alloc_pages(page, 0);
	}
}

static inline bool migrate_async_suitable(int migratetype)
{

/* IAMROOT-12:
 * -------------
 * cma 타입이나 movable 타입에 대해서만 async migration을 지원한다.
 */
	return is_migrate_cma(migratetype) || migratetype == MIGRATE_MOVABLE;
}

/*
 * Check that the whole (or subset of) a pageblock given by the interval of
 * [start_pfn, end_pfn) is valid and within the same zone, before scanning it
 * with the migration of free compaction scanner. The scanners then need to
 * use only pfn_valid_within() check for arches that allow holes within
 * pageblocks.
 *
 * Return struct page pointer of start_pfn, or NULL if checks were not passed.
 *
 * It's possible on some configurations to have a setup like node0 node1 node0
 * i.e. it's possible that all pages within a zones range of pages do not
 * belong to a single zone. We assume that a border between node0 and node1
 * can occur within a single pageblock, but not a node0 node1 node0
 * interleaving within a single pageblock. It is therefore sufficient to check
 * the first and last page of a pageblock and avoid checking each individual
 * page in a pageblock.
 */

/* IAMROOT-12:
 * -------------
 * start_pfn ~ end_pfn이 같은 zone에 있는 경우 start_pfn을 반환한다.
 * (보통 페이지블럭 단위로 start_pfn값과 end_pfn 값을 부여해서 해당 
 * 페이지 블럭이 같은 zone에 있는 상태인지 확인할 목적이다)
 */
static struct page *pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone)
{
	struct page *start_page;
	struct page *end_page;

	/* end_pfn is one past the range we are checking */
	end_pfn--;

	if (!pfn_valid(start_pfn) || !pfn_valid(end_pfn))
		return NULL;

	start_page = pfn_to_page(start_pfn);

	if (page_zone(start_page) != zone)
		return NULL;

	end_page = pfn_to_page(end_pfn);

	/* This gives a shorter code than deriving page_zone(end_page) */
	if (page_zone_id(start_page) != page_zone_id(end_page))
		return NULL;

	return start_page;
}

#ifdef CONFIG_COMPACTION

/* Do not skip compaction more than 64 times */

/* IAMROOT-12:
 * -------------
 * compaction 유예를 2^6 만큼 쌓이면 더 이상 유예하지 못한다.
 * 즉, compaction을 진행해야 한다.
 */
#define COMPACT_MAX_DEFER_SHIFT 6

/*
 * Compaction is deferred when compaction fails to result in a page
 * allocation success. 1 << compact_defer_limit compactions are skipped up
 * to a limit of 1 << COMPACT_MAX_DEFER_SHIFT
 */
void defer_compaction(struct zone *zone, int order)
{
	zone->compact_considered = 0;
	zone->compact_defer_shift++;

/* IAMROOT-12:
 * -------------
 * 기존 실패했던 order보다 작은 order로 요청했는데도 불구하고 실패한 케이스이므로
 * 실패 order를 update한다.
 */
	if (order < zone->compact_order_failed)
		zone->compact_order_failed = order;

/* IAMROOT-12:
 * -------------
 * compact_defer_shift는 6을 초과하지 못하게 제한한다.
 */
	if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
		zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;

	trace_mm_compaction_defer_compaction(zone, order);
}

/* Returns true if compaction should be skipped this time */

/* IAMROOT-12:
 * -------------
 * compaction 유예가 필요한 경우 true를 반환한다.
 *
 * 최종 실패한 order 보다 작은 order로 시도하는 경우 compaction 유예와 관련없이
 * 항상 false를 반환하여 compaction을 진행하게 한다.
 *
 * 최종 실패한 order와 같거나 큰 order로 요청한 경우는 compaction 유예를 할 수 
 * 있는데 compaction complete가 발생할 때 마다 유예해야하는 횟수가 늘어난다.
 * 처음 compaction complete가 발생하기 전에는 유예하지 않고, 
 * 다은 compaction complete가 발생할 때마다 유예 횟수가 2의 차수 단위로 
 * 증가하는데 최대 64까지 증가한다.)
 */
bool compaction_deferred(struct zone *zone, int order)
{

/* IAMROOT-12:
 * -------------
 * defer_limit는 1, 2, 4, 8, 16, 32, 64까지 가능(compact_defer_shift가 max가 6)
 *
 * compaction을 시도하는데 compact_defer_shift가 1일 때에는 유예하지 않고,
 * 증가함에 따라 유예횟수가 증가된다. 최종 6이 되는 경우 compaction을 시도할 때
 * 63번 유예한 후 64번째에 compaction을 시도하게 된다.
 *
 * (너무 빈번한 compaction을 막기 위한 조치)
 */
	unsigned long defer_limit = 1UL << zone->compact_defer_shift;

/* IAMROOT-12:
 * -------------
 * 최근 실패시 order 값이 담겨 있고, 이 보다 작은 order만 compaction 시도한다.
 */
	if (order < zone->compact_order_failed)
		return false;

	/* Avoid possible overflow */

/* IAMROOT-12:
 * -------------
 * compact_considered가 defer_limit 이상인 경우 그 값을 제한 시키고 
 * compaction 시도하게 한다.
 */
	if (++zone->compact_considered > defer_limit)
		zone->compact_considered = defer_limit;

	if (zone->compact_considered >= defer_limit)
		return false;

	trace_mm_compaction_deferred(zone, order);

/* IAMROOT-12:
 * -------------
 * true인 경우 compaction 유예를 한다.
 */
	return true;
}

/*
 * Update defer tracking counters after successful compaction of given order,
 * which means an allocation either succeeded (alloc_success == true) or is
 * expected to succeed.
 */
void compaction_defer_reset(struct zone *zone, int order,
		bool alloc_success)
{
/* IAMROOT-12:
 * -------------
 * alloc_success가 true인 경우 요청한 zone에 대해 defer 상태를 클리어한다.
 *                 false인 경우 클리어하지 않는다.
 *
 * alloc_success 여부와 상관없이 요청 order가 실패 order보다 같거나 큰 경우
 * 요청 order+1로 실패 order를 갱신한다.
 */
	if (alloc_success) {
		zone->compact_considered = 0;
		zone->compact_defer_shift = 0;
	}
	if (order >= zone->compact_order_failed)
		zone->compact_order_failed = order + 1;

	trace_mm_compaction_defer_reset(zone, order);
}

/* Returns true if restarting compaction after many failures */
bool compaction_restarting(struct zone *zone, int order)
{
/* IAMROOT-12:
 * -------------
 * 최근 compact 시 실패하였던 order 보다 작은 order로 요청한 경우 
 * 리스타팅 중이 아니다.
 */
	if (order < zone->compact_order_failed)
		return false;

/* IAMROOT-12:
 * -------------
 * compacton 유예가 한도까지 온 경우이면서 
 */
	return zone->compact_defer_shift == COMPACT_MAX_DEFER_SHIFT &&
		zone->compact_considered >= 1UL << zone->compact_defer_shift;
}

/* Returns true if the pageblock should be scanned for pages to isolate. */
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{

/* IAMROOT-12:
 * -------------
 * ignore_skip_hint가 설정된 경우 skip에 대한 hint를 사용하지 말고 무조건 
 * isolation 하도록 true를 반환한다.
 */

	if (cc->ignore_skip_hint)
		return true;

/* IAMROOT-12:
 * -------------
 * 해당 페이지의 페이지블럭에 skip bit가 설정된 경우 isolation을 하지 않도록
 * false를 반한한다.
 */

	return !get_pageblock_skip(page);
}

/*
 * This function is called to clear all cached information on pageblocks that
 * should be skipped for page isolation when the migrate and free page scanner
 * meet.
 */
static void __reset_isolation_suitable(struct zone *zone)
{
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long pfn;

/* IAMROOT-12:
 * -------------
 * migrate 스캐너(sync/async)에 zone 시작 pfn으로 리셋 (위로 증가)
 * free 스캐너에 zone 끝 pfn으로 리셋 (아래로 증가)
 */
	zone->compact_cached_migrate_pfn[0] = start_pfn;
	zone->compact_cached_migrate_pfn[1] = start_pfn;
	zone->compact_cached_free_pfn = end_pfn;
	zone->compact_blockskip_flush = false;

	/* Walk the zone and mark every pageblock as suitable for isolation */

/* IAMROOT-12:
 * -------------
 * start_pfn ~ end_pfn 까지의 페이지블럭단위로 페이지블럭의 PB_migrate_skip 비트를
 * 클리어한다. (zone의 모든 페이지 블럭의 migrate skip 비트를 클리어해서 
 * 다시 시작할 수 있도록 준비한다) 
 */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;

		cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);
		if (zone != page_zone(page))
			continue;

		clear_pageblock_skip(page);
	}
}

void reset_isolation_suitable(pg_data_t *pgdat)
{
	int zoneid;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		struct zone *zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		/* Only flush if a full compaction finished recently */
		if (zone->compact_blockskip_flush)
			__reset_isolation_suitable(zone);
	}
}

/*
 * If no pages were isolated then mark this pageblock to be skipped in the
 * future. The information is later cleared by __reset_isolation_suitable().
 */
static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long nr_isolated,
			bool migrate_scanner)
{
	struct zone *zone = cc->zone;
	unsigned long pfn;

	if (cc->ignore_skip_hint)
		return;

	if (!page)
		return;

	if (nr_isolated)
		return;

	set_pageblock_skip(page);

	pfn = page_to_pfn(page);

	/* Update where async and sync compaction should restart */
	if (migrate_scanner) {
		if (pfn > zone->compact_cached_migrate_pfn[0])
			zone->compact_cached_migrate_pfn[0] = pfn;
		if (cc->mode != MIGRATE_ASYNC &&
		    pfn > zone->compact_cached_migrate_pfn[1])
			zone->compact_cached_migrate_pfn[1] = pfn;
	} else {
		if (pfn < zone->compact_cached_free_pfn)
			zone->compact_cached_free_pfn = pfn;
	}
}
#else
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	return true;
}

static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long nr_isolated,
			bool migrate_scanner)
{
}
#endif /* CONFIG_COMPACTION */

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. For async compaction, back out if the lock cannot
 * be taken immediately. For sync compaction, spin on the lock if needed.
 *
 * Returns true if the lock is held
 * Returns false if the lock is not held and compaction should abort
 */
static bool compact_trylock_irqsave(spinlock_t *lock, unsigned long *flags,
						struct compact_control *cc)
{
	if (cc->mode == MIGRATE_ASYNC) {
		if (!spin_trylock_irqsave(lock, *flags)) {
			cc->contended = COMPACT_CONTENDED_LOCK;
			return false;
		}
	} else {
		spin_lock_irqsave(lock, *flags);
	}

	return true;
}

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. The lock should be periodically unlocked to avoid
 * having disabled IRQs for a long time, even when there is nobody waiting on
 * the lock. It might also be that allowing the IRQs will result in
 * need_resched() becoming true. If scheduling is needed, async compaction
 * aborts. Sync compaction schedules.
 * Either compaction type will also abort if a fatal signal is pending.
 * In either case if the lock was locked, it is dropped and not regained.
 *
 * Returns true if compaction should abort due to fatal signal pending, or
 *		async compaction due to need_resched()
 * Returns false when compaction can continue (sync compaction might have
 *		scheduled)
 */
static bool compact_unlock_should_abort(spinlock_t *lock,
		unsigned long flags, bool *locked, struct compact_control *cc)
{
	if (*locked) {
		spin_unlock_irqrestore(lock, flags);
		*locked = false;
	}

	if (fatal_signal_pending(current)) {
		cc->contended = COMPACT_CONTENDED_SCHED;
		return true;
	}

	if (need_resched()) {
		if (cc->mode == MIGRATE_ASYNC) {
			cc->contended = COMPACT_CONTENDED_SCHED;
			return true;
		}
		cond_resched();
	}

	return false;
}

/*
 * Aside from avoiding lock contention, compaction also periodically checks
 * need_resched() and either schedules in sync compaction or aborts async
 * compaction. This is similar to what compact_unlock_should_abort() does, but
 * is used where no lock is concerned.
 *
 * Returns false when no scheduling was needed, or sync compaction scheduled.
 * Returns true when async compaction should abort.
 */
static inline bool compact_should_abort(struct compact_control *cc)
{
	/* async compaction aborts if contended */

/* IAMROOT-12:
 * -------------
 * 리스케쥴 요청이 있으면서 스케쥴링 혼잡을 contended에 기록하고 abort 처리를
 * 하도록 true를 반환한다.
 */
	if (need_resched()) {
		if (cc->mode == MIGRATE_ASYNC) {
			cc->contended = COMPACT_CONTENDED_SCHED;
			return true;
		}

		cond_resched();
	}

	return false;
}

/* Returns true if the page is within a block suitable for migration to */
static bool suitable_migration_target(struct page *page)
{
	/* If the page is a large free page, then disallow migration */
	if (PageBuddy(page)) {
		/*
		 * We are checking page_order without zone->lock taken. But
		 * the only small danger is that we skip a potentially suitable
		 * pageblock, so it's not worth to check order for valid range.
		 */
		if (page_order_unsafe(page) >= pageblock_order)
			return false;
	}

	/* If the block is MIGRATE_MOVABLE or MIGRATE_CMA, allow migration */
	if (migrate_async_suitable(get_pageblock_migratetype(page)))
		return true;

	/* Otherwise skip the block */
	return false;
}

/*
 * Isolate free pages onto a private freelist. If @strict is true, will abort
 * returning 0 on any invalid PFNs or non-free pages inside of the pageblock
 * (even though it may still end up isolating some pages).
 */
static unsigned long isolate_freepages_block(struct compact_control *cc,
				unsigned long *start_pfn,
				unsigned long end_pfn,
				struct list_head *freelist,
				bool strict)
{
	int nr_scanned = 0, total_isolated = 0;
	struct page *cursor, *valid_page = NULL;
	unsigned long flags = 0;
	bool locked = false;
	unsigned long blockpfn = *start_pfn;

	cursor = pfn_to_page(blockpfn);

	/* Isolate free pages. */
	for (; blockpfn < end_pfn; blockpfn++, cursor++) {
		int isolated, i;
		struct page *page = cursor;

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort if fatal signal
		 * pending or async compaction detects need_resched()
		 */

/* IAMROOT-12:
 * -------------
 * 약 32개 단위로 abort 조건을 확인한다.
 */
		if (!(blockpfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&cc->zone->lock, flags,
								&locked, cc))
			break;

		nr_scanned++;
		if (!pfn_valid_within(blockpfn))
			goto isolate_fail;

		if (!valid_page)
			valid_page = page;

/* IAMROOT-12:
 * -------------
 * 버디 시스템에서 free 페이지를 찾아와야하기 때문에 Buddy 플래그가 설정되어 
 * 있어야 한다.
 */
		if (!PageBuddy(page))
			goto isolate_fail;

		/*
		 * If we already hold the lock, we can skip some rechecking.
		 * Note that if we hold the lock now, checked_pageblock was
		 * already set in some previous iteration (or strict is true),
		 * so it is correct to skip the suitable migration target
		 * recheck as well.
		 */
		if (!locked) {
			/*
			 * The zone lock must be held to isolate freepages.
			 * Unfortunately this is a very coarse lock and can be
			 * heavily contended if there are parallel allocations
			 * or parallel compactions. For async compaction do not
			 * spin on the lock and we acquire the lock as late as
			 * possible.
			 */
			locked = compact_trylock_irqsave(&cc->zone->lock,
								&flags, cc);
			if (!locked)
				break;

			/* Recheck this is a buddy page under lock */
			if (!PageBuddy(page))
				goto isolate_fail;
		}

		/* Found a free page, break it into order-0 pages */

/* IAMROOT-12:
 * -------------
 * 요청 페이지를 버디에서 제거한 후 order 0 페이지로 분리하여 가져온다.
 */
		isolated = split_free_page(page);
		total_isolated += isolated;

/* IAMROOT-12:
 * -------------
 * 분리된 order-0 페이지들을 모두 freelist에 추가한다.
 */
		for (i = 0; i < isolated; i++) {
			list_add(&page->lru, freelist);
			page++;
		}

		/* If a page was split, advance to the end of it */
		if (isolated) {
			cc->nr_freepages += isolated;
			if (!strict &&
				cc->nr_migratepages <= cc->nr_freepages) {
				blockpfn += isolated;
				break;
			}

			blockpfn += isolated - 1;
			cursor += isolated - 1;
			continue;
		}

isolate_fail:
		if (strict)
			break;
		else
			continue;

	}

	trace_mm_compaction_isolate_freepages(*start_pfn, blockpfn,
					nr_scanned, total_isolated);

	/* Record how far we have got within the block */
	*start_pfn = blockpfn;

	/*
	 * If strict isolation is requested by CMA then check that all the
	 * pages requested were isolated. If there were any failures, 0 is
	 * returned and CMA will fail.
	 */
	if (strict && blockpfn < end_pfn)
		total_isolated = 0;

	if (locked)
		spin_unlock_irqrestore(&cc->zone->lock, flags);

	/* Update the pageblock-skip if the whole pageblock was scanned */
	if (blockpfn == end_pfn)
		update_pageblock_skip(cc, valid_page, total_isolated, false);

	count_compact_events(COMPACTFREE_SCANNED, nr_scanned);
	if (total_isolated)
		count_compact_events(COMPACTISOLATED, total_isolated);
	return total_isolated;
}

/**
 * isolate_freepages_range() - isolate free pages.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Non-free pages, invalid PFNs, or zone boundaries within the
 * [start_pfn, end_pfn) range are considered errors, cause function to
 * undo its actions and return zero.
 *
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater then end_pfn if end fell in a middle of
 * a free page).
 */
unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long isolated, pfn, block_end_pfn;
	LIST_HEAD(freelist);

	pfn = start_pfn;
	block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);

	for (; pfn < end_pfn; pfn += isolated,
				block_end_pfn += pageblock_nr_pages) {
		/* Protect pfn from changing by isolate_freepages_block */
		unsigned long isolate_start_pfn = pfn;

		block_end_pfn = min(block_end_pfn, end_pfn);

		/*
		 * pfn could pass the block_end_pfn if isolated freepage
		 * is more than pageblock order. In this case, we adjust
		 * scanning range to right one.
		 */
		if (pfn >= block_end_pfn) {
			block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);
			block_end_pfn = min(block_end_pfn, end_pfn);
		}

		if (!pageblock_pfn_to_page(pfn, block_end_pfn, cc->zone))
			break;

		isolated = isolate_freepages_block(cc, &isolate_start_pfn,
						block_end_pfn, &freelist, true);

		/*
		 * In strict mode, isolate_freepages_block() returns 0 if
		 * there are any holes in the block (ie. invalid PFNs or
		 * non-free pages).
		 */
		if (!isolated)
			break;

		/*
		 * If we managed to isolate pages, it is always (1 << n) *
		 * pageblock_nr_pages for some non-negative n.  (Max order
		 * page may span two pageblocks).
		 */
	}

	/* split_free_page does not map the pages */
	map_pages(&freelist);

	if (pfn < end_pfn) {
		/* Loop terminated early, cleanup. */
		release_freepages(&freelist);
		return 0;
	}

	/* We don't use freelists for anything. */
	return pfn;
}

/* Update the number of anon and file isolated pages in the zone */
static void acct_isolated(struct zone *zone, struct compact_control *cc)
{
	struct page *page;
	unsigned int count[2] = { 0, };

	if (list_empty(&cc->migratepages))
		return;

/* IAMROOT-12:
 * -------------
 * migratepages 리스트에 있는 모든 엔트리에서 파일 캐시 페이지인 경우 
 * NR_ISOLATED_FILE 카운터를 증가시키고 그렇지 않은 경우 NR_ISOLATED_ANON
 * 카운터를 증가시킨다. (페이지 수 만큼)
 */
	list_for_each_entry(page, &cc->migratepages, lru)
		count[!!page_is_file_cache(page)]++;

	mod_zone_page_state(zone, NR_ISOLATED_ANON, count[0]);
	mod_zone_page_state(zone, NR_ISOLATED_FILE, count[1]);
}

/* Similar to reclaim, but different enough that they don't share logic */
static bool too_many_isolated(struct zone *zone)
{
	unsigned long active, inactive, isolated;

	inactive = zone_page_state(zone, NR_INACTIVE_FILE) +
					zone_page_state(zone, NR_INACTIVE_ANON);
	active = zone_page_state(zone, NR_ACTIVE_FILE) +
					zone_page_state(zone, NR_ACTIVE_ANON);
	isolated = zone_page_state(zone, NR_ISOLATED_FILE) +
					zone_page_state(zone, NR_ISOLATED_ANON);

	return isolated > (inactive + active) / 2;
}

/**
 * isolate_migratepages_block() - isolate all migrate-able pages within
 *				  a single pageblock
 * @cc:		Compaction control structure.
 * @low_pfn:	The first PFN to isolate
 * @end_pfn:	The one-past-the-last PFN to isolate, within same pageblock
 * @isolate_mode: Isolation mode to be used.
 *
 * Isolate all pages that can be migrated from the range specified by
 * [low_pfn, end_pfn). The range is expected to be within same pageblock.
 * Returns zero if there is a fatal signal pending, otherwise PFN of the
 * first page that was not scanned (which may be both less, equal to or more
 * than end_pfn).
 *
 * The pages are isolated on cc->migratepages list (not required to be empty),
 * and cc->nr_migratepages is updated accordingly. The cc->migrate_pfn field
 * is neither read nor updated.
 */

/* IAMROOT-12:
 * -------------
 * 한 개의 페이지블럭에서 옮길 수 있는 페이지 들을 찾아 cc->migratepages에 
 * 추가한다.
 */
static unsigned long
isolate_migratepages_block(struct compact_control *cc, unsigned long low_pfn,
			unsigned long end_pfn, isolate_mode_t isolate_mode)
{
	struct zone *zone = cc->zone;
	unsigned long nr_scanned = 0, nr_isolated = 0;
	struct list_head *migratelist = &cc->migratepages;
	struct lruvec *lruvec;
	unsigned long flags = 0;
	bool locked = false;
	struct page *page = NULL, *valid_page = NULL;
	unsigned long start_pfn = low_pfn;

	/*
	 * Ensure that there are not too many pages isolated from the LRU
	 * list by either parallel reclaimers or compaction. If there are,
	 * delay for some time until fewer pages are isolated
	 */

/* IAMROOT-12:
 * -------------
 * 낮은 확률로 과도한 isolation이 발생한 경우에만 sync일 때 0.1초를 쉰다.
 */
	while (unlikely(too_many_isolated(zone))) {
		/* async migration should just abort */
		if (cc->mode == MIGRATE_ASYNC)
			return 0;

		congestion_wait(BLK_RW_ASYNC, HZ/10);

		if (fatal_signal_pending(current))
			return 0;
	}


/* IAMROOT-12:
 * -------------
 * 다시 한 번 async에서 리스케쥴 요청이 있는지 확인한다.
 */

	if (compact_should_abort(cc))
		return 0;

	/* Time to isolate some pages for migration */
	for (; low_pfn < end_pfn; low_pfn++) {
		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort async compaction
		 * if contended.
		 */

/* IAMROOT-12:
 * -------------
 * 대략 32번에 한 번씩 compaction을 중지해야 하는 경우가 발생하는지 확인한다
 */
		if (!(low_pfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&zone->lru_lock, flags,
								&locked, cc))
			break;

		if (!pfn_valid_within(low_pfn))
			continue;
		nr_scanned++;

		page = pfn_to_page(low_pfn);

		if (!valid_page)
			valid_page = page;

		/*
		 * Skip if free. We read page order here without zone lock
		 * which is generally unsafe, but the race window is small and
		 * the worst thing that can happen is that we skip some
		 * potential isolation targets.
		 */

/* IAMROOT-12:
 * -------------
 * 버디 페이지인 경우에는 skip 한다. (지금은 사용중인 페이지를 찾는 중이다)
 */
		if (PageBuddy(page)) {
			unsigned long freepage_order = page_order_unsafe(page);

			/*
			 * Without lock, we cannot be sure that what we got is
			 * a valid page order. Consider only values in the
			 * valid order range to prevent low_pfn overflow.
			 */
			if (freepage_order > 0 && freepage_order < MAX_ORDER)
				low_pfn += (1UL << freepage_order) - 1;
			continue;
		}

		/*
		 * Check may be lockless but that's ok as we recheck later.
		 * It's possible to migrate LRU pages and balloon pages
		 * Skip any other type of page
		 */

/* IAMROOT-12:
 * -------------
 * lru 페이지가 아닌 페이지들은 skip하는데, 예외적으로 ballon movable 페이지가 
 * isolation 가능하면 isolation을 한다.
 */
		if (!PageLRU(page)) {
			if (unlikely(balloon_page_movable(page))) {
				if (balloon_page_isolate(page)) {
					/* Successfully isolated */
					goto isolate_success;
				}
			}
			continue;
		}

		/*
		 * PageLRU is set. lru_lock normally excludes isolation
		 * splitting and collapsing (collapsing has already happened
		 * if PageLRU is set) but the lock is not necessarily taken
		 * here and it is wasteful to take it just to check transhuge.
		 * Check TransHuge without lock and skip the whole pageblock if
		 * it's either a transhuge or hugetlbfs page, as calling
		 * compound_order() without preventing THP from splitting the
		 * page underneath us may return surprising results.
		 */

/* IAMROOT-12:
 * -------------
 * transparent huge page는 skip 한다.
 */
		if (PageTransHuge(page)) {
			if (!locked)
				low_pfn = ALIGN(low_pfn + 1,
						pageblock_nr_pages) - 1;
			else
				low_pfn += (1 << compound_order(page)) - 1;

			continue;
		}

		/*
		 * Migration will fail if an anonymous page is pinned in memory,
		 * so avoid taking lru_lock and isolating it unnecessarily in an
		 * admittedly racy check.
		 */

/* IAMROOT-12:
 * -------------
 * 매핑되지 않았으면서 페이지 참조 횟수가 매핑 카운터보다 큰 경우 skip 한다.
 */
		if (!page_mapping(page) &&
		    page_count(page) > page_mapcount(page))
			continue;

		/* If we already hold the lock, we can skip some rechecking */
		if (!locked) {
			locked = compact_trylock_irqsave(&zone->lru_lock,
								&flags, cc);
			if (!locked)
				break;

			/* Recheck PageLRU and PageTransHuge under lock */
			if (!PageLRU(page))
				continue;
			if (PageTransHuge(page)) {
				low_pfn += (1 << compound_order(page)) - 1;
				continue;
			}
		}

/* IAMROOT-12:
 * -------------
 * memcg가 설정된 경우 zone->lruvec이 아니라 memcg의 lruvec를 가져온다.
 */
		lruvec = mem_cgroup_page_lruvec(page, zone);

		/* Try isolate the page */
/* IAMROOT-12:
 * -------------
 * 요청 페이지가 isolation 되지 않은 경우 skip 한다.
 */
		if (__isolate_lru_page(page, isolate_mode) != 0)
			continue;

		VM_BUG_ON_PAGE(PageTransCompound(page), page);

		/* Successfully isolated */

/* IAMROOT-12:
 * -------------
 * 기존(zone->lruvec or mz->lruvec)에 있는 lruvec 리스트에서 해당 페이지를 
 * 제거한다.
 */
		del_page_from_lru_list(page, lruvec, page_lru(page));

isolate_success:

/* IAMROOT-12:
 * -------------
 * 해당 페이지를 일단 migratelist에 추가한다.
 */
		list_add(&page->lru, migratelist);
		cc->nr_migratepages++;
		nr_isolated++;

		/* Avoid isolating too much */

/* IAMROOT-12:
 * -------------
 * 32개 페이지가 isolate된 경우 중간에 루프를 탈출한다.
 */
		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX) {
			++low_pfn;
			break;
		}
	}

	/*
	 * The PageBuddy() check could have potentially brought us outside
	 * the range to be scanned.
	 */
	if (unlikely(low_pfn > end_pfn))
		low_pfn = end_pfn;

	if (locked)
		spin_unlock_irqrestore(&zone->lru_lock, flags);

	/*
	 * Update the pageblock-skip information and cached scanner pfn,
	 * if the whole pageblock was scanned without isolating any page.
	 */

/* IAMROOT-12:
 * -------------
 * 마지막까지 처리된 페이지블럭, 즉 현재 시점에서 더 이상 isolate할 페이지가 
 * 없는 경우 이 페이지블럭의 skip 플래그를 1로 설정한다.
 *
 * 다음에 스캔할때에는 이 페이지를 skip 처리한다.
 */
	if (low_pfn == end_pfn)
		update_pageblock_skip(cc, valid_page, nr_isolated, true);

	trace_mm_compaction_isolate_migratepages(start_pfn, low_pfn,
						nr_scanned, nr_isolated);

	count_compact_events(COMPACTMIGRATE_SCANNED, nr_scanned);
	if (nr_isolated)
		count_compact_events(COMPACTISOLATED, nr_isolated);

	return low_pfn;
}

/**
 * isolate_migratepages_range() - isolate migrate-able pages in a PFN range
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Returns zero if isolation fails fatally due to e.g. pending signal.
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater than end_pfn if end fell in a middle of a THP page).
 */
unsigned long
isolate_migratepages_range(struct compact_control *cc, unsigned long start_pfn,
							unsigned long end_pfn)
{
	unsigned long pfn, block_end_pfn;

	/* Scan block by block. First and last block may be incomplete */
	pfn = start_pfn;
	block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);

	for (; pfn < end_pfn; pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {

		block_end_pfn = min(block_end_pfn, end_pfn);

		if (!pageblock_pfn_to_page(pfn, block_end_pfn, cc->zone))
			continue;

		pfn = isolate_migratepages_block(cc, pfn, block_end_pfn,
							ISOLATE_UNEVICTABLE);

		/*
		 * In case of fatal failure, release everything that might
		 * have been isolated in the previous iteration, and signal
		 * the failure back to caller.
		 */
		if (!pfn) {
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			break;
		}

		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX)
			break;
	}
	acct_isolated(cc->zone, cc);

	return pfn;
}

#endif /* CONFIG_COMPACTION || CONFIG_CMA */
#ifdef CONFIG_COMPACTION
/*
 * Based on information in the current compact_control, find blocks
 * suitable for isolating free pages from and then isolate them.
 */
static void isolate_freepages(struct compact_control *cc)
{
	struct zone *zone = cc->zone;
	struct page *page;
	unsigned long block_start_pfn;	/* start of current pageblock */
	unsigned long isolate_start_pfn; /* exact pfn we start at */
	unsigned long block_end_pfn;	/* end of current pageblock */
	unsigned long low_pfn;	     /* lowest pfn scanner is able to scan */
	struct list_head *freelist = &cc->freepages;

	/*
	 * Initialise the free scanner. The starting point is where we last
	 * successfully isolated from, zone-cached value, or the end of the
	 * zone when isolating for the first time. For looping we also need
	 * this pfn aligned down to the pageblock boundary, because we do
	 * block_start_pfn -= pageblock_nr_pages in the for loop.
	 * For ending point, take care when isolating in last pageblock of a
	 * a zone which ends in the middle of a pageblock.
	 * The low boundary is the end of the pageblock the migration scanner
	 * is using.
	 */

/* IAMROOT-12:
 * -------------
 * free 스캐너가 동작하는 페이지 블럭의 시작 pfn 부터 끝 pfn을 구한다.
 */
	isolate_start_pfn = cc->free_pfn;
	block_start_pfn = cc->free_pfn & ~(pageblock_nr_pages-1);
	block_end_pfn = min(block_start_pfn + pageblock_nr_pages,
						zone_end_pfn(zone));
	low_pfn = ALIGN(cc->migrate_pfn + 1, pageblock_nr_pages);

	/*
	 * Isolate free pages until enough are available to migrate the
	 * pages on cc->migratepages. We stop searching if the migrate
	 * and free page scanners meet or enough free pages are isolated.
	 */

/* IAMROOT-12:
 * -------------
 * 현재 페이지 블럭이 migrate 스캐너보다 높은 위치에서 움직일 때
 */
	for (; block_start_pfn >= low_pfn &&
			cc->nr_migratepages > cc->nr_freepages;
				block_end_pfn = block_start_pfn,
				block_start_pfn -= pageblock_nr_pages,
				isolate_start_pfn = block_start_pfn) {

		/*
		 * This can iterate a massively long zone without finding any
		 * suitable migration targets, so periodically check if we need
		 * to schedule, or even abort async compaction.
		 */

/* IAMROOT-12:
 * -------------
 * 약 32번에 한 번씩 compact abort 케이스가 있는 경우 break
 */
		if (!(block_start_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;

/* IAMROOT-12:
 * -------------
 * 처리할 페이지블럭 하나가 같은 zone에 있는지 확인한다.
 */
		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
		if (!page)
			continue;

		/* Check the block is suitable for migration */
		if (!suitable_migration_target(page))
			continue;

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;

		/* Found a block suitable for isolating free pages from. */

/* IAMROOT-12:
 * -------------
 * free 스캐너로부터 스캔한 free 페이지들을 order-0 페이지로 spilit하여 
 * freelist에 담아온다.
 */
		isolate_freepages_block(cc, &isolate_start_pfn,
					block_end_pfn, freelist, false);

		/*
		 * Remember where the free scanner should restart next time,
		 * which is where isolate_freepages_block() left off.
		 * But if it scanned the whole pageblock, isolate_start_pfn
		 * now points at block_end_pfn, which is the start of the next
		 * pageblock.
		 * In that case we will however want to restart at the start
		 * of the previous pageblock.
		 */

/* IAMROOT-12:
 * -------------
 * 다음 free 스캔 위치를 저장한다.
 */
		cc->free_pfn = (isolate_start_pfn < block_end_pfn) ?
				isolate_start_pfn :
				block_start_pfn - pageblock_nr_pages;

		/*
		 * isolate_freepages_block() might have aborted due to async
		 * compaction being contended
		 */
		if (cc->contended)
			break;
	}

	/* split_free_page does not map the pages */
	map_pages(freelist);

	/*
	 * If we crossed the migrate scanner, we want to keep it that way
	 * so that compact_finished() may detect this
	 */

/* IAMROOT-12:
 * -------------
 * migrate 스캐너와 free 스캐너가 cross되어 만났음을 뜻하며 여기서 종료 처리한다.
 */
	if (block_start_pfn < low_pfn)
		cc->free_pfn = cc->migrate_pfn;
}

/*
 * This is a migrate-callback that "allocates" freepages by taking pages
 * from the isolated freelists in the block we are migrating to.
 */
static struct page *compaction_alloc(struct page *migratepage,
					unsigned long data,
					int **result)
{
	struct compact_control *cc = (struct compact_control *)data;
	struct page *freepage;

	/*
	 * Isolate free pages if necessary, and if we are not aborting due to
	 * contention.
	 */

/* IAMROOT-12:
 * -------------
 * cc->freepages에 free 스캔된 페이지가 등록되는데 이 곳이 비어 있는 경우 
 * free 스캐너를 통해서 free 페이지를 받아온다.
 */
	if (list_empty(&cc->freepages)) {
		if (!cc->contended)
			isolate_freepages(cc);

		if (list_empty(&cc->freepages))
			return NULL;
	}

/* IAMROOT-12:
 * -------------
 * free 페이지(order-0 페이지)를 하나 분리하고 반환한다.
 */
	freepage = list_entry(cc->freepages.next, struct page, lru);
	list_del(&freepage->lru);
	cc->nr_freepages--;

	return freepage;
}

/*
 * This is a migrate-callback that "frees" freepages back to the isolated
 * freelist.  All pages on the freelist are from the same zone, so there is no
 * special handling needed for NUMA.
 */
static void compaction_free(struct page *page, unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;

	list_add(&page->lru, &cc->freepages);
	cc->nr_freepages++;
}

/* possible outcome of isolate_migratepages */
typedef enum {
	ISOLATE_ABORT,		/* Abort compaction now */
	ISOLATE_NONE,		/* No pages isolated, continue scanning */
	ISOLATE_SUCCESS,	/* Pages isolated, migrate */
} isolate_migrate_t;

/*
 * Isolate all pages that can be migrated from the first suitable block,
 * starting at the block pointed to by the migrate scanner pfn within
 * compact_control.
 */
static isolate_migrate_t isolate_migratepages(struct zone *zone,
					struct compact_control *cc)
{
	unsigned long low_pfn, end_pfn;
	struct page *page;
	const isolate_mode_t isolate_mode =
		(cc->mode == MIGRATE_ASYNC ? ISOLATE_ASYNC_MIGRATE : 0);

	/*
	 * Start at where we last stopped, or beginning of the zone as
	 * initialized by compact_zone()
	 */

/* IAMROOT-12:
 * -------------
 * 1개의 페이지블럭에 대해서 low_pfn ~ end_pfn 까지 옮길 수 있는 페이지를 
 * cc->migratepages 리스트에 추가한다. 1개 이상의 페이지가 담기는 경우 
 * success를 반환한다. (이 페이지 블럭을 다 스캔한 경우 skip flag를 설정한다)
 */
	low_pfn = cc->migrate_pfn;

	/* Only scan within a pageblock boundary */
	end_pfn = ALIGN(low_pfn + 1, pageblock_nr_pages);

	/*
	 * Iterate over whole pageblocks until we find the first suitable.
	 * Do not cross the free scanner.
	 */
	for (; end_pfn <= cc->free_pfn;
			low_pfn = end_pfn, end_pfn += pageblock_nr_pages) {

		/*
		 * This can potentially iterate a massively long zone with
		 * many pageblocks unsuitable, so periodically check if we
		 * need to schedule, or even abort async compaction.
		 */

/* IAMROOT-12:
 * -------------
 * 대략 32개 페이지 블럭 처리 마다 async 모드에서 리스케쥴 요청이 있는지 
 * 확인하여 루프를 탈출한다.
 */
		if (!(low_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;

/* IAMROOT-12:
 * -------------
 * 페이지블럭이 zone 경계에 물려있는 경우 skip 한다.
 */
		page = pageblock_pfn_to_page(low_pfn, end_pfn, zone);
		if (!page)
			continue;

		/* If isolation recently failed, do not retry */
/* IAMROOT-12:
 * -------------
 * 해당 페이지의 페이지블럭에 skip bit가 설정된 경우 isolation을 하지 않도록
 * 한다. 단 cc->ignore_skip_hint가 설정된 경우 무조건 isolation을 하도록 한다.
 */
		if (!isolation_suitable(cc, page))
			continue;

		/*
		 * For async compaction, also only scan in MOVABLE blocks.
		 * Async compaction is optimistic to see if the minimum amount
		 * of work satisfies the allocation.
		 */
/* IAMROOT-12:
 * -------------
 * cma 타입이나 movable 타입에 대해서만 async migration을 지원한다.
 * sync migration의 경우는 migrate 타입에 상관없이 migration을 수행한다.
 */
		if (cc->mode == MIGRATE_ASYNC &&
		    !migrate_async_suitable(get_pageblock_migratetype(page)))
			continue;

		/* Perform the isolation */
/* IAMROOT-12:
 * -------------
 * 한 개의 페이지블럭에서 옮길 수 있는 페이지 들을 찾아 cc->migratepages에 
 * 추가한다. (최대 한 번에 32개 페이지)
 */
		low_pfn = isolate_migratepages_block(cc, low_pfn, end_pfn,
								isolate_mode);
/* IAMROOT-12:
 * -------------
 * 혼잡하거나 abort된 경우 ISOLATE_ABORT를 반환한다.
 */
		if (!low_pfn || cc->contended) {
			acct_isolated(zone, cc);
			return ISOLATE_ABORT;
		}

		/*
		 * Either we isolated something and proceed with migration. Or
		 * we failed and compact_zone should decide if we should
		 * continue or not.
		 */
		break;
	}

	acct_isolated(zone, cc);
	/*
	 * Record where migration scanner will be restarted. If we end up in
	 * the same pageblock as the free scanner, make the scanners fully
	 * meet so that compact_finished() terminates compaction.
	 */

/* IAMROOT-12:
 * -------------
 * 현재 처리한 페이지블럭이 free 스캐너보다 아래에 있는 경우 migrate 스캐너의 
 * 시작 pfn에 low_pfn을 대입시켜 중단된 위치부터 계속 스캔할 수 있도록 한다.
 * 그렇지 않은 경우 cc->migrate_pfn에 cc->free_pfn을 대입하여 종료됨을 
 * 알 수 있다.
 */
	cc->migrate_pfn = (end_pfn <= cc->free_pfn) ? low_pfn : cc->free_pfn;


/* IAMROOT-12:
 * -------------
 * migrate 페이지가 1개 이상인 경우 SUCCESS를 반환한다.
 */
	return cc->nr_migratepages ? ISOLATE_SUCCESS : ISOLATE_NONE;
}

static int __compact_finished(struct zone *zone, struct compact_control *cc,
			    const int migratetype)
{
	unsigned int order;
	unsigned long watermark;

	if (cc->contended || fatal_signal_pending(current))
		return COMPACT_PARTIAL;

	/* Compaction run completes if the migrate and free scanner meet */

/* IAMROOT-12:
 * -------------
 * migrate 스캐너와 free 스캐너가 중간에서 만났을 때 다시 시작 위치를 zone의
 * 시작과 끝으로 재설정하고 COMPACT_COMPLETE를 반환한다.
 */
	if (cc->free_pfn <= cc->migrate_pfn) {
		/* Let the next compaction start anew. */
		zone->compact_cached_migrate_pfn[0] = zone->zone_start_pfn;
		zone->compact_cached_migrate_pfn[1] = zone->zone_start_pfn;
		zone->compact_cached_free_pfn = zone_end_pfn(zone);

		/*
		 * Mark that the PG_migrate_skip information should be cleared
		 * by kswapd when it goes to sleep. kswapd does not set the
		 * flag itself as the decision to be clear should be directly
		 * based on an allocation request.
		 */

/* IAMROOT-12:
 * -------------
 * kswapd가 페이지 할당을 요청한 경우가 아닌 다른 곳에서 메모리 할당 요청인 경우
 * 에는 kswapd가 sleep할 때 각 페이지블럭의 skip bit를 클리어하는데 이를 하지
 * 않도록 요청한다.
 */
		if (!current_is_kswapd())
			zone->compact_blockskip_flush = true;

		return COMPACT_COMPLETE;
	}

	/*
	 * order == -1 is expected when compacting via
	 * /proc/sys/vm/compact_memory
	 */

/* IAMROOT-12:
 * -------------
 * 수동 compaction 요청인 경우
 */
	if (cc->order == -1)
		return COMPACT_CONTINUE;

	/* Compaction run is not finished if the watermark is not met */
	watermark = low_wmark_pages(zone);

	if (!zone_watermark_ok(zone, cc->order, watermark, cc->classzone_idx,
							cc->alloc_flags))
		return COMPACT_CONTINUE;

/* IAMROOT-12:
 * -------------
 * 현재 이 시점에서 확보한 free 페이지가 low 워터마크 이상인 경우 
 * 요청한 order 페이지가 있는 경우 compaction이 다 완료되지 않았지만
 * compaction을 통해 요청한 만큼의 페이지를 확보했다는 의미로 
 * COMPACT_PARTIAL을 반환한다.
 */
	/* Direct compactor: Is a suitable page free? */
	for (order = cc->order; order < MAX_ORDER; order++) {
		struct free_area *area = &zone->free_area[order];

		/* Job done if page is free of the right migratetype */
		if (!list_empty(&area->free_list[migratetype]))
			return COMPACT_PARTIAL;

		/* Job done if allocation would set block type */
		if (order >= pageblock_order && area->nr_free)
			return COMPACT_PARTIAL;
	}

	return COMPACT_NO_SUITABLE_PAGE;
}

static int compact_finished(struct zone *zone, struct compact_control *cc,
			    const int migratetype)
{
	int ret;

/* IAMROOT-12:
 * -------------
 * compaction을 끝내도 되는지 확인한다. 
 * (COMPACT_PARTIAL인 경우 페이지를 확보한 케이스이다)
 */
	ret = __compact_finished(zone, cc, migratetype);
	trace_mm_compaction_finished(zone, cc->order, ret);
	if (ret == COMPACT_NO_SUITABLE_PAGE)
		ret = COMPACT_CONTINUE;

	return ret;
}

/*
 * compaction_suitable: Is this suitable to run compaction on this zone now?
 * Returns
 *   COMPACT_SKIPPED  - If there are too few free pages for compaction
 *   COMPACT_PARTIAL  - If the allocation would succeed without compaction
 *   COMPACT_CONTINUE - If compaction should run now
 */
static unsigned long __compaction_suitable(struct zone *zone, int order,
					int alloc_flags, int classzone_idx)
{
	int fragindex;
	unsigned long watermark;

	/*
	 * order == -1 is expected when compacting via
	 * /proc/sys/vm/compact_memory
	 */

/* IAMROOT-12:
 * -------------
 * "echo 1 > /proc/ss/vm/compact_memory"를 수행하면 강제로 compaction을 수행한다.
 */
	if (order == -1)
		return COMPACT_CONTINUE;

	watermark = low_wmark_pages(zone);
	/*
	 * If watermarks for high-order allocation are already met, there
	 * should be no need for compaction at all.
	 */

/* IAMROOT-12:
 * -------------
 * 회수된 free 페이지가 low 워터마크를 초과 및 order 페이지가 있는 경우 skip
 */
	if (zone_watermark_ok(zone, order, watermark, classzone_idx,
								alloc_flags))
		return COMPACT_PARTIAL;

	/*
	 * Watermarks for order-0 must be met for compaction. Note the 2UL.
	 * This is because during migration, copies of pages need to be
	 * allocated and for a short time, the footprint is higher
	 */

/* IAMROOT-12:
 * -------------
 * migration 하는 동안 order의 두 배 페이지 수만큼 잠시 짧은 시간동안 내부에서
 * 할당을 하여 사용하므로 워터마크 기준을 상향시켜서 워터마크 기준을 통과하는지 
 * 체크한다.
 */
	watermark += (2UL << order);
	if (!zone_watermark_ok(zone, 0, watermark, classzone_idx, alloc_flags))
		return COMPACT_SKIPPED;

	/*
	 * fragmentation index determines if allocation failures are due to
	 * low memory or external fragmentation
	 *
	 * index of -1000 would imply allocations might succeed depending on
	 * watermarks, but we already failed the high-order watermark check
	 * index towards 0 implies failure is due to lack of memory
	 * index towards 1000 implies failure is due to fragmentation
	 *
	 * Only compact if a failure would be due to fragmentation.
	 */

/* IAMROOT-12:
 * -------------
 * 요청 order로 compaction 할 때 성공 확률을 판단하기 위해 요청 order에 대한 
 * 파편화 인덱스 값을 알아와서 그 값이 "/proc/sys/vm/extfrag_threshold" 
 * 값(default=500)을 초과하는 경우 compaction을 시도하게 한다.
 */
	fragindex = fragmentation_index(zone, order);
	if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold)
		return COMPACT_NOT_SUITABLE_ZONE;

	return COMPACT_CONTINUE;
}

unsigned long compaction_suitable(struct zone *zone, int order,
					int alloc_flags, int classzone_idx)
{
	unsigned long ret;

/* IAMROOT-12:
 * -------------
 * compaction 시도를 위한 조건(파편화 인덱스 비교)을 만족시키는지 알아온다.
 * (COMPACT_NOT_SUITABLE_ZONE = 불만족하여 skip을 반환한댜)
 */
	ret = __compaction_suitable(zone, order, alloc_flags, classzone_idx);
	trace_mm_compaction_suitable(zone, order, ret);
	if (ret == COMPACT_NOT_SUITABLE_ZONE)
		ret = COMPACT_SKIPPED;

	return ret;
}

static int compact_zone(struct zone *zone, struct compact_control *cc)
{
	int ret;
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	const int migratetype = gfpflags_to_migratetype(cc->gfp_mask);
	const bool sync = cc->mode != MIGRATE_ASYNC;
	unsigned long last_migrated_pfn = 0;

/* IAMROOT-12:
 * -------------
 * compaction 시도를 위한 조건(파편화 인덱스 비교)을 만족시키는지 알아온다.
 *   - COMPACT_PARTIAL: 비교 전에 이미 order에 맞는 free 페이지가 생겼다.
 *   - COMPACT_SKIPPED: 파편화 인덱스 비교하여 적절하지 못하여 skip 
 *   - COMPACT_CONTINUE: 파편화 인덱스 비교하여 적절하므로 compactio 시도 
 */
	ret = compaction_suitable(zone, cc->order, cc->alloc_flags,
							cc->classzone_idx);
	switch (ret) {
	case COMPACT_PARTIAL:
	case COMPACT_SKIPPED:
		/* Compaction is likely to fail */
		return ret;
	case COMPACT_CONTINUE:
		/* Fall through to compaction */
		;
	}

	/*
	 * Clear pageblock skip if there were failures recently and compaction
	 * is about to be retried after being deferred. kswapd does not do
	 * this reset as it'll reset the cached information when going to sleep.
	 */

/* IAMROOT-12:
 * -------------
 * compaction 실패가 마지막 횟수까지 도달하였고 kswapd가 활동 상태가 아닌 경우
 * compaction을 다시 처음부터 하기 위해 zone의 usemap(pageblock_flags)에서 
 * 모든 PB_migrate_skip 비트를 clear한다.
 */
	if (compaction_restarting(zone, cc->order) && !current_is_kswapd())
		__reset_isolation_suitable(zone);

	/*
	 * Setup to move all movable pages to the end of the zone. Used cached
	 * information on where the scanners should start but check that it
	 * is initialised by ensuring the values are within zone boundaries.
	 */

/* IAMROOT-12:
 * -------------
 * 이 루틴에 들어올 때 마다 zone에 저장된 migrate/free 스캐너의 위치를 알아온다.
 */
	cc->migrate_pfn = zone->compact_cached_migrate_pfn[sync];
	cc->free_pfn = zone->compact_cached_free_pfn;

/* IAMROOT-12:
 * -------------
 * free 스캐너가 zone 범위를 벗어나는 경우 페이지 블럭단위로 정렬한 end pfn
 * 값으로 설정한다.
 */
	if (cc->free_pfn < start_pfn || cc->free_pfn > end_pfn) {
		cc->free_pfn = end_pfn & ~(pageblock_nr_pages-1);
		zone->compact_cached_free_pfn = cc->free_pfn;
	}

/* IAMROOT-12:
 * -------------
 * migrate 스캐너가 zone 범위를 벗어나는 경우 페이지 블럭단위로 정렬한 end pfn
 * 값으로 설정한다.
 */
	if (cc->migrate_pfn < start_pfn || cc->migrate_pfn > end_pfn) {
		cc->migrate_pfn = start_pfn;
		zone->compact_cached_migrate_pfn[0] = cc->migrate_pfn;
		zone->compact_cached_migrate_pfn[1] = cc->migrate_pfn;
	}

	trace_mm_compaction_begin(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync);

/* IAMROOT-12:
 * -------------
 * per-cpu로 관리되는 아래 4개의 pagevec 리스트에서 관리되는 페이지를 회수하여 
 * lruvec로 옮긴다.
 *	- lru_add_pvec
 *	- lru_rotate_pvecs
 *	- lru_deactivate_pvecs
 *	- activate_page_pvecs
 */
	migrate_prep_local();

	while ((ret = compact_finished(zone, cc, migratetype)) ==
						COMPACT_CONTINUE) {
		int err;
		unsigned long isolate_start_pfn = cc->migrate_pfn;

/* IAMROOT-12:
 * -------------
 * isolation 시작
 */
		switch (isolate_migratepages(zone, cc)) {
		case ISOLATE_ABORT:
			ret = COMPACT_PARTIAL;

/* IAMROOT-12:
 * -------------
 * cc->migratepages로 옮긴 모든 페이지들을 원래 lru로 원위치 시킨다.
 */
			putback_movable_pages(&cc->migratepages);
			cc->nr_migratepages = 0;
			goto out;
		case ISOLATE_NONE:
			/*
			 * We haven't isolated and migrated anything, but
			 * there might still be unflushed migrations from
			 * previous cc->order aligned block.
			 */
			goto check_drain;
		case ISOLATE_SUCCESS:
			;
		}

/* IAMROOT-12:
 * -------------
 * migration을 수행한다. (cc->migratepages -> cc->freepages)
 */
		err = migrate_pages(&cc->migratepages, compaction_alloc,
				compaction_free, (unsigned long)cc, cc->mode,
				MR_COMPACTION);

		trace_mm_compaction_migratepages(cc->nr_migratepages, err,
							&cc->migratepages);

		/* All pages were either migrated or will be released */

/* IAMROOT-12:
 * -------------
 * 처리가 되지 않고 남아 있는 migratepages들을 lru 캐시를 통해 lruvec으로 되돌려놓는다.
 */
		cc->nr_migratepages = 0;
		if (err) {
			putback_movable_pages(&cc->migratepages);
			/*
			 * migrate_pages() may return -ENOMEM when scanners meet
			 * and we want compact_finished() to detect it
			 */
			if (err == -ENOMEM && cc->free_pfn > cc->migrate_pfn) {
				ret = COMPACT_PARTIAL;
				goto out;
			}
		}

		/*
		 * Record where we could have freed pages by migration and not
		 * yet flushed them to buddy allocator. We use the pfn that
		 * isolate_migratepages() started from in this loop iteration
		 * - this is the lowest page that could have been isolated and
		 * then freed by migration.
		 */
		if (!last_migrated_pfn)
			last_migrated_pfn = isolate_start_pfn;

check_drain:
		/*
		 * Has the migration scanner moved away from the previous
		 * cc->order aligned block where we migrated from? If yes,
		 * flush the pages that were freed, so that they can merge and
		 * compact_finished() can detect immediately if allocation
		 * would succeed.
		 */
		if (cc->order > 0 && last_migrated_pfn) {
			int cpu;

/* IAMROOT-12:
 * -------------
 * cc->migrate_pfn을 cc->order 단위로 절삭 정렬
 */
			unsigned long current_block_start =
				cc->migrate_pfn & ~((1UL << cc->order) - 1);

			if (last_migrated_pfn < current_block_start) {
				cpu = get_cpu();
/* IAMROOT-12:
 * -------------
 * zone에 있는 order-0 버디 시스템으로 pcp에 있는 order-0 페이지들을 
 * 보내줘야 free 페이지들이 짝을 이루어 상향 combine 되면서 order가 
 * 상향된다. 이렇게 해줘야 요청한 order 페이지에 대한 성공률이 
 * 높아진다.
 *
 * lru per-cpu 캐시에 있는 페이지들을 lruvec에 모두 옮긴다.
 * pcp per-cpu 캐시에 있는 페이지들을 버디시스템에 모두 옮긴다. 
 */
				lru_add_drain_cpu(cpu);
				drain_local_pages(zone);
				put_cpu();
				/* No more flushing until we migrate again */
				last_migrated_pfn = 0;
			}
		}

	}

out:
	/*
	 * Release free pages and update where the free scanner should restart,
	 * so we don't leave any returned pages behind in the next attempt.
	 */
	if (cc->nr_freepages > 0) {

/* IAMROOT-12:
 * -------------
 * free 스캐너가 isolation한 페이지들을 다시 버디시스템으로 돌려보낸다.
 * (free_pfn: free 시킨 페이지 중 가장 상위 pfn)
 */
		unsigned long free_pfn = release_freepages(&cc->freepages);

		cc->nr_freepages = 0;
		VM_BUG_ON(free_pfn == 0);
		/* The cached pfn is always the first in a pageblock */
		free_pfn &= ~(pageblock_nr_pages-1);
		/*
		 * Only go back, not forward. The cached pfn might have been
		 * already reset to zone end in compact_finished()
		 */

/* IAMROOT-12:
 * -------------
 * 다음 scan시 시작할 free 스캐너 위치를 기억한다.
 */
		if (free_pfn > zone->compact_cached_free_pfn)
			zone->compact_cached_free_pfn = free_pfn;
	}

	trace_mm_compaction_end(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync, ret);

	return ret;
}

static unsigned long compact_zone_order(struct zone *zone, int order,
		gfp_t gfp_mask, enum migrate_mode mode, int *contended,
		int alloc_flags, int classzone_idx)
{
	unsigned long ret;

/* IAMROOT-12:
 * -------------
 * compact 요청 첫 시도시 nr_freepages가 0부터 시작
 */
	struct compact_control cc = {
		.nr_freepages = 0,
		.nr_migratepages = 0,
		.order = order,
		.gfp_mask = gfp_mask,
		.zone = zone,
		.mode = mode,
		.alloc_flags = alloc_flags,
		.classzone_idx = classzone_idx,
	};
	INIT_LIST_HEAD(&cc.freepages);
	INIT_LIST_HEAD(&cc.migratepages);

	ret = compact_zone(zone, &cc);

	VM_BUG_ON(!list_empty(&cc.freepages));
	VM_BUG_ON(!list_empty(&cc.migratepages));

	*contended = cc.contended;
	return ret;
}

int sysctl_extfrag_threshold = 500;

/**
 * try_to_compact_pages - Direct compact to satisfy a high-order allocation
 * @gfp_mask: The GFP mask of the current allocation
 * @order: The order of the current allocation
 * @alloc_flags: The allocation flags of the current allocation
 * @ac: The context of current allocation
 * @mode: The migration mode for async, sync light, or sync migration
 * @contended: Return value that determines if compaction was aborted due to
 *	       need_resched() or lock contention
 *
 * This is the main entry point for direct page compaction.
 */
unsigned long try_to_compact_pages(gfp_t gfp_mask, unsigned int order,
			int alloc_flags, const struct alloc_context *ac,
			enum migrate_mode mode, int *contended)
{
	int may_enter_fs = gfp_mask & __GFP_FS;
	int may_perform_io = gfp_mask & __GFP_IO;
	struct zoneref *z;
	struct zone *zone;
	int rc = COMPACT_DEFERRED;
	int all_zones_contended = COMPACT_CONTENDED_LOCK; /* init for &= op */

	*contended = COMPACT_CONTENDED_NONE;

	/* Check if the GFP flags allow compaction */
/* IAMROOT-12:
 * -------------
 * order 0 페이지는 compaction을 해도 소용 없다. 또한 fs/io 요청이 없는 경우 skip 한다.
 */
	if (!order || !may_enter_fs || !may_perform_io)
		return COMPACT_SKIPPED;

	trace_mm_compaction_try_to_compact_pages(order, gfp_mask, mode);

	/* Compact each zone in the list */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
								ac->nodemask) {
		int status;
		int zone_contended;

/* IAMROOT-12:
 * -------------
 * compaction 유예 상황이면 skip
 */
		if (compaction_deferred(zone, order))
			continue;

		status = compact_zone_order(zone, order, gfp_mask, mode,
				&zone_contended, alloc_flags,
				ac->classzone_idx);
		rc = max(status, rc);
		/*
		 * It takes at least one zone that wasn't lock contended
		 * to clear all_zones_contended.
		 */

/* IAMROOT-12:
 * -------------
 * zonelist를 돌면서 모든 후보 zone에서 하나라도 contended 상황이 없으면 클리어된다.
 */
		all_zones_contended &= zone_contended;

		/* If a normal allocation would succeed, stop compacting */
		if (zone_watermark_ok(zone, order, low_wmark_pages(zone),
					ac->classzone_idx, alloc_flags)) {
			/*
			 * We think the allocation will succeed in this zone,
			 * but it is not certain, hence the false. The caller
			 * will repeat this with true if allocation indeed
			 * succeeds in this zone.
			 */

/* IAMROOT-12:
 * -------------
 * low 워터마크 기준을 충족하는 경우 요청 order가 compaction 실패 order 값보다 
 * 같거나 큰 경우 요청 order +1을 설정한다.
 */
			compaction_defer_reset(zone, order, false);
			/*
			 * It is possible that async compaction aborted due to
			 * need_resched() and the watermarks were ok thanks to
			 * somebody else freeing memory. The allocation can
			 * however still fail so we better signal the
			 * need_resched() contention anyway (this will not
			 * prevent the allocation attempt).
			 */
			if (zone_contended == COMPACT_CONTENDED_SCHED)
				*contended = COMPACT_CONTENDED_SCHED;

			goto break_loop;
		}

		if (mode != MIGRATE_ASYNC && status == COMPACT_COMPLETE) {
			/*
			 * We think that allocation won't succeed in this zone
			 * so we defer compaction there. If it ends up
			 * succeeding after all, it will be reset.
			 */

/* IAMROOT-12:
 * -------------
 * async 모드가 아니면서 compact 완료시 defer를 최대 6까지 증가시킨다.
 * ("echo 1 > /proc/sys/vm/compact_memory"로 manual compaction을 시도할 때
 * MIGRATE_SYNC 모드로 compaction을 진행하는데 이 때 유예 시스템을 가동한다)
 */
			defer_compaction(zone, order);
		}

		/*
		 * We might have stopped compacting due to need_resched() in
		 * async compaction, or due to a fatal signal detected. In that
		 * case do not try further zones and signal need_resched()
		 * contention.
		 */
		if ((zone_contended == COMPACT_CONTENDED_SCHED)
					|| fatal_signal_pending(current)) {
			*contended = COMPACT_CONTENDED_SCHED;
			goto break_loop;
		}

		continue;
break_loop:
		/*
		 * We might not have tried all the zones, so  be conservative
		 * and assume they are not all lock contended.
		 */
		all_zones_contended = 0;
		break;
	}

	/*
	 * If at least one zone wasn't deferred or skipped, we report if all
	 * zones that were tried were lock contended.
	 */
	if (rc > COMPACT_SKIPPED && all_zones_contended)
		*contended = COMPACT_CONTENDED_LOCK;

	return rc;
}


/* Compact all zones within a node */
static void __compact_pgdat(pg_data_t *pgdat, struct compact_control *cc)
{
	int zoneid;
	struct zone *zone;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		cc->nr_freepages = 0;
		cc->nr_migratepages = 0;
		cc->zone = zone;
		INIT_LIST_HEAD(&cc->freepages);
		INIT_LIST_HEAD(&cc->migratepages);

		if (cc->order == -1 || !compaction_deferred(zone, cc->order))
			compact_zone(zone, cc);

		if (cc->order > 0) {
			if (zone_watermark_ok(zone, cc->order,
						low_wmark_pages(zone), 0, 0))
				compaction_defer_reset(zone, cc->order, false);
		}

		VM_BUG_ON(!list_empty(&cc->freepages));
		VM_BUG_ON(!list_empty(&cc->migratepages));
	}
}

void compact_pgdat(pg_data_t *pgdat, int order)
{
	struct compact_control cc = {
		.order = order,
		.mode = MIGRATE_ASYNC,
	};

	if (!order)
		return;

	__compact_pgdat(pgdat, &cc);
}

static void compact_node(int nid)
{
	struct compact_control cc = {
		.order = -1,
		.mode = MIGRATE_SYNC,
		.ignore_skip_hint = true,
	};

	__compact_pgdat(NODE_DATA(nid), &cc);
}

/* Compact all nodes in the system */
static void compact_nodes(void)
{
	int nid;

	/* Flush pending updates to the LRU lists */
	lru_add_drain_all();

	for_each_online_node(nid)
		compact_node(nid);
}

/* The written value is actually unused, all memory is compacted */
int sysctl_compact_memory;

/* This is the entry point for compacting all nodes via /proc/sys/vm */
int sysctl_compaction_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos)
{
	if (write)
		compact_nodes();

	return 0;
}

int sysctl_extfrag_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec_minmax(table, write, buffer, length, ppos);

	return 0;
}

#if defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
static ssize_t sysfs_compact_node(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int nid = dev->id;

	if (nid >= 0 && nid < nr_node_ids && node_online(nid)) {
		/* Flush pending updates to the LRU lists */
		lru_add_drain_all();

		compact_node(nid);
	}

	return count;
}
static DEVICE_ATTR(compact, S_IWUSR, NULL, sysfs_compact_node);

int compaction_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_compact);
}

void compaction_unregister_node(struct node *node)
{
	return device_remove_file(&node->dev, &dev_attr_compact);
}
#endif /* CONFIG_SYSFS && CONFIG_NUMA */

#endif /* CONFIG_COMPACTION */
