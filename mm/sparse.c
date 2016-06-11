/*
 * sparse memory mappings.
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/bootmem.h>
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "internal.h"
#include <asm/dma.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

/*
 * Permanent SPARSEMEM data:
 *
 * 1) mem_section	- memory sections, mem_map's for valid memory
 */

/* IAMROOT-12AB:
 * -------------
 * mem_section이 null인 경우 hole 영역을 의미한다.
 *	EXTREME: 1차 *mem_section[] 포인터 배열이 static하게 생성되고,
 *		     2차 mem_section[] 배열의 처음을 가리킨다.
 *		 2차 mem_section[] 배열은 dynamic 하게 생성된다.
 *		 (주로 64bit 시스템에서 사용)
 *	STATIC:  1차 mem_section[] 배열은 static 하게 생성된다.
 *		 (주로 32bit 시스템에서 사용)
 */
#ifdef CONFIG_SPARSEMEM_EXTREME
struct mem_section *mem_section[NR_SECTION_ROOTS]
	____cacheline_internodealigned_in_smp;
#else
struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT]
	____cacheline_internodealigned_in_smp;
#endif
EXPORT_SYMBOL(mem_section);

#ifdef NODE_NOT_IN_PAGE_FLAGS
/*
 * If we did not store the node number in the page then we have to
 * do a lookup in the section_to_node_table in order to find which
 * node the page belongs to.
 */

/* IAMROOT-12AB:
 * -------------
 * 섹션별로 노드 id가 기록되어 있는 테이블
 */
#if MAX_NUMNODES <= 256
static u8 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#else
static u16 section_to_node_table[NR_MEM_SECTIONS] __cacheline_aligned;
#endif

int page_to_nid(const struct page *page)
{
	return section_to_node_table[page_to_section(page)];
}
EXPORT_SYMBOL(page_to_nid);

static void set_section_nid(unsigned long section_nr, int nid)
{
/* IAMROOT-12AB:
 * -------------
 * 섹션 별로 노드 id를 기록한다.
 */
	section_to_node_table[section_nr] = nid;
}
#else /* !NODE_NOT_IN_PAGE_FLAGS */
static inline void set_section_nid(unsigned long section_nr, int nid)
{
}
#endif

#ifdef CONFIG_SPARSEMEM_EXTREME

/* IAMROOT-12AB:
 * -------------
 * __init_refok:
 *	.ref.text 섹션에 저장한다.
 *	이렇게 한 이유는 컴파일러(modpost 스크립트)로 하여금 .init 섹션에서
 *	없어질 코드를 레퍼런스해도 경고 메시지 출력을 하지 않게끔 한다.
 *	(아래 코드에서 memblock_virt_alloc_node() 함수를 사용했는데
 *	이 함수는 나중에 커널이 초기화된 후 DISCARD 옵션에 따라 free되어
 *	없어질 수도 있다)
 */
static struct mem_section noinline __init_refok *sparse_index_alloc(int nid)
{
	struct mem_section *section = NULL;

/* IAMROOT-12AB:
 * -------------
 * array_size는 페이지 사이즈에 가까운 크기
 */
	unsigned long array_size = SECTIONS_PER_ROOT *
				   sizeof(struct mem_section);

	if (slab_is_available()) {
		if (node_state(nid, N_HIGH_MEMORY))
			section = kzalloc_node(array_size, GFP_KERNEL, nid);
		else
			section = kzalloc(array_size, GFP_KERNEL);
	} else {

/* IAMROOT-12AB:
 * -------------
 * 가능하면 지정된 노드 및 범위에서 메모리를 할당받아온다.
 */
		section = memblock_virt_alloc_node(array_size, nid);
	}

	return section;
}

static int __meminit sparse_index_init(unsigned long section_nr, int nid)
{
	unsigned long root = SECTION_NR_TO_ROOT(section_nr);
	struct mem_section *section;

/* IAMROOT-12AB:
 * -------------
 * EXTREME: 1차 mem_section[] 포인터 배열이 이미 초기화된 경우는 종료
 * STATIC: 1차 mem_section[] 배열의 값이 이미 초기화된 경우는 종료
 */
	if (mem_section[root])
		return -EEXIST;

/* IAMROOT-12AB:
 * -------------
 * EXTREME: 2차 mem_section[] 배열을 할당 받는다.
 */
	section = sparse_index_alloc(nid);
	if (!section)
		return -ENOMEM;

	mem_section[root] = section;

	return 0;
}
#else /* !SPARSEMEM_EXTREME */
static inline int sparse_index_init(unsigned long section_nr, int nid)
{
	return 0;
}
#endif

