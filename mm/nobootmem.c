/*
 *  bootmem - A boot-time physical memory allocator and configurator
 *
 *  Copyright (C) 1999 Ingo Molnar
 *                1999 Kanoj Sarcar, SGI
 *                2008 Johannes Weiner
 *
 * Access to this subsystem has to be serialized externally (which is true
 * for the boot process anyway).
 */
#include <linux/init.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/bootmem.h>
#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/range.h>
#include <linux/memblock.h>

#include <asm/bug.h>
#include <asm/io.h>
#include <asm/processor.h>

#include "internal.h"

#ifndef CONFIG_NEED_MULTIPLE_NODES
struct pglist_data __refdata contig_page_data;
EXPORT_SYMBOL(contig_page_data);
#endif

/* IAMROOT-12AB:
 * -------------
 * min_low_pfn: DRAM 가장 하위 pfn 
 * max_low_pfn: highmem 시작 pfn 
 * max_pfn: DRAM 끝 pfn+1
 */
unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;

static void * __init __alloc_memory_core_early(int nid, u64 size, u64 align,
					u64 goal, u64 limit)
{
	void *ptr;
	u64 addr;

	if (limit > memblock.current_limit)
		limit = memblock.current_limit;

	addr = memblock_find_in_range_node(size, align, goal, limit, nid);
	if (!addr)
		return NULL;

	if (memblock_reserve(addr, size))
		return NULL;

	ptr = phys_to_virt(addr);
	memset(ptr, 0, size);
	/*
	 * The min_count is set to 0 so that bootmem allocated blocks
	 * are never reported as leaks.
	 */
	kmemleak_alloc(ptr, size, 0, 0);
	return ptr;
}

/*
 * free_bootmem_late - free bootmem pages directly to page allocator
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * This is only useful when the bootmem allocator has already been torn
 * down, but we are still initializing the system.  Pages are given directly
 * to the page allocator, no bootmem metadata is updated because it is gone.
 */
void __init free_bootmem_late(unsigned long addr, unsigned long size)
{
	unsigned long cursor, end;

	kmemleak_free_part(__va(addr), size);

	cursor = PFN_UP(addr);
	end = PFN_DOWN(addr + size);

	for (; cursor < end; cursor++) {
		__free_pages_bootmem(pfn_to_page(cursor), 0);
		totalram_pages++;
	}
}

static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int order;

	while (start < end) {

/* IAMROOT-12AB:
 * -------------
 * 버디 시스템은 MAX_ORDER-1 단위의 order를 등록하지 않기 때문에 min() 함수로 
 * 잘라낸다. (arm32: MAX_ORDER=11, 버디시스템에 최대 4M 단위 페이지가 등록된다)
 *
 * 메모리 영역의 시작 부분 관련
 *	- 시작 pfn으로 order를 먼저 결정을 한다. 
 *	- 단 MAX_ORDER-1을 넘지 않도록 제한한다.
 */
		order = min(MAX_ORDER - 1UL, __ffs(start));

/* IAMROOT-12AB:
 * -------------
 * 메모리 영역의 끝 부분 관련
 *	- order를 더해 end를 초과하는 경우 order를 줄여나간다. 
 */
		while (start + (1UL << order) > end)
			order--;

/* IAMROOT-12AB:
 * -------------
 * 해재할 메모리를 order 단위로 버디시스템에 돌려준다(free)
 *
 * 예) start=0x12345, end=0x13456
 *              start		order 
 *              -------		-----
 *     loop 01:	0x12345		0
 *     loop 02: 0x12346         1
 *     loop 03: 0x12348		3
 *     loop 04: 0x12350		4
 *     loop 05: 0x12360		5
 *     loop 06: 0x12380		7
 *     loop 07: 0x12400		10
 *     loop 08: 0x12800		10 
 *     loop 09: 0x12c00		10 
 *     loop 10: 0x13000		10
 *     loop 11: 0x13400		6
 *     loop 12: 0x13440		4
 *     loop 13: 0x13450		2
 *     loop 14: 0x13454		1
 */
		__free_pages_bootmem(pfn_to_page(start), order);

		start += (1UL << order);
	}
}

