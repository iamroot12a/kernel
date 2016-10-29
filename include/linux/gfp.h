#ifndef __LINUX_GFP_H
#define __LINUX_GFP_H

#include <linux/mmdebug.h>
#include <linux/mmzone.h>
#include <linux/stddef.h>
#include <linux/linkage.h>
#include <linux/topology.h>

struct vm_area_struct;

/* Plain integer GFP bitmasks. Do not use this directly. */

/* IAMROOT-12AB:
 * -------------
 * 아래 4개는 zone을 나타낸다.
 */
#define ___GFP_DMA		0x01u
#define ___GFP_HIGHMEM		0x02u
#define ___GFP_DMA32		0x04u
#define ___GFP_MOVABLE		0x08u

/* IAMROOT-12AB:
 * -------------
 * 할당 중 wait(sleep) 가능
 * - 인터럽트 핸들러에서는 사용 못함
 */
#define ___GFP_WAIT		0x10u

/* IAMROOT-12AB:
 * -------------
 * 높은 우선 순위로 할당 요청
 */
#define ___GFP_HIGH		0x20u

/* IAMROOT-12AB:
 * -------------
 * 할당 중 IO 요청 가능 
 */
#define ___GFP_IO		0x40u

/* IAMROOT-12AB:
 * -------------
 * 할당 중 File System Call 사용 가능
 */
#define ___GFP_FS		0x80u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 cold(리스트의 tail에 있는) 페이지를 할당 받는다.
 */
#define ___GFP_COLD		0x100u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 경고 출력하지 않도록 한다.
 */
#define ___GFP_NOWARN		0x200u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 메모리가 부족한 경우 1번 더 시도하게 한다.
 */
#define ___GFP_REPEAT		0x400u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 메모리가 부족한 경우 성공할 때까지 시도하게 한다.
 */
#define ___GFP_NOFAIL		0x800u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 실패하는 경우 그냥 포기한다.
 */
#define ___GFP_NORETRY		0x1000u

/* IAMROOT-12AB:
 * -------------
 * 할당을 비상 영역에서 할 수 있도록 한다.
 * (커널 내부에서 compaction 또는 reclaim 관련하여 사용)
 */
#define ___GFP_MEMALLOC		0x2000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 컴파운드(복합) 페이지를 구성하게 한다.
 * 컴파운드 페이지는 메타데이터를 가질 수 있다.
 * (page descriptors에서 메타데이터의 위치를 지정하고
 *  생성자와 소멸자도 지정할 수 있다)
 */
#define ___GFP_COMP		0x4000u

/* IAMROOT-12AB:
 * -------------
 * 할당된 메모리를 0으로 초기화 시킨다.
 */
#define ___GFP_ZERO		0x8000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 비상 영역을 사용하지 못하도록 한다.
 */
#define ___GFP_NOMEMALLOC	0x10000u

/* IAMROOT-12AB:
 * -------------
 * 
 */
#define ___GFP_HARDWALL		0x20000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 NUMA의 경우 현재 노드에서만 허용하게 한다.
 */
#define ___GFP_THISNODE		0x40000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 회수 가능한 페이지로 할당한다.
 * (file 캐시 등에서 사용한다.)
 */
#define ___GFP_RECLAIMABLE	0x80000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 Control Group에서 사용량등을 통제하지 못하게 한다.
 */
#define ___GFP_NOACCOUNT	0x100000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 디버그 트레이싱을 하지 못하게 한다.
 */
#define ___GFP_NOTRACK		0x200000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 kswapd에 의해 swap 파일로 이동하지 못하게 한다.
 */
#define ___GFP_NO_KSWAPD	0x400000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 다른 노드에서 할당하도록 한다.
 */
#define ___GFP_OTHER_NODE	0x800000u

/* IAMROOT-12AB:
 * -------------
 * 할당 시 dirty 페이지로 할당하도록 한다.
 */
#define ___GFP_WRITE		0x1000000u
/* If the above are modified, __GFP_BITS_SHIFT may need updating */

/*
 * GFP bitmasks..
 *
 * Zone modifiers (see linux/mmzone.h - low three bits)
 *
 * Do not put any conditional on these. If necessary modify the definitions
 * without the underscores and use them consistently. The definitions here may
 * be used in bit comparisons.
 */