/*
 * Although written for the SPARSEMEM_EXTREME case, this happens
 * to also work for the flat array case because
 * NR_SECTION_ROOTS==NR_MEM_SECTIONS.
 */
int __section_nr(struct mem_section* ms)
{
	unsigned long root_nr;
	struct mem_section* root;

	for (root_nr = 0; root_nr < NR_SECTION_ROOTS; root_nr++) {
		root = __nr_to_section(root_nr * SECTIONS_PER_ROOT);
		if (!root)
			continue;

		if ((ms >= root) && (ms < (root + SECTIONS_PER_ROOT)))
		     break;
	}

	VM_BUG_ON(root_nr == NR_SECTION_ROOTS);

	return (root_nr * SECTIONS_PER_ROOT) + (ms - root);
}

/*
 * During early boot, before section_mem_map is used for an actual
 * mem_map, we use section_mem_map to store the section's NUMA
 * node.  This keeps us from having to use another data structure.  The
 * node information is cleared just before we store the real mem_map.
 */

/* IAMROOT-12AB:
 * -------------
 * section_mem_map의 bit2부터 노드 id를 기록한다.
 * 실제 이 멤버 변수가 섹션이 사용하는 mem_map을 가리키기 전까지
 * 커널이 early boot 과정에 있는 동안 잠시 사용되고 지워진다.
 */
static inline unsigned long sparse_encode_early_nid(int nid)
{
	return (nid << SECTION_NID_SHIFT);
}

static inline int sparse_early_nid(struct mem_section *section)
{
	return (section->section_mem_map >> SECTION_NID_SHIFT);
}

/* Validate the physical addressing limitations of the model */
void __meminit mminit_validate_memmodel_limits(unsigned long *start_pfn,
						unsigned long *end_pfn)
{

/* IAMROOT-12AB:
 * -------------
 * Sparse에서 최대 관리 가능한 메모리 페이지 번호
 */
	unsigned long max_sparsemem_pfn = 1UL << (MAX_PHYSMEM_BITS-PAGE_SHIFT);

	/*
	 * Sanity checks - do not allow an architecture to pass
	 * in larger pfns than the maximum scope of sparsemem:
	 */

/* IAMROOT-12AB:
 * -------------
 * 시작 주소가 최대 관리 가능한 pfn을 초과하는 경우 포기
 */
	if (*start_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"Start of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*start_pfn = max_sparsemem_pfn;
		*end_pfn = max_sparsemem_pfn;
/* IAMROOT-12AB:
 * -------------
 * 끝 주소가 최대 관리 가능한 pfn을 초과하는 경우 max_sparsemem_pfn 값으로 자른다.
 */
	} else if (*end_pfn > max_sparsemem_pfn) {
		mminit_dprintk(MMINIT_WARNING, "pfnvalidation",
			"End of range %lu -> %lu exceeds SPARSEMEM max %lu\n",
			*start_pfn, *end_pfn, max_sparsemem_pfn);
		WARN_ON_ONCE(1);
		*end_pfn = max_sparsemem_pfn;
	}
}

/* Record a memory area against a node. */

/* IAMROOT-12AB:
 * -------------
 * Sparse 메모리 모델에서 사용되는 메모리 영역에 대해 섹션 단위 관리를 준비한다.
 */
void __init memory_present(int nid, unsigned long start, unsigned long end)
{
	unsigned long pfn;

/* IAMROOT-12AB:
 * -------------
 * 시작 주소에 대해 섹션 단위로 round down
 */
	start &= PAGE_SECTION_MASK;

/* IAMROOT-12AB:
 * -------------
 * start ~ end 까지가 최대 섹션 관리 영역에 들어가는 것만 아래 루프 수행
 */
	mminit_validate_memmodel_limits(&start, &end);

/* IAMROOT-12AB:
 * -------------
 * 섹션 단위로 증가하며 관련 구조 생성
 */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		unsigned long section = pfn_to_section_nr(pfn);
		struct mem_section *ms;

/* IAMROOT-12AB:
 * -------------
 * SPARSE_EXTREAM: 메모리가 있는 섹션인 경우
 *		   루트별 mem_section 영역을 할당 받고 *mem_section[]과 연결한다.
 *		   (실제 할당된 영역은 mem_section[][]과 같이 2차원 배열로 연결된다.)
 */
		sparse_index_init(section, nid);

/* IAMROOT-12AB:
 * -------------
 * 섹션별로 노드 id를 기록한다.
 */
		set_section_nid(section, nid);

		ms = __nr_to_section(section);

/* IAMROOT-12AB:
 * -------------
 * section_mem_map이 null인 경우 노드 id와 섹션에 메모리 존재 플래그를 설정한다.
 * 노드 id는 나중에 섹션에 mem_map이 연결되어 사용되어질 때 삭제된다.
 */
		if (!ms->section_mem_map)
			ms->section_mem_map = sparse_encode_early_nid(nid) |
							SECTION_MARKED_PRESENT;
	}
}

