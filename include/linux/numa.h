#ifndef _LINUX_NUMA_H
#define _LINUX_NUMA_H


#ifdef CONFIG_NODES_SHIFT
#define NODES_SHIFT     CONFIG_NODES_SHIFT
#else
#define NODES_SHIFT     0
#endif


/* IAMROOT-12AB:
 * -------------
 * 전체 누마 노드 수
 * rpi2: MAX_NUMNODES=1
 */
#define MAX_NUMNODES    (1 << NODES_SHIFT)


/* IAMROOT-12AB:
 * -------------
 * 어떤 NUMA 노드이든 상관 없을 때
 */
#define	NUMA_NO_NODE	(-1)

#endif /* _LINUX_NUMA_H */