#define __GFP_DMA	((__force gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM	((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32	((__force gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE	((__force gfp_t)___GFP_MOVABLE)  /* Page is movable */
#define GFP_ZONEMASK	(__GFP_DMA|__GFP_HIGHMEM|__GFP_DMA32|__GFP_MOVABLE)
/*
 * Action modifiers - doesn't change the zoning
 *
 * __GFP_REPEAT: Try hard to allocate the memory, but the allocation attempt
 * _might_ fail.  This depends upon the particular VM implementation.
 *
 * __GFP_NOFAIL: The VM implementation _must_ retry infinitely: the caller
 * cannot handle allocation failures.  This modifier is deprecated and no new
 * users should be added.
 *
 * __GFP_NORETRY: The VM implementation must not retry indefinitely.
 *
 * __GFP_MOVABLE: Flag that this page will be movable by the page migration
 * mechanism or reclaimed
 */

/* IAMROOT-12:
 * -------------
 * __GFP_THISNODE:
 *      지정된 노드의 zonelist[1]번을 사용하여 다른 노드의 zone을 사용하지 않게 한다.
 */

#define __GFP_WAIT	((__force gfp_t)___GFP_WAIT)	/* Can wait and reschedule? */
#define __GFP_HIGH	((__force gfp_t)___GFP_HIGH)	/* Should access emergency pools? */
#define __GFP_IO	((__force gfp_t)___GFP_IO)	/* Can start physical IO? */
#define __GFP_FS	((__force gfp_t)___GFP_FS)	/* Can call down to low-level FS? */
#define __GFP_COLD	((__force gfp_t)___GFP_COLD)	/* Cache-cold page required */
#define __GFP_NOWARN	((__force gfp_t)___GFP_NOWARN)	/* Suppress page allocation failure warning */
#define __GFP_REPEAT	((__force gfp_t)___GFP_REPEAT)	/* See above */
#define __GFP_NOFAIL	((__force gfp_t)___GFP_NOFAIL)	/* See above */
#define __GFP_NORETRY	((__force gfp_t)___GFP_NORETRY) /* See above */
#define __GFP_MEMALLOC	((__force gfp_t)___GFP_MEMALLOC)/* Allow access to emergency reserves */
#define __GFP_COMP	((__force gfp_t)___GFP_COMP)	/* Add compound page metadata */
#define __GFP_ZERO	((__force gfp_t)___GFP_ZERO)	/* Return zeroed page on success */
#define __GFP_NOMEMALLOC ((__force gfp_t)___GFP_NOMEMALLOC) /* Don't use emergency reserves.
							 * This takes precedence over the
							 * __GFP_MEMALLOC flag if both are
							 * set
							 */
#define __GFP_HARDWALL   ((__force gfp_t)___GFP_HARDWALL) /* Enforce hardwall cpuset memory allocs */
#define __GFP_THISNODE	((__force gfp_t)___GFP_THISNODE)/* No fallback, no policies */
#define __GFP_RECLAIMABLE ((__force gfp_t)___GFP_RECLAIMABLE) /* Page is reclaimable */
#define __GFP_NOACCOUNT	((__force gfp_t)___GFP_NOACCOUNT) /* Don't account to kmemcg */
#define __GFP_NOTRACK	((__force gfp_t)___GFP_NOTRACK)  /* Don't track with kmemcheck */

#define __GFP_NO_KSWAPD	((__force gfp_t)___GFP_NO_KSWAPD)
#define __GFP_OTHER_NODE ((__force gfp_t)___GFP_OTHER_NODE) /* On behalf of other node */
#define __GFP_WRITE	((__force gfp_t)___GFP_WRITE)	/* Allocator intends to dirty page */

/*
 * This may seem redundant, but it's a way of annotating false positives vs.
 * allocations that simply cannot be supported (e.g. page tables).
 */
#define __GFP_NOTRACK_FALSE_POSITIVE (__GFP_NOTRACK)

#define __GFP_BITS_SHIFT 25	/* Room for N __GFP_FOO bits */
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