static unsigned long __init __free_memory_core(phys_addr_t start,
				 phys_addr_t end)
{
	unsigned long start_pfn = PFN_UP(start);

/* IAMROOT-12AB:
 * -------------
 * 지정된 end 영역이 lowmem을 초과하지 않도록 한다.
 */
	unsigned long end_pfn = min_t(unsigned long,
				      PFN_DOWN(end), max_low_pfn);

	if (start_pfn > end_pfn)
		return 0;

	__free_pages_memory(start_pfn, end_pfn);

	return end_pfn - start_pfn;
}

static unsigned long __init free_low_memory_core_early(void)
{
	unsigned long count = 0;
	phys_addr_t start, end;
	u64 i;

/* IAMROOT-12AB:
 * -------------
 * memblock 전체에서 MEMBLOCK_HOTPLUG 플래그를 제거한다.
 */
	memblock_clear_hotplug(0, -1);

	for_each_free_mem_range(i, NUMA_NO_NODE, &start, &end, NULL)
		count += __free_memory_core(start, end);

/* IAMROOT-12AB:
 * -------------
 * memblock을 관리하는 구조체들이 버디시스템으로 전환 후에 필요 없어지므로 
 * 사용했던 구조체들을 삭제한다. 
 * (단 HOTPLUG 옵션이 사용되는 경우 memblock은 계속 필요하다)
 *
 */
#ifdef CONFIG_ARCH_DISCARD_MEMBLOCK
	{
		phys_addr_t size;

		/* Free memblock.reserved array if it was allocated */
		size = get_allocated_memblock_reserved_regions_info(&start);
		if (size)
			count += __free_memory_core(start, start + size);

		/* Free memblock.memory array if it was allocated */
		size = get_allocated_memblock_memory_regions_info(&start);
		if (size)
			count += __free_memory_core(start, start + size);
	}
#endif

	return count;
}

static int reset_managed_pages_done __initdata;

/* IAMROOT-12AB:
 * -------------
 * 지정된 노드에 있는 zone의 managed_pages를 clear 한다.
 */
void reset_node_managed_pages(pg_data_t *pgdat)
{
	struct zone *z;

	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++)
		z->managed_pages = 0;
}

/* IAMROOT-12AB:
 * -------------
 * 전체 노드의 zone->managed_pages를 clear 한다.
 */
void __init reset_all_zones_managed_pages(void)
{
	struct pglist_data *pgdat;

/* IAMROOT-12AB:
 * -------------
 * 두 번 이상 호출되는 것을 막는다.
 */
	if (reset_managed_pages_done)
		return;

	for_each_online_pgdat(pgdat)
		reset_node_managed_pages(pgdat);

	reset_managed_pages_done = 1;
}

/**
 * free_all_bootmem - release free pages to the buddy allocator
 *
 * Returns the number of pages actually released.
 */
unsigned long __init free_all_bootmem(void)
{
	unsigned long pages;

/* IAMROOT-12AB:
 * -------------
 * 각 zone->managed_pages을 clear한다.
 */
	reset_all_zones_managed_pages();

	/*
	 * We need to use NUMA_NO_NODE instead of NODE_DATA(0)->node_id
	 *  because in some case like Node0 doesn't have RAM installed
	 *  low ram will be on Node1
	 */
	pages = free_low_memory_core_early();
	totalram_pages += pages;

	return pages;
}

/**
 * free_bootmem_node - mark a page range as usable
 * @pgdat: node the range resides on
 * @physaddr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must reside completely on the specified node.
 */
void __init free_bootmem_node(pg_data_t *pgdat, unsigned long physaddr,
			      unsigned long size)
{
	memblock_free(physaddr, size);
}

/**
 * free_bootmem - mark a page range as usable
 * @addr: starting address of the range
 * @size: size of the range in bytes
 *
 * Partial pages will be considered reserved and left as they are.
 *
 * The range must be contiguous but may span node boundaries.
 */
void __init free_bootmem(unsigned long addr, unsigned long size)
{
	memblock_free(addr, size);
}