/*
 * Only used by the i386 NUMA architecures, but relatively
 * generic code.
 */
unsigned long __init node_memmap_size_bytes(int nid, unsigned long start_pfn,
						     unsigned long end_pfn)
{
	unsigned long pfn;
	unsigned long nr_pages = 0;

	mminit_validate_memmodel_limits(&start_pfn, &end_pfn);
	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		if (nid != early_pfn_to_nid(pfn))
			continue;

		if (pfn_present(pfn))
			nr_pages += PAGES_PER_SECTION;
	}

	return nr_pages * sizeof(struct page);
}

/*
 * Subtle, we encode the real pfn into the mem_map such that
 * the identity pfn - section_mem_map will return the actual
 * physical page frame number.
 */
static unsigned long sparse_encode_mem_map(struct page *mem_map, unsigned long pnum)
{
	return (unsigned long)(mem_map - (section_nr_to_pfn(pnum)));
}

/*
 * Decode mem_map from the coded memmap
 */
struct page *sparse_decode_mem_map(unsigned long coded_mem_map, unsigned long pnum)
{
	/* mask off the extra low bits of information */
	coded_mem_map &= SECTION_MAP_MASK;
	return ((struct page *)coded_mem_map) + section_nr_to_pfn(pnum);
}

static int __meminit sparse_init_one_section(struct mem_section *ms,
		unsigned long pnum, struct page *mem_map,
		unsigned long *pageblock_bitmap)
{
/* IAMROOT-12AB:
 * -------------
 * 메모리가 없는 섹션인 경우 에러 리턴
 */
	if (!present_section(ms))
		return -EINVAL;

/* IAMROOT-12AB:
 * -------------
 * 2개 비트만 남기고(기존에 노드 정보를 bit3 이상에 기록하였었던) 
 *  section_mem_map을 클리어한 다음에 xxxxxx + valid 표식(bit1)
 */
	ms->section_mem_map &= ~SECTION_MAP_MASK;
	ms->section_mem_map |= sparse_encode_mem_map(mem_map, pnum) |
							SECTION_HAS_MEM_MAP;

/* IAMROOT-12AB:
 * -------------
 * usemap을 가리키게 한다.
 */
 	ms->pageblock_flags = pageblock_bitmap;

	return 1;
}

/* IAMROOT-12AB:
 * -------------
 * usemap의 크기는 섹션 및 pageblock 크기에 따라 영향을 받고 바이트 수로 반환한다.
 */
unsigned long usemap_size(void)
{
	unsigned long size_bytes;
	size_bytes = roundup(SECTION_BLOCKFLAGS_BITS, 8) / 8;
	size_bytes = roundup(size_bytes, sizeof(unsigned long));
	return size_bytes;
}

#ifdef CONFIG_MEMORY_HOTPLUG
static unsigned long *__kmalloc_section_usemap(void)
{
	return kmalloc(usemap_size(), GFP_KERNEL);
}
#endif /* CONFIG_MEMORY_HOTPLUG */

#ifdef CONFIG_MEMORY_HOTREMOVE
static unsigned long * __init

/* IAMROOT-12AB:
 * -------------
 * 이 함수 대신 아래에 있는 함수로 분석한다.
 */
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
{
	unsigned long goal, limit;
	unsigned long *p;
	int nid;
	/*
	 * A page may contain usemaps for other sections preventing the
	 * page being freed and making a section unremovable while
	 * other sections referencing the usemap remain active. Similarly,
	 * a pgdat can prevent a section being removed. If section A
	 * contains a pgdat and section B contains the usemap, both
	 * sections become inter-dependent. This allocates usemaps
	 * from the same section as the pgdat where possible to avoid
	 * this problem.
	 */
	goal = __pa(pgdat) & (PAGE_SECTION_MASK << PAGE_SHIFT);
	limit = goal + (1UL << PA_SECTION_SHIFT);
	nid = early_pfn_to_nid(goal >> PAGE_SHIFT);
again:
	p = memblock_virt_alloc_try_nid_nopanic(size,
						SMP_CACHE_BYTES, goal, limit,
						nid);
	if (!p && limit) {
		limit = 0;
		goto again;
	}
	return p;
}