/* This equals 0, but use constants in case they ever change */
#define GFP_NOWAIT	(GFP_ATOMIC & ~__GFP_HIGH)
/* GFP_ATOMIC means both !wait (__GFP_WAIT not set) and use emergency pool */
#define GFP_ATOMIC	(__GFP_HIGH)
#define GFP_NOIO	(__GFP_WAIT)
#define GFP_NOFS	(__GFP_WAIT | __GFP_IO)
#define GFP_KERNEL	(__GFP_WAIT | __GFP_IO | __GFP_FS)
#define GFP_TEMPORARY	(__GFP_WAIT | __GFP_IO | __GFP_FS | \
			 __GFP_RECLAIMABLE)
#define GFP_USER	(__GFP_WAIT | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE)
#define GFP_IOFS	(__GFP_IO | __GFP_FS)
#define GFP_TRANSHUGE	(GFP_HIGHUSER_MOVABLE | __GFP_COMP | \
			 __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN | \
			 __GFP_NO_KSWAPD)

/*
 * GFP_THISNODE does not perform any reclaim, you most likely want to
 * use __GFP_THISNODE to allocate from a given node without fallback!
 */
#ifdef CONFIG_NUMA
#define GFP_THISNODE	(__GFP_THISNODE | __GFP_NOWARN | __GFP_NORETRY)
#else
#define GFP_THISNODE	((__force gfp_t)0)
#endif

/* This mask makes up all the page movable related flags */
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE|__GFP_MOVABLE)

/* Control page allocator reclaim behavior */
#define GFP_RECLAIM_MASK (__GFP_WAIT|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_REPEAT|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_MEMALLOC|__GFP_NOMEMALLOC)

/* Control slab gfp mask during early boot */

/* IAMROOT-12:
 * -------------
 * 부트 프로세스 동작중에 전체 GFP 비트마스크중 3개(WAIT, IO, FS)를 제거
 * 아직 스케쥴러가 동작하지 않고, 디바이스 입출력 장치도 준비가 안된 
 * 상태이기 때문에 해당 기능들을 제거하게 만든다.
 */
#define GFP_BOOT_MASK (__GFP_BITS_MASK & ~(__GFP_WAIT|__GFP_IO|__GFP_FS))

/* Control allocation constraints */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

/* Flag - indicates that the buffer will be suitable for DMA.  Ignored on some
   platforms, used as appropriate on others */

#define GFP_DMA		__GFP_DMA

/* 4GB DMA on some platforms */
#define GFP_DMA32	__GFP_DMA32

/* Convert GFP flags to their corresponding migrate type */
static inline int gfpflags_to_migratetype(const gfp_t gfp_flags)
{
/* IAMROOT-12:
 * -------------
 * gfp 마스크를 해석하여 mobility를 반환한다.
 *      0=MIGRATE_UNMOVABLE
 *      1=MIGRATE_RECLAIMABLE
 *      2=MIGRATE_MOVABLE
 */
	WARN_ON((gfp_flags & GFP_MOVABLE_MASK) == GFP_MOVABLE_MASK);

	if (unlikely(page_group_by_mobility_disabled))
		return MIGRATE_UNMOVABLE;

	/* Group based on mobility */
	return (((gfp_flags & __GFP_MOVABLE) != 0) << 1) |
		((gfp_flags & __GFP_RECLAIMABLE) != 0);
}

#ifdef CONFIG_HIGHMEM
#define OPT_ZONE_HIGHMEM ZONE_HIGHMEM
#else
#define OPT_ZONE_HIGHMEM ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA
#define OPT_ZONE_DMA ZONE_DMA
#else
#define OPT_ZONE_DMA ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA32
#define OPT_ZONE_DMA32 ZONE_DMA32
#else
#define OPT_ZONE_DMA32 ZONE_NORMAL
#endif

