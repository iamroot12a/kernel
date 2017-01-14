#ifndef _LINUX_SLUB_DEF_H
#define _LINUX_SLUB_DEF_H

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter
 */
#include <linux/kobject.h>

enum stat_item {
	ALLOC_FASTPATH,		/* Allocation from cpu slab */
	ALLOC_SLOWPATH,		/* Allocation by getting a new cpu slab */
	FREE_FASTPATH,		/* Free to cpu slab */
	FREE_SLOWPATH,		/* Freeing not to cpu slab */
	FREE_FROZEN,		/* Freeing to frozen slab */
	FREE_ADD_PARTIAL,	/* Freeing moves slab to partial list */
	FREE_REMOVE_PARTIAL,	/* Freeing removes last object */
	ALLOC_FROM_PARTIAL,	/* Cpu slab acquired from node partial list */
	ALLOC_SLAB,		/* Cpu slab acquired from page allocator */
	ALLOC_REFILL,		/* Refill cpu slab from slab freelist */
	ALLOC_NODE_MISMATCH,	/* Switching cpu slab */
	FREE_SLAB,		/* Slab freed to the page allocator */
	CPUSLAB_FLUSH,		/* Abandoning of the cpu slab */
	DEACTIVATE_FULL,	/* Cpu slab was full when deactivated */
	DEACTIVATE_EMPTY,	/* Cpu slab was empty when deactivated */
	DEACTIVATE_TO_HEAD,	/* Cpu slab was moved to the head of partials */
	DEACTIVATE_TO_TAIL,	/* Cpu slab was moved to the tail of partials */
	DEACTIVATE_REMOTE_FREES,/* Slab contained remotely freed objects */
	DEACTIVATE_BYPASS,	/* Implicit deactivation */
	ORDER_FALLBACK,		/* Number of times fallback was necessary */
	CMPXCHG_DOUBLE_CPU_FAIL,/* Failure of this_cpu_cmpxchg_double */
	CMPXCHG_DOUBLE_FAIL,	/* Number of times that cmpxchg double did not match */
	CPU_PARTIAL_ALLOC,	/* Used cpu partial on alloc */
	CPU_PARTIAL_FREE,	/* Refill cpu partial on free */
	CPU_PARTIAL_NODE,	/* Refill cpu partial from node partial */
	CPU_PARTIAL_DRAIN,	/* Drain cpu partial to node partial */
	NR_SLUB_STAT_ITEMS };

struct kmem_cache_cpu {
	void **freelist;	/* Pointer to next available object */
	unsigned long tid;	/* Globally unique transaction id */
	struct page *page;	/* The slab from which we are allocating */
	struct page *partial;	/* Partially allocated frozen slabs */
#ifdef CONFIG_SLUB_STATS
	unsigned stat[NR_SLUB_STAT_ITEMS];
#endif
};

/*
 * Word size structure that can be atomically updated or read and that
 * contains both the order and the number of objects that a slab of the
 * given order would contain.
 */
struct kmem_cache_order_objects {
	unsigned long x;
};

/*
 * Slab cache management.
 */
struct kmem_cache {
	struct kmem_cache_cpu __percpu *cpu_slab;
	/* Used for retriving partial slabs etc */
	unsigned long flags;

/* IAMROOT-12:
 * -------------
 * min_partial: 각 노드에서 적절히 유지할 slub 페이지 수
 */
	unsigned long min_partial;
	int size;		/* The size of an object including meta data */
	int object_size;	/* The size of an object without meta data */

/* IAMROOT-12:
 * -------------
 * FP(Free Pointer)까지의 offset가 담긴다.
 * (poison, track 등의 디버그 정보가 있는 경우에는 FP가 이동되고 그 FP까지의 
 * offset이 지정되지만 없는 경우에는 0이 대입된다.)
 */
	int offset;		/* Free pointer offset. */

/* IAMROOT-12:
 * -------------
 * cpu partial 리스트가 적절히 소유할 object 수
 */
	int cpu_partial;	/* Number of per cpu partial objects to keep around */
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	struct kmem_cache_order_objects max;
	struct kmem_cache_order_objects min;
	gfp_t allocflags;	/* gfp flags to use on each alloc */
	int refcount;		/* Refcount for slab cache destroy */
	void (*ctor)(void *);
/* IAMROOT-12:
 * -------------
 * 메타데이터 까지의 offset가 담긴다.
 * (red-zone 정보가 포함된 size와 동일하고, red-zone이 없는 경우에는 
 *  포인터 주소 길이로 정렬된 object size와 같다)
 * (디버그 정보가 있는 경우에는 FP까지의 offset가 담긴 s->offset와 동일하다.)
 *
 * 예) 어떠한 디버그 정보도 없는 경우 object_size=5:   size=8, offset=0, inuse=8 
 *     red-zone만 추가된 경우         object_size=5:   size=8, offset=0, inuse=8
 *                                    object_size=4:   size=8, offset=0, inuse=8
 *     poison만 추가된 경우           object_size=5:   size=12, offset=8, inuse=8 
 *     red-zone+poison 추가된 경우    object_size=5:   size=12, offset=8, inuse=8
 *                                    onject_size=4:   size=12, offset=8, inuse=8
 */
	int inuse;		/* Offset to metadata */
	int align;		/* Alignment */
	int reserved;		/* Reserved bytes at the end of slabs */
	const char *name;	/* Name (only for display!) */
	struct list_head list;	/* List of slab caches */
#ifdef CONFIG_SYSFS
	struct kobject kobj;	/* For sysfs */
#endif
#ifdef CONFIG_MEMCG_KMEM
	struct memcg_cache_params memcg_params;
	int max_attr_size; /* for propagation, maximum size of a stored attr */
#ifdef CONFIG_SYSFS
	struct kset *memcg_kset;
#endif
#endif

#ifdef CONFIG_NUMA
	/*
	 * Defragmentation by allocating from a remote node.
	 */
	int remote_node_defrag_ratio;
#endif
	struct kmem_cache_node *node[MAX_NUMNODES];
};

#ifdef CONFIG_SYSFS
#define SLAB_SUPPORTS_SYSFS
void sysfs_slab_remove(struct kmem_cache *);
#else
static inline void sysfs_slab_remove(struct kmem_cache *s)
{
}
#endif


/**
 * virt_to_obj - returns address of the beginning of object.
 * @s: object's kmem_cache
 * @slab_page: address of slab page
 * @x: address within object memory range
 *
 * Returns address of the beginning of object
 */
static inline void *virt_to_obj(struct kmem_cache *s,
				const void *slab_page,
				const void *x)
{
	return (void *)x - ((x - slab_page) % s->size);
}

void object_err(struct kmem_cache *s, struct page *page,
		u8 *object, char *reason);

#endif /* _LINUX_SLUB_DEF_H */