static void __init check_usemap_section_nr(int nid, unsigned long *usemap)
{
	unsigned long usemap_snr, pgdat_snr;
	static unsigned long old_usemap_snr = NR_MEM_SECTIONS;
	static unsigned long old_pgdat_snr = NR_MEM_SECTIONS;
	struct pglist_data *pgdat = NODE_DATA(nid);
	int usemap_nid;

	usemap_snr = pfn_to_section_nr(__pa(usemap) >> PAGE_SHIFT);
	pgdat_snr = pfn_to_section_nr(__pa(pgdat) >> PAGE_SHIFT);

/* IAMROOT-12AB:
 * -------------
 * usemap과 pgdat가 같은 섹션에 있는 경우 ok
 */
	if (usemap_snr == pgdat_snr)
		return;

/* IAMROOT-12AB:
 * -------------
 * usemap에 대한 섹션번호가 변경되지 않았으면 한 번 출력된 에러메시지를 
 * 반복해서 출력하지 않기 위해 사용
 */
	if (old_usemap_snr == usemap_snr && old_pgdat_snr == pgdat_snr)
		/* skip redundant message */
		return;

/* IAMROOT-12AB:
 * -------------
 * 바뀐 섹션 번호를 기억한다. (같은 섹션에 대한 메시지 반복 출력을 막기 위해)
 */
	old_usemap_snr = usemap_snr;
	old_pgdat_snr = pgdat_snr;

	usemap_nid = sparse_early_nid(__nr_to_section(usemap_snr));

/* IAMROOT-12AB:
 * -------------
 * usemap이 nid와 다른 노드에 할당 받은 경우 경고 메시지를 출력한다.
 */
	if (usemap_nid != nid) {
		printk(KERN_INFO
		       "node %d must be removed before remove section %ld\n",
		       nid, usemap_snr);
		return;
	}
	/*
	 * There is a circular dependency.
	 * Some platforms allow un-removable section because they will just
	 * gather other removable sections for dynamic partitioning.
	 * Just notify un-removable section's number here.
	 */
	printk(KERN_INFO "Section %ld and %ld (node %d)", usemap_snr,
	       pgdat_snr, nid);
	printk(KERN_CONT
	       " have a circular dependency on usemap and pgdat allocations\n");
}
#else
static unsigned long * __init
sparse_early_usemaps_alloc_pgdat_section(struct pglist_data *pgdat,
					 unsigned long size)
{
/* IAMROOT-12AB:
 * -------------
 * CONFIG_MEMORY_HOTREMOVE가 없는 경우
 */
	return memblock_virt_alloc_node_nopanic(size, pgdat->node_id);
}

static void __init check_usemap_section_nr(int nid, unsigned long *usemap)
{
}
#endif /* CONFIG_MEMORY_HOTREMOVE */


/* IAMROOT-12AB:
 * -------------
 * pnum_begin: 지정된 노드에 처음 발견된 메모리가 있는 섹션 번호
 * usemap_count: 지정된 노드에 존재하는 메모리가 있는 섹션 수
 */
static void __init sparse_early_usemaps_alloc_node(void *data,
				 unsigned long pnum_begin,
				 unsigned long pnum_end,
				 unsigned long usemap_count, int nodeid)
{
	void *usemap;
	unsigned long pnum;
	unsigned long **usemap_map = (unsigned long **)data;

/* IAMROOT-12AB:
 * -------------
 * size: usemap 사이즈(바이트 단위)
 *	한 섹션에 들어갈 pageblock 수 x pageblock flags 비트 수(4) / 8 bits
 *	(align 생략)
 */
	int size = usemap_size();

/* IAMROOT-12AB:
 * -------------
 * 지정된 노드에 대한 usemap을 할당해야하는데 가능하면 NODE_DATA(nodeid)가 
 * 생성되어 있는 노드에서 할당받는다. (왜냐하면 usemap이 같은 노드에 있지
 * 않은 경우 메모리 탈착이 가능한 hotplug memory를 지원하는 시스템에서
 * 서로 circular dependency 문제가 발생하여 노드의 메모리를 제거할 수 
 * 없게 되는 문제가 있다.)
 */
	usemap = sparse_early_usemaps_alloc_pgdat_section(NODE_DATA(nodeid),
							  size * usemap_count);
	if (!usemap) {
		printk(KERN_WARNING "%s: allocation failed\n", __func__);
		return;
	}

/* IAMROOT-12AB:
 * -------------
 * usemap_map에 대한 초기화를 수행한다.
 *	usemap_map[]은 할당받은 usemap의 적절한 위치를 가리킨다.
 *	(섹션들은 할당받은 주소에 usemap 사이즈만큼 계속 증가한 위치를 가리킨다)
 */
	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		if (!present_section_nr(pnum))
			continue;
		usemap_map[pnum] = usemap;
		usemap += size;

/* IAMROOT-12AB:
 * -------------
 * usemap과 NODE_DATA()가 다른 노드에 위치한 경우 경고 메시지를 출력한다.
 */
		check_usemap_section_nr(nodeid, usemap_map[pnum]);
	}
}