/*
 * GFP_ZONE_TABLE is a word size bitstring that is used for looking up the
 * zone to use given the lowest 4 bits of gfp_t. Entries are ZONE_SHIFT long
 * and there are 16 of them to cover all possible combinations of
 * __GFP_DMA, __GFP_DMA32, __GFP_MOVABLE and __GFP_HIGHMEM.
 *
 * The zone fallback order is MOVABLE=>HIGHMEM=>NORMAL=>DMA32=>DMA.
 * But GFP_MOVABLE is not only a zone specifier but also an allocation
 * policy. Therefore __GFP_MOVABLE plus another zone selector is valid.
 * Only 1 bit of the lowest 3 bits (DMA,DMA32,HIGHMEM) can be set to "1".
 *
 *       bit       result
 *       =================
 *       0x0    => NORMAL
 *       0x1    => DMA or NORMAL
 *       0x2    => HIGHMEM or NORMAL
 *       0x3    => BAD (DMA+HIGHMEM)
 *       0x4    => DMA32 or DMA or NORMAL
 *       0x5    => BAD (DMA+DMA32)
 *       0x6    => BAD (HIGHMEM+DMA32)
 *       0x7    => BAD (HIGHMEM+DMA32+DMA)
 *       0x8    => NORMAL (MOVABLE+0)
 *       0x9    => DMA or NORMAL (MOVABLE+DMA)
 *       0xa    => MOVABLE (Movable is valid only if HIGHMEM is set too)
 *       0xb    => BAD (MOVABLE+HIGHMEM+DMA)
 *       0xc    => DMA32 (MOVABLE+DMA32)
 *       0xd    => BAD (MOVABLE+DMA32+DMA)
 *       0xe    => BAD (MOVABLE+DMA32+HIGHMEM)
 *       0xf    => BAD (MOVABLE+DMA32+HIGHMEM+DMA)
 *
 * ZONES_SHIFT must be <= 2 on 32 bit platforms.
 */

#if 16 * ZONES_SHIFT > BITS_PER_LONG
#error ZONES_SHIFT too large to create GFP_ZONE_TABLE integer
#endif

/* IAMROOT-12AB:
 * -------------
 * ZONES_SHIFT: zone을 표현할 때 사용하는 비트 수로 최대 2bit를 사용한다.
 * rpi2: 1(ZONE_NORMAL, ZONE_MOVABLE)
 *
 * OPT_ZONE_DMA: CONFIG_ZONE_DMA가 설정된 경우 ZONE_DMA(0)이고 
 *                                 설정되지 않은 경우 ZONE_NORMAL이다.
 *
 * GFP_ZONE_TABLE에 의해 만들어지는 상수들은 다음과 같이 16개의 항목으로 나뉘고 
 * 각 항목은 사용하는 zone bit 수 만큼 결정된다. (0~2) 
 *
 * 이 테이블은 4bit로 구성된 gfp 플래그 -> zone 인덱스(0~3)로 변환한다.
 * ----------------------------------------------------------------------------
 * entry #00: GOOD - ZONE_NORMAL 
 * entry #01: GOOD - ZONE_DMA(없으면 ZONE_NORMAL)
 * entry #02: GOOD - ZONE_HIGHMEM(없으면 ZONE_NORMAL)
 * entry #03: BAD  - 
 * entry #04: GOOD - ZONE_DMA32(없으면 ZONE_NORMAL) 
 * entry #05: BAD  -
 * entry #06: BAD  -
 * entry #07: BAD  -
 * entry #08: GOOD - ZONE_NORMAL 
 * entry #09: GOOD - ZONE_DMA(없으면 ZONE_NORMAL) 
 * entry #10: GOOD - ZONE_MOVABLE 
 * entry #11: BAD  -
 * entry #12: GOOD - ZONE_DMA32(없으면 ZONE_NORMAL) 
 * entry #13: BAD  -
 * entry #14: BAD  -
 * entry #15: BAD  -
 */
#define GFP_ZONE_TABLE ( \
	(ZONE_NORMAL << 0 * ZONES_SHIFT)				      \
	| (OPT_ZONE_DMA << ___GFP_DMA * ZONES_SHIFT)			      \
	| (OPT_ZONE_HIGHMEM << ___GFP_HIGHMEM * ZONES_SHIFT)		      \
	| (OPT_ZONE_DMA32 << ___GFP_DMA32 * ZONES_SHIFT)		      \
	| (ZONE_NORMAL << ___GFP_MOVABLE * ZONES_SHIFT)			      \
	| (OPT_ZONE_DMA << (___GFP_MOVABLE | ___GFP_DMA) * ZONES_SHIFT)	      \
	| (ZONE_MOVABLE << (___GFP_MOVABLE | ___GFP_HIGHMEM) * ZONES_SHIFT)   \
	| (OPT_ZONE_DMA32 << (___GFP_MOVABLE | ___GFP_DMA32) * ZONES_SHIFT)   \
)