static void * __init ___alloc_bootmem_nopanic(unsigned long size,
					unsigned long align,
					unsigned long goal,
					unsigned long limit)
{
	void *ptr;

	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc(size, GFP_NOWAIT);

restart:

	ptr = __alloc_memory_core_early(NUMA_NO_NODE, size, align, goal, limit);

	if (ptr)
		return ptr;

	if (goal != 0) {
		goal = 0;
		goto restart;
	}

	return NULL;
}

/**
 * __alloc_bootmem_nopanic - allocate boot memory without panicking
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * Returns NULL on failure.
 */
void * __init __alloc_bootmem_nopanic(unsigned long size, unsigned long align,
					unsigned long goal)
{
	unsigned long limit = -1UL;

	return ___alloc_bootmem_nopanic(size, align, goal, limit);
}

static void * __init ___alloc_bootmem(unsigned long size, unsigned long align,
					unsigned long goal, unsigned long limit)
{
	void *mem = ___alloc_bootmem_nopanic(size, align, goal, limit);

	if (mem)
		return mem;
	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem - allocate boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem(unsigned long size, unsigned long align,
			      unsigned long goal)
{
	unsigned long limit = -1UL;

	return ___alloc_bootmem(size, align, goal, limit);
}

void * __init ___alloc_bootmem_node_nopanic(pg_data_t *pgdat,
						   unsigned long size,
						   unsigned long align,
						   unsigned long goal,
						   unsigned long limit)
{
	void *ptr;

again:
	ptr = __alloc_memory_core_early(pgdat->node_id, size, align,
					goal, limit);
	if (ptr)
		return ptr;

	ptr = __alloc_memory_core_early(NUMA_NO_NODE, size, align,
					goal, limit);
	if (ptr)
		return ptr;

	if (goal) {
		goal = 0;
		goto again;
	}

	return NULL;
}

void * __init __alloc_bootmem_node_nopanic(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, 0);
}

static void * __init ___alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				    unsigned long align, unsigned long goal,
				    unsigned long limit)
{
	void *ptr;

	ptr = ___alloc_bootmem_node_nopanic(pgdat, size, align, goal, limit);
	if (ptr)
		return ptr;

	printk(KERN_ALERT "bootmem alloc of %lu bytes failed!\n", size);
	panic("Out of memory");
	return NULL;
}

/**
 * __alloc_bootmem_node - allocate boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_node(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat, size, align, goal, 0);
}

void * __init __alloc_bootmem_node_high(pg_data_t *pgdat, unsigned long size,
				   unsigned long align, unsigned long goal)
{
	return __alloc_bootmem_node(pgdat, size, align, goal);
}

#ifndef ARCH_LOW_ADDRESS_LIMIT
#define ARCH_LOW_ADDRESS_LIMIT	0xffffffffUL
#endif

/**
 * __alloc_bootmem_low - allocate low boot memory
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may happen on any node in the system.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low(unsigned long size, unsigned long align,
				  unsigned long goal)
{
	return ___alloc_bootmem(size, align, goal, ARCH_LOW_ADDRESS_LIMIT);
}

void * __init __alloc_bootmem_low_nopanic(unsigned long size,
					  unsigned long align,
					  unsigned long goal)
{
	return ___alloc_bootmem_nopanic(size, align, goal,
					ARCH_LOW_ADDRESS_LIMIT);
}

/**
 * __alloc_bootmem_low_node - allocate low boot memory from a specific node
 * @pgdat: node to allocate from
 * @size: size of the request in bytes
 * @align: alignment of the region
 * @goal: preferred starting address of the region
 *
 * The goal is dropped if it can not be satisfied and the allocation will
 * fall back to memory below @goal.
 *
 * Allocation may fall back to any node in the system if the specified node
 * can not hold the requested memory.
 *
 * The function panics if the request can not be satisfied.
 */
void * __init __alloc_bootmem_low_node(pg_data_t *pgdat, unsigned long size,
				       unsigned long align, unsigned long goal)
{
	if (WARN_ON_ONCE(slab_is_available()))
		return kzalloc_node(size, GFP_NOWAIT, pgdat->node_id);

	return ___alloc_bootmem_node(pgdat, size, align, goal,
				     ARCH_LOW_ADDRESS_LIMIT);
}