#ifndef CONFIG_SPARSEMEM_VMEMMAP
struct page __init *sparse_mem_map_populate(unsigned long pnum, int nid)
{
	struct page *map;
	unsigned long size;

/* IAMROOT-12AB:
 * -------------
 * 특정 아키텍처(tile)에서 수행된다. 
 * arm에서는 null로 리턴된다.
 */
	map = alloc_remap(nid, sizeof(struct page) * PAGES_PER_SECTION);
	if (map)
		return map;

/* IAMROOT-12AB:
 * -------------
 * 한 개의 섹션에 허용되는 페이지 수 만큼 struct page 공간을 할당한다.
 */
	size = PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION);
	map = memblock_virt_alloc_try_nid(size,
					  PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
					  BOOTMEM_ALLOC_ACCESSIBLE, nid);
	return map;
}
void __init sparse_mem_maps_populate_node(struct page **map_map,
					  unsigned long pnum_begin,
					  unsigned long pnum_end,
					  unsigned long map_count, int nodeid)
{
	void *map;
	unsigned long pnum;
	unsigned long size = sizeof(struct page) * PAGES_PER_SECTION;

	map = alloc_remap(nodeid, size * map_count);
	if (map) {
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			map_map[pnum] = map;
			map += size;
		}
		return;
	}

	size = PAGE_ALIGN(size);
	map = memblock_virt_alloc_try_nid(size * map_count,
					  PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
					  BOOTMEM_ALLOC_ACCESSIBLE, nodeid);
	if (map) {
		for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
			if (!present_section_nr(pnum))
				continue;
			map_map[pnum] = map;
			map += size;
		}
		return;
	}

	/* fallback */
	for (pnum = pnum_begin; pnum < pnum_end; pnum++) {
		struct mem_section *ms;

		if (!present_section_nr(pnum))
			continue;
		map_map[pnum] = sparse_mem_map_populate(pnum, nodeid);
		if (map_map[pnum])
			continue;
		ms = __nr_to_section(pnum);
		printk(KERN_ERR "%s: sparsemem memory map backing failed "
			"some memory will not be available.\n", __func__);
		ms->section_mem_map = 0;
	}
}
#endif /* !CONFIG_SPARSEMEM_VMEMMAP */

#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
static void __init sparse_early_mem_maps_alloc_node(void *data,
				 unsigned long pnum_begin,
				 unsigned long pnum_end,
				 unsigned long map_count, int nodeid)
{
	struct page **map_map = (struct page **)data;
	sparse_mem_maps_populate_node(map_map, pnum_begin, pnum_end,
					 map_count, nodeid);
}
#else
static struct page __init *sparse_early_mem_map_alloc(unsigned long pnum)
{
	struct page *map;
	struct mem_section *ms = __nr_to_section(pnum);
	int nid = sparse_early_nid(ms);


/* IAMROOT-12AB:
 * -------------
 * 지정된 노드에 한 개의 섹션에 허용되는 페이지 수 만큼 struct page 공간을 할당한다.
 */
	map = sparse_mem_map_populate(pnum, nid);
	if (map)
		return map;

	printk(KERN_ERR "%s: sparsemem memory map backing failed "
			"some memory will not be available.\n", __func__);
	ms->section_mem_map = 0;
	return NULL;
}
#endif

void __weak __meminit vmemmap_populate_print_last(void)
{
}

/**
 *  alloc_usemap_and_memmap - memory alloction for pageblock flags and vmemmap
 *  @map: usemap_map for pageblock flags or mmap_map for vmemmap
 */