/*
 * GFP_ZONE_BAD is a bitmap for all combinations of __GFP_DMA, __GFP_DMA32
 * __GFP_HIGHMEM and __GFP_MOVABLE that are not permitted. One flag per
 * entry starting with bit 0. Bit is set if the combination is not
 * allowed.
 */
#define GFP_ZONE_BAD ( \
	1 << (___GFP_DMA | ___GFP_HIGHMEM)				      \
	| 1 << (___GFP_DMA | ___GFP_DMA32)				      \
	| 1 << (___GFP_DMA32 | ___GFP_HIGHMEM)				      \
	| 1 << (___GFP_DMA | ___GFP_DMA32 | ___GFP_HIGHMEM)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_HIGHMEM | ___GFP_DMA)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_DMA)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_HIGHMEM)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_DMA | ___GFP_HIGHMEM)  \
)

/* IAMROOT-12AB:
 * -------------
 * GFP_ZONE_TABLE로 부터 요청 gfp 플래그에 해당하는 zone 인덱스 번호를 반환한다. 
 * (시스템에 따라 0 ~ 3까지)
 * (rpi2: 0(ZONE_NORMAL) ~ 1(ZONE_MOVABLE))
 *
 * flags에 zone에 대해 아무것도 지정하지 않은 경우 ZONE_NORMAL을 반환한다.
 */
static inline enum zone_type gfp_zone(gfp_t flags)
{
	enum zone_type z;
	int bit = (__force int) (flags & GFP_ZONEMASK);

	z = (GFP_ZONE_TABLE >> (bit * ZONES_SHIFT)) &
					 ((1 << ZONES_SHIFT) - 1);
	VM_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	return z;
}

/*
 * There is only one page-allocator function, and two main namespaces to
 * it. The alloc_page*() variants return 'struct page *' and as such
 * can allocate highmem pages, the *get*page*() variants return
 * virtual kernel addresses to the allocated page(s).
 */

static inline int gfp_zonelist(gfp_t flags)
{
	if (IS_ENABLED(CONFIG_NUMA) && unlikely(flags & __GFP_THISNODE))
		return 1;

	return 0;
}

/*
 * We get the zone list from the current node and the gfp_mask.
 * This zone list contains a maximum of MAXNODES*MAX_NR_ZONES zones.
 * There are two zonelists per node, one for all zones with memory and
 * one containing just zones from the node the zonelist belongs to.
 *
 * For the normal case of non-DISCONTIGMEM systems the NODE_DATA() gets
 * optimized to &contig_page_data at compile-time.
 */
static inline struct zonelist *node_zonelist(int nid, gfp_t flags)
{
/* IAMROOT-12:
 * -------------
 * 각 노드에 미리 만들어놓은 zone fallback list
 */
	return NODE_DATA(nid)->node_zonelists + gfp_zonelist(flags);
}

#ifndef HAVE_ARCH_FREE_PAGE
static inline void arch_free_page(struct page *page, int order) { }
#endif
#ifndef HAVE_ARCH_ALLOC_PAGE
static inline void arch_alloc_page(struct page *page, int order) { }
#endif

struct page *
__alloc_pages_nodemask(gfp_t gfp_mask, unsigned int order,
		       struct zonelist *zonelist, nodemask_t *nodemask);

static inline struct page *
__alloc_pages(gfp_t gfp_mask, unsigned int order,
		struct zonelist *zonelist)
{
	return __alloc_pages_nodemask(gfp_mask, order, zonelist, NULL);
}

static inline struct page *alloc_pages_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	/* Unknown node is current node */
	if (nid < 0)
		nid = numa_node_id();

	return __alloc_pages(gfp_mask, order, node_zonelist(nid, gfp_mask));
}

static inline struct page *alloc_pages_exact_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	VM_BUG_ON(nid < 0 || nid >= MAX_NUMNODES || !node_online(nid));

	return __alloc_pages(gfp_mask, order, node_zonelist(nid, gfp_mask));
}

#ifdef CONFIG_NUMA
extern struct page *alloc_pages_current(gfp_t gfp_mask, unsigned order);

static inline struct page *