static void __init alloc_usemap_and_memmap(void (*alloc_func)
					(void *, unsigned long, unsigned long,
					unsigned long, int), void *data)
{
	unsigned long pnum;
	unsigned long map_count;
	int nodeid_begin = 0;
	unsigned long pnum_begin = 0;


/* IAMROOT-12AB:
 * -------------
 * 아래 처음 루프는 메모리가 있는 처음 섹션을 알아내어
 * 섹션번호와 노드id를 기억한다.
 *
 * map_count: 각 노드에서 메모리가 있는 섹션의 갯수
 */

	for (pnum = 0; pnum < NR_MEM_SECTIONS; pnum++) {
		struct mem_section *ms;

/* IAMROOT-12AB:
 * -------------
 * 섹션에 메모리가 없는 경우 현재 섹션은 skip한다.
 */
		if (!present_section_nr(pnum))
			continue;
		ms = __nr_to_section(pnum);

/* IAMROOT-12AB:
 * -------------
 * 처음 만난 메모리가 있는 섹션의 노드와 섹션 번호를 저장한다.
 */
		nodeid_begin = sparse_early_nid(ms);
		pnum_begin = pnum;
		break;
	}

/* IAMROOT-12AB:
 * -------------
 * 아래 루프는 노드가 바뀔 때 기존 노드에 대한 섹션 갯수를 알아내고
 * alloc_func() 함수를 호출한다.
 */
	map_count = 1;
	for (pnum = pnum_begin + 1; pnum < NR_MEM_SECTIONS; pnum++) {
		struct mem_section *ms;
		int nodeid;

/* IAMROOT-12AB:
 * -------------
 * 메모리가 없는 섹션은 skip
 */
		if (!present_section_nr(pnum))
			continue;
		ms = __nr_to_section(pnum);
		nodeid = sparse_early_nid(ms);

/* IAMROOT-12AB:
 * -------------
 * 읽어들인 섹션에서 노드가 동일한 경우 map_count++ 증가시키고 계속한다.
 */
		if (nodeid == nodeid_begin) {
			map_count++;
			continue;
		}

/* IAMROOT-12AB:
 * -------------
 * 한 개 노드가 끝날 때마다 alloc_func()를 호출한다.
 */
		/* ok, we need to take cake of from pnum_begin to pnum - 1*/
		alloc_func(data, pnum_begin, pnum,
						map_count, nodeid_begin);
		/* new start, update count etc*/

/* IAMROOT-12AB:
 * -------------
 * 노드가 변경되었으므로 변경된 노드로 다시 루프를 돌기위해 준비한다.
 * 바뀐 노드와 섹션 번호를 다시 기억하고 map_count도 1로 대입한다.
 */
		nodeid_begin = nodeid;
		pnum_begin = pnum;
		map_count = 1;
	}
	/* ok, last chunk */
	alloc_func(data, pnum_begin, NR_MEM_SECTIONS,
						map_count, nodeid_begin);
}

/*
 * Allocate the accumulated non-linear sections, allocate a mem_map
 * for each and record the physical to section mapping.
 */
void __init sparse_init(void)
{
	unsigned long pnum;
	struct page *map;
	unsigned long *usemap;
	unsigned long **usemap_map;
	int size;
#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	int size2;
	struct page **map_map;
#endif

	/* see include/linux/mmzone.h 'struct mem_section' definition */
	BUILD_BUG_ON(!is_power_of_2(sizeof(struct mem_section)));

	/* Setup pageblock_order for HUGETLB_PAGE_SIZE_VARIABLE */

/* IAMROOT-12AB:
 * -------------
 * HUGETLB_PAGE_SIZE_VARIABLE 커널 옵션을 사용하는 경우에 pageblock_order
 * 를 변수로 선언하여 값을 계산하여 사용하게 한다.
 *
 * 보통 pageblock_order는 매크로 상수로 define되어 사용한다.
 * (default로 MAX_ORDER-1)
 */
	set_pageblock_order();

	/*
	 * map is using big page (aka 2M in x86 64 bit)
	 * usemap is less one page (aka 24 bytes)
	 * so alloc 2M (with 2M align) and 24 bytes in turn will
	 * make next 2M slip to one more 2M later.
	 * then in big system, the memory will have a lot of holes...
	 * here try to allocate 2M pages continuously.
	 *
	 * powerpc need to call sparse_init_one_section right after each
	 * sparse_early_mem_map_alloc, so allocate usemap_map at first.
	 */

/* IAMROOT-12AB:
 * -------------
 * usemap_map의 크기는 포인터 사이즈 x 섹션 수로 이루어진다.
 */
	size = sizeof(unsigned long *) * NR_MEM_SECTIONS;

/* IAMROOT-12AB:
 * -------------
 * usemap: pageblock 단위마다 4bits의 mobility를 담는다.
 *         pageblock은 커널 설정마다 다르다. (default=2^(MAX-ORDER-1) pages=4MB)
 *         rpi2: pageblock_order=10, pageblock_nr_pages=1024
 *
 * usemap_map: 섹션마다 usemap을 가리킨다.
 */
	usemap_map = memblock_virt_alloc(size, 0);
	if (!usemap_map)
		panic("can not allocate usemap_map\n");

/* IAMROOT-12AB:
 * -------------
 * alloc_usemap_and_memmap()에서 첫 번째 인수에 주어진
 * usemap 또는 mem_map 할당자를 노드마다 호출한다.
 */
	alloc_usemap_and_memmap(sparse_early_usemaps_alloc_node,
							(void *)usemap_map);

/* IAMROOT-12AB:
 * -------------
 * 아래 커널 옵션은 x86_64에서만 동작한다.
 */
#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	size2 = sizeof(struct page *) * NR_MEM_SECTIONS;
	map_map = memblock_virt_alloc(size2, 0);
	if (!map_map)
		panic("can not allocate map_map\n");
	alloc_usemap_and_memmap(sparse_early_mem_maps_alloc_node,
							(void *)map_map);
#endif

	for (pnum = 0; pnum < NR_MEM_SECTIONS; pnum++) {

/* IAMROOT-12AB:
 * -------------
 * 메모리가 없는 섹션은 생략한다.
 */
		if (!present_section_nr(pnum))
			continue;

/* IAMROOT-12AB:
 * -------------
 * 섹션과 관련있는 usemap의 주소를 알아온다.
 */
		usemap = usemap_map[pnum];
		if (!usemap)
			continue;

#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
		map = map_map[pnum];
#else
/* IAMROOT-12AB:
 * -------------
 * 지정된 노드에 현재 섹션에 대한 mem_map을 할당받는다.
 * (한 개의 섹션에 허용되는 페이지 수 만큼 struct page 공간을 할당)
 */
		map = sparse_early_mem_map_alloc(pnum);
#endif
		if (!map)
			continue;

/* IAMROOT-12AB:
 * -------------
 * 섹션 초기화를 수행한다.
 */
		sparse_init_one_section(__nr_to_section(pnum), pnum, map,
								usemap);
	}

	vmemmap_populate_print_last();

/* IAMROOT-12AB:
 * -------------
 * map_map[] 배열 영역과 usemap_map[] 배열 영역을 free 시킨다.
 * 할당받은 각 usemap은 섹션 별로 mem_section의 멤버 pageblock_flags에 연결된다.
 */
#ifdef CONFIG_SPARSEMEM_ALLOC_MEM_MAP_TOGETHER
	memblock_free_early(__pa(map_map), size2);
#endif
	memblock_free_early(__pa(usemap_map), size);
}