/* IAMROOT-12:
 * -------------
 * order 페이지를 할당한다. 
 *      - NUMA: alloc_pages_current() -> 노드 선택 알고리즘 사용
 *      - UMA:  alloc_pages_node() -> 자기 노드의 버디 시스템에서 할당
 */
alloc_pages(gfp_t gfp_mask, unsigned int order)
{
	return alloc_pages_current(gfp_mask, order);
}
extern struct page *alloc_pages_vma(gfp_t gfp_mask, int order,
			struct vm_area_struct *vma, unsigned long addr,
			int node, bool hugepage);
#define alloc_hugepage_vma(gfp_mask, vma, addr, order)	\
	alloc_pages_vma(gfp_mask, order, vma, addr, numa_node_id(), true)
#else
#define alloc_pages(gfp_mask, order) \
		alloc_pages_node(numa_node_id(), gfp_mask, order)
#define alloc_pages_vma(gfp_mask, order, vma, addr, node, false)\
	alloc_pages(gfp_mask, order)
#define alloc_hugepage_vma(gfp_mask, vma, addr, order)	\
	alloc_pages(gfp_mask, order)
#endif

/* IAMROOT-12:
 * -------------
 * alloc_page(): 1 페이지 할당
 * alloc_pages(): order 만큼 할당
 */
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)
#define alloc_page_vma(gfp_mask, vma, addr)			\
	alloc_pages_vma(gfp_mask, 0, vma, addr, numa_node_id(), false)
#define alloc_page_vma_node(gfp_mask, vma, addr, node)		\
	alloc_pages_vma(gfp_mask, 0, vma, addr, node, false)

extern struct page *alloc_kmem_pages(gfp_t gfp_mask, unsigned int order);
extern struct page *alloc_kmem_pages_node(int nid, gfp_t gfp_mask,
					  unsigned int order);

extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
extern unsigned long get_zeroed_page(gfp_t gfp_mask);

void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);
/* This is different from alloc_pages_exact_node !!! */
void * __meminit alloc_pages_exact_nid(int nid, size_t size, gfp_t gfp_mask);

#define __get_free_page(gfp_mask) \
		__get_free_pages((gfp_mask), 0)

#define __get_dma_pages(gfp_mask, order) \
		__get_free_pages((gfp_mask) | GFP_DMA, (order))

extern void __free_pages(struct page *page, unsigned int order);
extern void free_pages(unsigned long addr, unsigned int order);
extern void free_hot_cold_page(struct page *page, bool cold);
extern void free_hot_cold_page_list(struct list_head *list, bool cold);

extern void __free_kmem_pages(struct page *page, unsigned int order);
extern void free_kmem_pages(unsigned long addr, unsigned int order);


/* IAMROOT-12:
 * -------------
 * order를 0으로 지정하여 한 페이지만 버디시스템으로 돌려준다.
 */
#define __free_page(page) __free_pages((page), 0)
#define free_page(addr) free_pages((addr), 0)

void page_alloc_init(void);
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp);
void drain_all_pages(struct zone *zone);
void drain_local_pages(struct zone *zone);

/*
 * gfp_allowed_mask is set to GFP_BOOT_MASK during early boot to restrict what
 * GFP flags are used before interrupts are enabled. Once interrupts are
 * enabled, it is set to __GFP_BITS_MASK while the system is running. During
 * hibernation, it is used by PM to avoid I/O during memory allocation while
 * devices are suspended.
 */
extern gfp_t gfp_allowed_mask;

/* Returns true if the gfp_mask allows use of ALLOC_NO_WATERMARK */
bool gfp_pfmemalloc_allowed(gfp_t gfp_mask);

extern void pm_restrict_gfp_mask(void);
extern void pm_restore_gfp_mask(void);

#ifdef CONFIG_PM_SLEEP
extern bool pm_suspended_storage(void);
#else
static inline bool pm_suspended_storage(void)
{
	return false;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_CMA

/* The below functions must be run on a range from a single zone. */
extern int alloc_contig_range(unsigned long start, unsigned long end,
			      unsigned migratetype);
extern void free_contig_range(unsigned long pfn, unsigned nr_pages);

/* CMA stuff */
extern void init_cma_reserved_pageblock(struct page *page);

#endif

#endif /* __LINUX_GFP_H */