#ifdef CONFIG_MEMORY_HOTPLUG
#ifdef CONFIG_SPARSEMEM_VMEMMAP
static inline struct page *kmalloc_section_memmap(unsigned long pnum, int nid)
{
	/* This will make the necessary allocations eventually. */
	return sparse_mem_map_populate(pnum, nid);
}
static void __kfree_section_memmap(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end);
}
#ifdef CONFIG_MEMORY_HOTREMOVE
static void free_map_bootmem(struct page *memmap)
{
	unsigned long start = (unsigned long)memmap;
	unsigned long end = (unsigned long)(memmap + PAGES_PER_SECTION);

	vmemmap_free(start, end);
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#else
static struct page *__kmalloc_section_memmap(void)
{
	struct page *page, *ret;
	unsigned long memmap_size = sizeof(struct page) * PAGES_PER_SECTION;

	page = alloc_pages(GFP_KERNEL|__GFP_NOWARN, get_order(memmap_size));
	if (page)
		goto got_map_page;

	ret = vmalloc(memmap_size);
	if (ret)
		goto got_map_ptr;

	return NULL;
got_map_page:
	ret = (struct page *)pfn_to_kaddr(page_to_pfn(page));
got_map_ptr:

	return ret;
}

static inline struct page *kmalloc_section_memmap(unsigned long pnum, int nid)
{
	return __kmalloc_section_memmap();
}

static void __kfree_section_memmap(struct page *memmap)
{
	if (is_vmalloc_addr(memmap))
		vfree(memmap);
	else
		free_pages((unsigned long)memmap,
			   get_order(sizeof(struct page) * PAGES_PER_SECTION));
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static void free_map_bootmem(struct page *memmap)
{
	unsigned long maps_section_nr, removing_section_nr, i;
	unsigned long magic, nr_pages;
	struct page *page = virt_to_page(memmap);

	nr_pages = PAGE_ALIGN(PAGES_PER_SECTION * sizeof(struct page))
		>> PAGE_SHIFT;

	for (i = 0; i < nr_pages; i++, page++) {
		magic = (unsigned long) page->lru.next;

		BUG_ON(magic == NODE_INFO);

		maps_section_nr = pfn_to_section_nr(page_to_pfn(page));
		removing_section_nr = page->private;

		/*
		 * When this function is called, the removing section is
		 * logical offlined state. This means all pages are isolated
		 * from page allocator. If removing section's memmap is placed
		 * on the same section, it must not be freed.
		 * If it is freed, page allocator may allocate it which will
		 * be removed physically soon.
		 */
		if (maps_section_nr != removing_section_nr)
			put_page_bootmem(page);
	}
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#endif /* CONFIG_SPARSEMEM_VMEMMAP */

/*
 * returns the number of sections whose mem_maps were properly
 * set.  If this is <=0, then that means that the passed-in
 * map was not consumed and must be freed.
 */
int __meminit sparse_add_one_section(struct zone *zone, unsigned long start_pfn)
{
	unsigned long section_nr = pfn_to_section_nr(start_pfn);
	struct pglist_data *pgdat = zone->zone_pgdat;
	struct mem_section *ms;
	struct page *memmap;
	unsigned long *usemap;
	unsigned long flags;
	int ret;

	/*
	 * no locking for this, because it does its own
	 * plus, it does a kmalloc
	 */
	ret = sparse_index_init(section_nr, pgdat->node_id);
	if (ret < 0 && ret != -EEXIST)
		return ret;
	memmap = kmalloc_section_memmap(section_nr, pgdat->node_id);
	if (!memmap)
		return -ENOMEM;
	usemap = __kmalloc_section_usemap();
	if (!usemap) {
		__kfree_section_memmap(memmap);
		return -ENOMEM;
	}

	pgdat_resize_lock(pgdat, &flags);

	ms = __pfn_to_section(start_pfn);
	if (ms->section_mem_map & SECTION_MARKED_PRESENT) {
		ret = -EEXIST;
		goto out;
	}

	memset(memmap, 0, sizeof(struct page) * PAGES_PER_SECTION);

	ms->section_mem_map |= SECTION_MARKED_PRESENT;

	ret = sparse_init_one_section(ms, section_nr, memmap, usemap);

out:
	pgdat_resize_unlock(pgdat, &flags);
	if (ret <= 0) {
		kfree(usemap);
		__kfree_section_memmap(memmap);
	}
	return ret;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
#ifdef CONFIG_MEMORY_FAILURE
static void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
	int i;

	if (!memmap)
		return;

	for (i = 0; i < PAGES_PER_SECTION; i++) {
		if (PageHWPoison(&memmap[i])) {
			atomic_long_sub(1, &num_poisoned_pages);
			ClearPageHWPoison(&memmap[i]);
		}
	}
}
#else
static inline void clear_hwpoisoned_pages(struct page *memmap, int nr_pages)
{
}
#endif

static void free_section_usemap(struct page *memmap, unsigned long *usemap)
{
	struct page *usemap_page;

	if (!usemap)
		return;

	usemap_page = virt_to_page(usemap);
	/*
	 * Check to see if allocation came from hot-plug-add
	 */
	if (PageSlab(usemap_page) || PageCompound(usemap_page)) {
		kfree(usemap);
		if (memmap)
			__kfree_section_memmap(memmap);
		return;
	}

	/*
	 * The usemap came from bootmem. This is packed with other usemaps
	 * on the section which has pgdat at boot time. Just keep it as is now.
	 */

	if (memmap)
		free_map_bootmem(memmap);
}

void sparse_remove_one_section(struct zone *zone, struct mem_section *ms)
{
	struct page *memmap = NULL;
	unsigned long *usemap = NULL, flags;
	struct pglist_data *pgdat = zone->zone_pgdat;

	pgdat_resize_lock(pgdat, &flags);
	if (ms->section_mem_map) {
		usemap = ms->pageblock_flags;
		memmap = sparse_decode_mem_map(ms->section_mem_map,
						__section_nr(ms));
		ms->section_mem_map = 0;
		ms->pageblock_flags = NULL;
	}
	pgdat_resize_unlock(pgdat, &flags);

	clear_hwpoisoned_pages(memmap, PAGES_PER_SECTION);
	free_section_usemap(memmap, usemap);
}
#endif /* CONFIG_MEMORY_HOTREMOVE */
#endif /* CONFIG_MEMORY_HOTPLUG */
