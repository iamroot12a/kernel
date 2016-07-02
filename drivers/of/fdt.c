/*
 * Functions for working with the Flattened Device Tree data format
 *
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/crc32.h>
#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/libfdt.h>
#include <linux/debugfs.h>
#include <linux/serial_core.h>
#include <linux/sysfs.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */
#include <asm/page.h>

/*
 * of_fdt_limit_memory - limit the number of regions in the /memory node
 * @limit: maximum entries
 *
 * Adjust the flattened device tree to have at most 'limit' number of
 * memory entries in the /memory node. This function may be called
 * any time after initial_boot_param is set.
 */
void of_fdt_limit_memory(int limit)
{
	int memory;
	int len;
	const void *val;
	int nr_address_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
	int nr_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	const uint32_t *addr_prop;
	const uint32_t *size_prop;
	int root_offset;
	int cell_size;

	root_offset = fdt_path_offset(initial_boot_params, "/");
	if (root_offset < 0)
		return;

	addr_prop = fdt_getprop(initial_boot_params, root_offset,
				"#address-cells", NULL);
	if (addr_prop)
		nr_address_cells = fdt32_to_cpu(*addr_prop);

	size_prop = fdt_getprop(initial_boot_params, root_offset,
				"#size-cells", NULL);
	if (size_prop)
		nr_size_cells = fdt32_to_cpu(*size_prop);

	cell_size = sizeof(uint32_t)*(nr_address_cells + nr_size_cells);

	memory = fdt_path_offset(initial_boot_params, "/memory");
	if (memory > 0) {
		val = fdt_getprop(initial_boot_params, memory, "reg", &len);
		if (len > limit*cell_size) {
			len = limit*cell_size;
			pr_debug("Limiting number of entries to %d\n", limit);
			fdt_setprop(initial_boot_params, memory, "reg", val,
					len);
		}
	}
}

/**
 * of_fdt_is_compatible - Return true if given node from the given blob has
 * compat in its compatible list
 * @blob: A device tree blob
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 *
 * On match, returns a non-zero value with smaller values returned for more
 * specific compatible values.
 */
int of_fdt_is_compatible(const void *blob,
		      unsigned long node, const char *compat)
{
	const char *cp;
	int cplen;
	unsigned long l, score = 0;

	cp = fdt_getprop(blob, node, "compatible", &cplen);
	if (cp == NULL)
		return 0;
	while (cplen > 0) {
		score++;
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return score;
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
	}

	return 0;
}

/**
 * of_fdt_match - Return true if node matches a list of compatible values
 */
int of_fdt_match(const void *blob, unsigned long node,
                 const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	while (*compat) {
		tmp = of_fdt_is_compatible(blob, node, *compat);
		if (tmp && (score == 0 || (tmp < score)))
			score = tmp;
		compat++;
	}

	return score;
}

static void *unflatten_dt_alloc(void **mem, unsigned long size,
				       unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	res = *mem;
	*mem += size;

	return res;
}

/**
 * unflatten_dt_node - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @p: pointer to node in flat tree
 * @dad: Parent struct device_node
 * @fpsize: Size of the node path up at the current depth.
 */
static void * unflatten_dt_node(void *blob,
				void *mem,
				int *poffset,
				struct device_node *dad,
				struct device_node **nodepp,
				unsigned long fpsize,
				bool dryrun)
{
	const __be32 *p;
	struct device_node *np;
	struct property *pp, **prev_pp = NULL;
	const char *pathp;
	unsigned int l, allocl;
	static int depth = 0;
	int old_depth;
	int offset;
	int has_name = 0;
	int new_format = 0;

	pathp = fdt_get_name(blob, *poffset, &l);
	if (!pathp)
		return mem;


/* IAMROOT-12AB:
 * -------------
 * 기존(구) DTB를 사용하는 경우 allocl 값은 아래코드로 지정하고 그렇지 않은 경우는
 * 다시 재계산된다.
 */

	allocl = l++;

	/* version 0x10 has a more compact unit name here instead of the full
	 * path. we accumulate the full path size using "fpsize", we'll rebuild
	 * it later. We detect this because the first character of the name is
	 * not '/'.
	 */

/* IAMROOT-12AB:
 * -------------
 * 기존 DTB는 full path명을 사용하였고 항상 /로 시작하였다.
 * 현재 DTB 0x10은 full path명을 사용하지 않고 루트 노드마저도 
 * null(*.dtb의 바이너리)로 구성되어 있다.
 */

	if ((*pathp) != '/') {
		new_format = 1;

/* IAMROOT-12AB:
 * -------------
 * 루트 노드일 때에만 fpsize가 0이다.
 */

		if (fpsize == 0) {
			/* root node: special case. fpsize accounts for path
			 * plus terminating zero. root node only has '/', so
			 * fpsize should be 2, but we want to avoid the first
			 * level nodes to have two '/' so we use fpsize 1 here
			 */
			fpsize = 1;
			allocl = 2;
			l = 1;
			pathp = "";
		} else {
			/* account for '/' and path size minus terminal 0
			 * already in 'l'
			 */
			fpsize += l;
			allocl = fpsize;
		}
	}


/* IAMROOT-12AB:
 * -------------
 * 각 노드의 할당을 위해 device_node 구조체 크기로 정렬한다.
 * 
 * 추가된 allocl(full path size)이 device_node 하나를 차지하므로 
 * 비능률적으로 보이는데 그 원인은???
 */

	np = unflatten_dt_alloc(&mem, sizeof(struct device_node) + allocl,
				__alignof__(struct device_node));
	if (!dryrun) {
		char *fn;
		of_node_init(np);

/* IAMROOT-12AB:
 * -------------
 * full_name은 device_node의 다음에 붙어 있는 주소를 가리킨다.
 */
		np->full_name = fn = ((char *)np) + sizeof(*np);
		if (new_format) {
			/* rebuild full path for new format */
/* IAMROOT-12AB:
 * -------------
 * 1) 현재 depth=0 노드인 경우
 *	'/'만 추가된다.
 * 1) 현재 depth=1 노드인 경우
 *	'/'+ 노드명
 * 2) 현재 depth>1 노드인 경우
 *	부모노드명 + '/' + 노드명
 */
			if (dad && dad->parent) {
				strcpy(fn, dad->full_name);
#ifdef DEBUG
				if ((strlen(fn) + l + 1) != allocl) {
					pr_debug("%s: p: %d, l: %d, a: %d\n",
						pathp, (int)strlen(fn),
						l, allocl);
				}
#endif
				fn += strlen(fn);
			}

			*(fn++) = '/';
		}
		memcpy(fn, pathp, l);

		prev_pp = &np->properties;

/* IAMROOT-12AB:
 * -------------
 * 노드를 추가
 */

		if (dad != NULL) {
			np->parent = dad;
			np->sibling = dad->child;
			dad->child = np;
		}
	}
	/* process properties */

/* IAMROOT-12AB:
 * -------------
 * blob + (structure block offset) + poffset가 가리키는 노드로부터 처음 속성부터
 * 해당 노드의 마지막 속성까지 루프를 돈다.
 */
	for (offset = fdt_first_property_offset(blob, *poffset);
	     (offset >= 0);
	     (offset = fdt_next_property_offset(blob, offset))) {
		const char *pname;
		u32 sz;

		if (!(p = fdt_getprop_by_offset(blob, offset, &pname, &sz))) {
			offset = -FDT_ERR_INTERNAL;
			break;
		}

		if (pname == NULL) {
			pr_info("Can't find property name in list !\n");
			break;
		}

/* IAMROOT-12AB:
 * -------------
 * 최신 DTB 0x10은 각 노드마다 name 속성이 없을 수 있다. 따라서 끝까지 검색하여
 * name 속성이 발견되지 않는 경우 마지막에 name 속성을 추가한다.
 */
		if (strcmp(pname, "name") == 0)
			has_name = 1;
		pp = unflatten_dt_alloc(&mem, sizeof(struct property),
					__alignof__(struct property));
		if (!dryrun) {
			/* We accept flattened tree phandles either in
			 * ePAPR-style "phandle" properties, or the
			 * legacy "linux,phandle" properties.  If both
			 * appear and have different values, things
			 * will get weird.  Don't do that. */

/* IAMROOT-12AB:
 * -------------
 * phandle 속성을 찾아 노드 구조체의 phandle 값으로 대입한다.
 */
			if ((strcmp(pname, "phandle") == 0) ||
			    (strcmp(pname, "linux,phandle") == 0)) {
				if (np->phandle == 0)
					np->phandle = be32_to_cpup(p);
			}
			/* And we process the "ibm,phandle" property
			 * used in pSeries dynamic device tree
			 * stuff */
			if (strcmp(pname, "ibm,phandle") == 0)
				np->phandle = be32_to_cpup(p);

/* IAMROOT-12AB:
 * -------------
 * 속성값을 채우고 추가 연결한다.
 */
			pp->name = (char *)pname;
			pp->length = sz;
			pp->value = (__be32 *)p;
			*prev_pp = pp;
			prev_pp = &pp->next;
		}
	}
	/* with version 0x10 we may not have the name property, recreate
	 * it here from the unit name if absent
	 */
	if (!has_name) {
		const char *p1 = pathp, *ps = pathp, *pa = NULL;
		int sz;

/* IAMROOT-12AB:
 * -------------
 * 아래 pa와 ps를 산출한다.
 * pa: pointer of at(@)
 * ps: pointer of slash(/) (발견하지 못한 경우 노드명의 처음 주소)
 */
		while (*p1) {
			if ((*p1) == '@')
				pa = p1;
			if ((*p1) == '/')
				ps = p1 + 1;
			p1++;
		}
		if (pa < ps)
			pa = p1;

/* IAMROOT-12AB:
 * -------------
 * 예) abc@1000
 *     -> sz= 3+1(null)
 * 예) /abc@2000
 *     -> sz= 3+1(null)
 */
		sz = (pa - ps) + 1;

/* IAMROOT-12AB:
 * -------------
 * name 속성의 경우 노드 방식과 같이 property 구조체뒤에 compact 노드명을 추가한다.
 */
		pp = unflatten_dt_alloc(&mem, sizeof(struct property) + sz,
					__alignof__(struct property));
		if (!dryrun) {
			pp->name = "name";
			pp->length = sz;

/* IAMROOT-12AB:
 * -------------
 * value는 compact(골뱅이 빠진) 노드명을 가리킨다.
 * 속성 구조체 뒤에 compact 노드명이 복사된다.
 */
			pp->value = pp + 1;
			*prev_pp = pp;
			prev_pp = &pp->next;
			memcpy(pp->value, ps, sz - 1);
			((char *)pp->value)[sz - 1] = 0;
			pr_debug("fixed up name for %s -> %s\n", pathp,
				(char *)pp->value);
		}
	}
	if (!dryrun) {
		*prev_pp = NULL;

/* IAMROOT-12AB:
 * -------------
 * 노드 구조체의 name에는 comapct 노드명이 대입된다.
 * 노드 구조체의 type에는 device_type 속성값을 읽어와서 대입한다.
 */
		np->name = of_get_property(np, "name", NULL);
		np->type = of_get_property(np, "device_type", NULL);

		if (!np->name)
			np->name = "<NULL>";
		if (!np->type)
			np->type = "<NULL>";
	}

	old_depth = depth;
	*poffset = fdt_next_node(blob, *poffset, &depth);
	if (depth < 0)
		depth = 0;

/* IAMROOT-12AB:
 * -------------
 * sub 노드가 있는 경우 재귀호출을 한다.
 */
	while (*poffset > 0 && depth > old_depth)
		mem = unflatten_dt_node(blob, mem, poffset, np, NULL,
					fpsize, dryrun);

	if (*poffset < 0 && *poffset != -FDT_ERR_NOTFOUND)
		pr_err("unflatten: error %d processing FDT\n", *poffset);

	/*
	 * Reverse the child list. Some drivers assumes node order matches .dts
	 * node order
	 */

/* IAMROOT-12AB:
 * -------------
 * 형제 노드를 reverse 처리한다.
 */
	if (!dryrun && np->child) {
		struct device_node *child = np->child;
		np->child = NULL;
		while (child) {
			struct device_node *next = child->sibling;
			child->sibling = np->child;
			np->child = child;
			child = next;
		}
	}

/* IAMROOT-12AB:
 * -------------
 * 인수로 변수가 지정된 경우 np를 대입한다.
 */
	if (nodepp)
		*nodepp = np;

	return mem;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens a device-tree, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * @blob: The blob to expand
 * @mynodes: The device_node tree created by the call
 * @dt_alloc: An allocator that provides a virtual address to memory
 * for the resulting tree
 */
static void __unflatten_device_tree(void *blob,
			     struct device_node **mynodes,
			     void * (*dt_alloc)(u64 size, u64 align))
{
	unsigned long size;
	int start;
	void *mem;

	pr_debug(" -> unflatten_device_tree()\n");

	if (!blob) {
		pr_debug("No device tree pointer\n");
		return;
	}

	pr_debug("Unflattening device tree:\n");
	pr_debug("magic: %08x\n", fdt_magic(blob));
	pr_debug("size: %08x\n", fdt_totalsize(blob));
	pr_debug("version: %08x\n", fdt_version(blob));

	if (fdt_check_header(blob)) {
		pr_err("Invalid device tree blob header\n");
		return;
	}

	/* First pass, scan for size */

/* IAMROOT-12AB:
 * -------------
 * DTB를 분석하여 할당할 사이즈를 파악한다.
 */
	start = 0;
	size = (unsigned long)unflatten_dt_node(blob, NULL, &start, NULL, NULL, 0, true);
	size = ALIGN(size, 4);

	pr_debug("  size is %lx, allocating...\n", size);

	/* Allocate memory for the expanded device tree */

/* IAMROOT-12AB:
 * -------------
 * 파악된 사이즈 + 4로 메모리 할당을 한다.
 */
	mem = dt_alloc(size + 4, __alignof__(struct device_node));
	memset(mem, 0, size);


/* IAMROOT-12AB:
 * -------------
 * 사이즈의 마지막 4바이트에 0xdeadbeef를 저장한다.
 */
	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);

	pr_debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	start = 0;

/* IAMROOT-12AB:
 * -------------
 * 실제 노드와 속성을 풀어서 대입한다.
 */
	unflatten_dt_node(blob, mem, &start, NULL, mynodes, 0, false);
	if (be32_to_cpup(mem + size) != 0xdeadbeef)
		pr_warning("End of tree marker overwritten: %08x\n",
			   be32_to_cpup(mem + size));

	pr_debug(" <- unflatten_device_tree()\n");
}

static void *kernel_tree_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

/**
 * of_fdt_unflatten_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
void of_fdt_unflatten_tree(unsigned long *blob,
			struct device_node **mynodes)
{
	__unflatten_device_tree(blob, mynodes, &kernel_tree_alloc);
}
EXPORT_SYMBOL_GPL(of_fdt_unflatten_tree);

/* Everything below here references initial_boot_params directly. */
int __initdata dt_root_addr_cells;
int __initdata dt_root_size_cells;

/* IAMROOT-12A:
 * ------------
 * dtb 또는 ATAG 시작 위치에 대한 가상 주소가 담김.
 */
void *initial_boot_params;

#ifdef CONFIG_OF_EARLY_FLATTREE

static u32 of_fdt_crc32;

/**
 * res_mem_reserve_reg() - reserve all memory described in 'reg' property
 */
static int __init __reserved_mem_reserve_reg(unsigned long node,
					     const char *uname)
{

/* IAMROOT-12AB:
 * -------------
 * dt_root_addr_cells, dt_root_size_cells:
 *	루트노드에 #addr-cells와 #size-cells 값이 담긴다.
 */
	int t_len = (dt_root_addr_cells + dt_root_size_cells) * sizeof(__be32);
	phys_addr_t base, size;
	int len;
	const __be32 *prop;
	int nomap, first = 1;

/* IAMROOT-12AB:
 * -------------
 * reg 속성이 있는 경우 시작주소와 사이즈가 주어진다.
 * reg 속성이 없는 경우 size 속성에서 사이즈 정보만을 가져온다.
 */
	prop = of_get_flat_dt_prop(node, "reg", &len);
	if (!prop)
		return -ENOENT;

	if (len && len % t_len != 0) {
		pr_err("Reserved memory: invalid reg property in '%s', skipping node.\n",
		       uname);
		return -EINVAL;
	}

/* IAMROOT-12AB:
 * -------------
 * "no-map" 속성을 만나는 경우 해당 영역을 remove한다.
 * 그렇지 않은 경우 해당 영역을 reserve memblock에 추가한다.
 */
	nomap = of_get_flat_dt_prop(node, "no-map", NULL) != NULL;

	while (len >= t_len) {
		base = dt_mem_next_cell(dt_root_addr_cells, &prop);
		size = dt_mem_next_cell(dt_root_size_cells, &prop);

		if (size &&
		    early_init_dt_reserve_memory_arch(base, size, nomap) == 0)
			pr_debug("Reserved memory: reserved region for node '%s': base %pa, size %ld MiB\n",
				uname, &base, (unsigned long)size / SZ_1M);
		else
			pr_info("Reserved memory: failed to reserve memory for node '%s': base %pa, size %ld MiB\n",
				uname, &base, (unsigned long)size / SZ_1M);

		len -= t_len;

/* IAMROOT-12AB:
 * -------------
 * 전역 reserved_mem[] 배열에 해당 노드당 한 번만 저장시킨다.
 * reg의 값이 배열인 경우 첫 항목만 reserved_mem[]에 추가한다.
 */
		if (first) {
			fdt_reserved_mem_save_node(node, uname, base, size);
			first = 0;
		}
	}
	return 0;
}

/**
 * __reserved_mem_check_root() - check if #size-cells, #address-cells provided
 * in /reserved-memory matches the values supported by the current implementation,
 * also check if ranges property has been provided
 */
static int __init __reserved_mem_check_root(unsigned long node)
{
	const __be32 *prop;


/* IAMROOT-12AB:
 * -------------
 * 루트노드에서 명시한 #size-cells와 reserved-memory에서 명시한 값이 같아야 한다.
 */
	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	if (!prop || be32_to_cpup(prop) != dt_root_size_cells)
		return -EINVAL;

/* IAMROOT-12AB:
 * -------------
 * 루트노드에서 명시한 #address-cells와 reserved-memory에서 명시한 값이 같아야 한다.
 */
	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	if (!prop || be32_to_cpup(prop) != dt_root_addr_cells)
		return -EINVAL;


/* IAMROOT-12AB:
 * -------------
 * reserved-memory 노드에 ranges라는 속성이 있어야 한다.
 */
	prop = of_get_flat_dt_prop(node, "ranges", NULL);
	if (!prop)
		return -EINVAL;
	return 0;
}

/**
 * fdt_scan_reserved_mem() - scan a single FDT node for reserved memory
 */
static int __init __fdt_scan_reserved_mem(unsigned long node, const char *uname,
					  int depth, void *data)
{
	static int found;
	const char *status;
	int err;


	if (!found && depth == 1 && strcmp(uname, "reserved-memory") == 0) {
		if (__reserved_mem_check_root(node) != 0) {
			pr_err("Reserved memory: unsupported node format, ignoring\n");
			/* break scan */
			return 1;
		}
/* IAMROOT-12AB:
 * -------------
 * depth-1, reserved-memory 노드이면서 #size-cells와 #address-cells가 루트노드의
 * 것과 동일하고 ranges라는 속성을 발견하는 경우에만 성공
 */
		found = 1;
		/* scan next node */
		return 0;
	} else if (!found) {
		/* scan next node */
		return 0;
	} else if (found && depth < 2) {
		/* scanning of /reserved-memory has been finished */

/* IAMROOT-12AB:
 * -------------
 * found된 상태에서 depth가 1인 경우가 되는 case는 모든 reserved-memory의 child
 * 노드 수행이 완료되었다는 의미이다. (return 1을 하는 경우는 항상 노드 scan이 종료됨)
 */
		return 1;
	}


/* IAMROOT-12AB:
 * -------------
 * found된 상태에서 depth가 2 이상인 노드인 경우 아래 루틴을 수행한다.
 * status 속성이 ok인 경우에만 reg 속성을 읽어와서 reserved_mem[]에
 * 첫 항목만 추가 한다. (reg가 배열인 경우 첫 엔트리만 등록)
 */
	status = of_get_flat_dt_prop(node, "status", NULL);
	if (status && strcmp(status, "okay") != 0 && strcmp(status, "ok") != 0)
		return 0;


/* IAMROOT-12AB:
 * -------------
 * 두 가지 속성에서 reserve 한다.
 *    - reg(시작 주소와 사이즈가 지정된) 속성은 함수 내부에서 alloc
 *    - size 속성은 함수 외부에서 alloc
 *       .전체 범위 
 *       .alloc-ranges 속성 사용
 */

	err = __reserved_mem_reserve_reg(node, uname);

/* IAMROOT-12AB:
 * -------------
 * reg 속성이 없는 대신 size 속성이 있는 경우 reserved_mem[]에 zero 영역 추가 
 */
	if (err == -ENOENT && of_get_flat_dt_prop(node, "size", NULL))
		fdt_reserved_mem_save_node(node, uname, 0, 0);

	/* scan next node */
	return 0;
}

/**
 * early_init_fdt_scan_reserved_mem() - create reserved memory regions
 *
 * This function grabs memory from early allocator for device exclusive use
 * defined in device tree structures. It should be called by arch specific code
 * once the early allocator (i.e. memblock) has been fully activated.
 */
void __init early_init_fdt_scan_reserved_mem(void)
{
	int n;
	u64 base, size;

	if (!initial_boot_params)
		return;

	/* Reserve the dtb region */

/* IAMROOT-12AB:
 * -------------
 * 1) DTB영역 자체를 reserve memblock에 추가한다.
 */
	early_init_dt_reserve_memory_arch(__pa(initial_boot_params),
					  fdt_totalsize(initial_boot_params),
					  0);

/* IAMROOT-12AB:
 * -------------
 * 2) DTB의 memory reservation block에서 읽어들인 주소와 사이즈로 reserve 한다.
 * 예: arch/arm/boot/dts/axm516-amarillo.dts 
 *     /memreserve/ 0x00000000 0x00001000
 *
 * memory reservation block은 16바이트(주소+사이즈) array로 구성되는데
 * 항상 마지막 16바이트는 0으로 되어있다.
 * memory reservation이 구현되어 있지 않은 DTB는 16바이트의 0으로 구성된다.
 */
	/* Process header /memreserve/ fields */
	for (n = 0; ; n++) {
		fdt_get_mem_rsv(initial_boot_params, n, &base, &size);
		if (!size)
			break;
		early_init_dt_reserve_memory_arch(base, size, 0);
	}


/* IAMROOT-12AB:
 * -------------
 * 3) reserved-memory 노드에 등록된 값을 reserve memblock에 추가한다.
 */
	of_scan_flat_dt(__fdt_scan_reserved_mem, NULL);
	fdt_init_reserved_mem();
}

/**
 * of_scan_flat_dt - scan flattened tree blob and call callback on each.
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree, it is
 * used to extract the memory information at boot before we can
 * unflatten the tree
 */
int __init of_scan_flat_dt(int (*it)(unsigned long node,
				     const char *uname, int depth,
				     void *data),
			   void *data)
{
	const void *blob = initial_boot_params;
	const char *pathp;
	int offset, rc = 0, depth = -1;

/* IAMROOT-12A:
 * ------------
 * dtb 처음 위치의 노드부터 하나씩 노드 시작 위치를 읽어온다.
 * depth: root 노드는 0
 *        /chosen 노드는 1
 */
        for (offset = fdt_next_node(blob, -1, &depth);
             offset >= 0 && depth >= 0 && !rc;
             offset = fdt_next_node(blob, offset, &depth)) {

/* IAMROOT-12A:
 * ------------
 * pathp: 노드 명
 */
		pathp = fdt_get_name(blob, offset, NULL);
		if (*pathp == '/')
			pathp = kbasename(pathp);

/* IAMROOT-12A:
 * ------------
 *  early_init_dt_scan_nodes()에서 전달하는 함수 들
 * 	early_init_dt_scan_chosen()
 *	early_init_dt_scan_root()
 *	early_init_dt_scan_memory() 
 */
		rc = it(offset, pathp, depth, data);
	}
	return rc;
}

/**
 * of_get_flat_dt_root - find the root node in the flat blob
 */
unsigned long __init of_get_flat_dt_root(void)
{
	return 0;
}

/**
 * of_get_flat_dt_size - Return the total size of the FDT
 */
int __init of_get_flat_dt_size(void)
{
	return fdt_totalsize(initial_boot_params);
}

/**
 * of_get_flat_dt_prop - Given a node in the flat blob, return the property ptr
 *
 * This function can be used within scan_flattened_dt callback to get
 * access to properties
 */
const void *__init of_get_flat_dt_prop(unsigned long node, const char *name,
				       int *size)
{
	return fdt_getprop(initial_boot_params, node, name, size);
}

/**
 * of_flat_dt_is_compatible - Return true if given node has compat in compatible list
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 */
int __init of_flat_dt_is_compatible(unsigned long node, const char *compat)
{
	return of_fdt_is_compatible(initial_boot_params, node, compat);
}

/**
 * of_flat_dt_match - Return true if node matches a list of compatible values
 */
int __init of_flat_dt_match(unsigned long node, const char *const *compat)
{
	return of_fdt_match(initial_boot_params, node, compat);
}

struct fdt_scan_status {
	const char *name;
	int namelen;
	int depth;
	int found;
	int (*iterator)(unsigned long node, const char *uname, int depth, void *data);
	void *data;
};

const char * __init of_flat_dt_get_machine_name(void)
{
	const char *name;
	unsigned long dt_root = of_get_flat_dt_root();

	name = of_get_flat_dt_prop(dt_root, "model", NULL);
	if (!name)
		name = of_get_flat_dt_prop(dt_root, "compatible", NULL);
	return name;
}

/**
 * of_flat_dt_match_machine - Iterate match tables to find matching machine.
 *
 * @default_match: A machine specific ptr to return in case of no match.
 * @get_next_compat: callback function to return next compatible match table.
 *
 * Iterate through machine match tables to find the best match for the machine
 * compatible string in the FDT.
 */
const void * __init of_flat_dt_match_machine(const void *default_match,
		const void * (*get_next_compat)(const char * const**))
{
	const void *data = NULL;
	const void *best_data = default_match;
	const char *const *compat;
	unsigned long dt_root;
	unsigned int best_score = ~1, score = 0;

	dt_root = of_get_flat_dt_root();
	while ((data = get_next_compat(&compat))) {
		score = of_flat_dt_match(dt_root, compat);
		if (score > 0 && score < best_score) {
			best_data = data;
			best_score = score;
		}
	}
	if (!best_data) {
		const char *prop;
		int size;

		pr_err("\n unrecognized device tree list:\n[ ");

		prop = of_get_flat_dt_prop(dt_root, "compatible", &size);
		if (prop) {
			while (size > 0) {
				printk("'%s' ", prop);
				size -= strlen(prop) + 1;
				prop += strlen(prop) + 1;
			}
		}
		printk("]\n\n");
		return NULL;
	}

	pr_info("Machine model: %s\n", of_flat_dt_get_machine_name());

	return best_data;
}

#ifdef CONFIG_BLK_DEV_INITRD
/**
 * early_init_dt_check_for_initrd - Decode initrd location from flat tree
 * @node: reference to node containing initrd location ('chosen')
 */
static void __init early_init_dt_check_for_initrd(unsigned long node)
{
	u64 start, end;
	int len;
	const __be32 *prop;


/* IAMROOT-12AB:
 * -------------
 * 전역 initrd_start, initrd_end에 /chosen 노드의 initrd와 관련된 속성 값을 저장한다.
 */
	pr_debug("Looking for initrd properties... ");

	prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
	if (!prop)
		return;
	start = of_read_number(prop, len/4);

	prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
	if (!prop)
		return;
	end = of_read_number(prop, len/4);

	initrd_start = (unsigned long)__va(start);
	initrd_end = (unsigned long)__va(end);
	initrd_below_start_ok = 1;

	pr_debug("initrd_start=0x%llx  initrd_end=0x%llx\n",
		 (unsigned long long)start, (unsigned long long)end);
}
#else
static inline void early_init_dt_check_for_initrd(unsigned long node)
{
}
#endif /* CONFIG_BLK_DEV_INITRD */

#ifdef CONFIG_SERIAL_EARLYCON

/* IAMROOT-12AB:
 * -------------
 * OF_EARLYCON_DECLARE()로 만들어진 of_device_id 구조체가 있는 테이블
 */
extern struct of_device_id __earlycon_of_table[];

static int __init early_init_dt_scan_chosen_serial(void)
{
	int offset;
	const char *p;
	int l;
	const struct of_device_id *match = __earlycon_of_table;
	const void *fdt = initial_boot_params;

/* IAMROOT-12AB:
 * -------------
 * "/chosen" 노드를 찾아온다.
 */
	offset = fdt_path_offset(fdt, "/chosen");
	if (offset < 0)
		offset = fdt_path_offset(fdt, "/chosen@0");
	if (offset < 0)
		return -ENOENT;

/* IAMROOT-12AB:
 * -------------
 * "stdout-path" 속성 값을 알아온다.
 */
	p = fdt_getprop(fdt, offset, "stdout-path", &l);
	if (!p)
		p = fdt_getprop(fdt, offset, "linux,stdout-path", &l);
	if (!p || !l)
		return -ENOENT;

/* IAMROOT-12AB:
 * -------------
 * "stdout-path" 속성이 가리키는 노드를 찾아온다.
 */
	/* Get the node specified by stdout-path */
	offset = fdt_path_offset(fdt, p);
	if (offset < 0)
		return -ENODEV;

/* IAMROOT-12AB:
 * -------------
 * 빈 구조체인 __earlycon_of_table_sentinel 구조체가 
 * __earlycon_of_table_end 위치에 들어간다.
 */
	while (match->compatible[0]) {
		unsigned long addr;

/* IAMROOT-12AB:
 * -------------
 * compatible 속성 값에서 문자열 비교: 
 *	0=match, 1=non-match, 길이=”compatible” 속성이 발견되지 않는 경우
 */
		if (fdt_node_check_compatible(fdt, offset, match->compatible)) {
			match++;
			continue;
		}

/* IAMROOT-12AB:
 * -------------
 * 매치된 경우 주소를 알아온다.
 */
		addr = fdt_translate_address(fdt, offset);
		if (!addr)
			return -ENXIO;

/* IAMROOT-12AB:
 * -------------
 * match->data에 있는 함수를 실행한다.
 *	- pl011_early_console_setup()
 */
		of_setup_earlycon(addr, match->data);
		return 0;
	}
	return -ENODEV;
}


/* IAMROOT-12AB:
 * -------------
 * rpi2: earlycon으로 등록되는 함수가 여러 가지 있다.
 *	- setup_of_earlycon()
 *	- pl011_early_console_setup()
 *	- uart_setup_earlycon()
 *	- uart8250_setup_earlycon()
 */
static int __init setup_of_earlycon(char *buf)
{
	if (buf)
		return 0;

	return early_init_dt_scan_chosen_serial();
}
early_param("earlycon", setup_of_earlycon);
#endif

/**
 * early_init_dt_scan_root - fetch the top level address and size cells
 */
int __init early_init_dt_scan_root(unsigned long node, const char *uname,
				   int depth, void *data)
{
	const __be32 *prop;

	if (depth != 0)
		return 0;

	dt_root_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	dt_root_addr_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;

/* IAMROOT-12A:
 * ------------
 * depth=0인 루트노드에서 #size-cells 속성을 찾아 전역 변수 
 *		          dt_root_size_cells에 저장한다.
 *	"	  "       #address-cells 속성을 찾아 전역 변수
 *		          dt_root_addr_cells에 저장한다.
 *
 * be32_to_cpup():
 *	커널이 리틀엔디안 아키텍처에서 동작하는 경우 변환하여 읽어온다.
 *	--> __swab32p() 함수 사용
 *	빅엔디안 아키텍처에서는 변환하지 않는다.
 */
	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	if (prop)
		dt_root_size_cells = be32_to_cpup(prop);
	pr_debug("dt_root_size_cells = %x\n", dt_root_size_cells);

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	if (prop)
		dt_root_addr_cells = be32_to_cpup(prop);
	pr_debug("dt_root_addr_cells = %x\n", dt_root_addr_cells);

	/* break now */
	return 1;
}

u64 __init dt_mem_next_cell(int s, const __be32 **cellp)
{
	const __be32 *p = *cellp;

	*cellp = p + s;
	return of_read_number(p, s);
}

/**
 * early_init_dt_scan_memory - Look for an parse memory nodes
 */
int __init early_init_dt_scan_memory(unsigned long node, const char *uname,
				     int depth, void *data)
{
/* IAMROOT-12A:
 * ------------
 * device_type 속성 값을 읽어온다.
 */
	const char *type = of_get_flat_dt_prop(node, "device_type", NULL);
	const __be32 *reg, *endp;
	int l;

/* IAMROOT-12A:
 * ------------
 * device_type 속성 값이 memory가 아닌 경우 함수를 종료함.
 * (예외: PPC32이면서 depth=1이고 노드명이 "memory@0" 인경우)
 */
	/* We are scanning "memory" nodes only */
	if (type == NULL) {
		/*
		 * The longtrail doesn't have a device_type on the
		 * /memory node, so look for the node called /memory@0.
		 */
		if (!IS_ENABLED(CONFIG_PPC32) || depth != 1 || strcmp(uname, "memory@0") != 0)
			return 0;
	} else if (strcmp(type, "memory") != 0)
		return 0;

/* IAMROOT-12A:
 * ------------
 * linux,usable-memory 또는 reg 속성을 읽어오는데 없으면 리턴한다.
 */
	reg = of_get_flat_dt_prop(node, "linux,usable-memory", &l);
	if (reg == NULL)
		reg = of_get_flat_dt_prop(node, "reg", &l);
	if (reg == NULL)
		return 0;

/* IAMROOT-12A:
 * ------------
 * 1) dtb에서 reg와 #address-cells 및 #size-cells 상관 관계
 *    - reg는 메모리 시작 주소와 사이즈를 지정
 *    - #address-cells = <n> 	메모리의 시작 주소 표현에 사용되는 셀 수
 *    - #size-cells = <n> 	메모리의 사이즈 표현에 사용되는 셀 수
 *    - 셀은 4바이트(32bit) 부호없는 정수
 *    - #address-cells 및 #size-cells는 memory노드의 부모 노드에서 지정
 *    - 64bit 값을 표현하려면 2개의 cells을 사용한다.
 *   
 * 2) memory 노드에서의 reg
 *    - reg <시작주소, 사이즈> [, <시작주소, 사이즈>] [, ...];
 *    - NUMA 시스템 또는 split RAM 시스템에서 메모리 정보는 2개 이상 존재
 *    
 * 3) 사용 예)
 *    - 32bit: 시작주소 0x0000_0000, 사이즈 1GByte
 *      	#address-cells = <1>;
 *      	#size-cells = <1>;
 *		memory {
 *			device_type = "memory";
 * 			reg = <0x0 0x40000000>;
 *		} 
 *
 *    - 64bit: 시작주소 0x0000_0000_4000_0000, 사이즈 2GByte
 *      	#address-cells = <2>;
 *      	#size-cells = <2>;
 * 		memory@40000000 {
 *			device_type = "memory";
 *			reg = <0x0 0x40000000 0x0 0x80000000>;
 *		};
 *
 *    - 64bit: 첫 번째 RAM - 시작주소 0x0000_0000_8000_0000, 사이즈 2G
 *             두 번째 RAM - 시작주소 0x0000_0008_8000_0000, 사이즈 2G
 *      	#address-cells = <2>;
 *      	#size-cells = <2>;
 *   		memory@80000000 {
 *			device_type = "memory";
 *			reg = <0x00000000 0x80000000 0 0x80000000>,
 *		      	      <0x00000008 0x80000000 0 0x80000000>;
 *		};
 *
 * l은 reg에서 사용된 바이트 수
 */
	endp = reg + (l / sizeof(__be32));

	pr_debug("memory scan node %s, reg size %d, data: %x %x %x %x,\n",
	    uname, l, reg[0], reg[1], reg[2], reg[3]);

/* IAMROOT-12A:
 * ------------
 * 추가할 메모리 수만큼 루프로 반복
 */
	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		u64 base, size;

		base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		size = dt_mem_next_cell(dt_root_size_cells, &reg);

		if (size == 0)
			continue;
		pr_debug(" - %llx ,  %llx\n", (unsigned long long)base,
		    (unsigned long long)size);

/* IAMROOT-12A:
 * ------------
 * 추가할 메모리 수만큼 루프로 반복
 *
 * early_init_dt_add_memory_arch()
 * 	early 메모리 관리자를 사용해 메모리 영역을 등록한다.
 *
 *      현재도 early boot process가 진행중에 있고, 또한
 * 	메모리 관리자(buddy allocator)가 아직 활성화된 단계가 아니므로
 *      그 때까지는 임시로 사용할 early allocator가 필요하고 그 곳에
 *      메모리 영역을 추가하여 관리해야 한다. early allocator가 
 *      아키텍처별로 또한 단계별로 약간씩 다르지만 현재 ARM은 
 *	리눅스 추세대로 이 곳에서 memblock을 사용한다. 
 */
		early_init_dt_add_memory_arch(base, size);
	}

	return 0;
}

int __init early_init_dt_scan_chosen(unsigned long node, const char *uname,
				     int depth, void *data)
{
	int l;
	const char *p;

	pr_debug("search \"chosen\", depth: %d, uname: %s\n", depth, uname);

	if (depth != 1 || !data ||
	    (strcmp(uname, "chosen") != 0 && strcmp(uname, "chosen@0") != 0))
		return 0;

/* IAMROOT-12A:
 * ------------
 * 노드가 depth=1인 노드명 chosen만 아래 루틴에 진입한다. 
 * /chosen 노드의 initrd와 관련된 속성 값을 전역 변수에 저장한다.
 */
	early_init_dt_check_for_initrd(node);

/* IAMROOT-12A:
 * ------------
 * 현재 노드에서 bootargs 속성을 찾아온다.
 * 속성을 찾은 경우 boot_command_line에 데이터를 저장한다.
 */
	/* Retrieve command line */
	p = of_get_flat_dt_prop(node, "bootargs", &l);
	if (p != NULL && l > 0)
		strlcpy(data, p, min((int)l, COMMAND_LINE_SIZE));

	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 */

/* IAMROOT-12AB:
 * -------------
 * - CONFIG_CMDLINE_FORCE 옵션에 따라 
 *   .사용하는 경우 DTB의 bootargs를 무시하고 커널 파라메터를 사용.
 *   .사용하지 않는 경우 DTB의 bootargs가 없는 경우네만 커널 파라메터를 사용
 */
#ifdef CONFIG_CMDLINE
#ifndef CONFIG_CMDLINE_FORCE
	if (!((char *)data)[0])
#endif
		strlcpy(data, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#endif /* CONFIG_CMDLINE */

	pr_debug("Command line is: %s\n", (char*)data);

	/* break now */
	return 1;
}

#ifdef CONFIG_HAVE_MEMBLOCK
#define MAX_PHYS_ADDR	((phys_addr_t)~0)

void __init __weak early_init_dt_add_memory_arch(u64 base, u64 size)
{

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: phys_offset = 0x0000_0000
 */
	const u64 phys_offset = __pa(PAGE_OFFSET);

/* IAMROOT-12A:
 * ------------
 * base 주소가 페이지 align이 되어있지 않으면 
 *	또한 size가 1페이지도 안되는 경우 경고 문구 출력 후 빠져나간다.
 *	그렇지 않은 경우 size와 base 주소를 align한 후 사용한다.
 */
	if (!PAGE_ALIGNED(base)) {
		if (size < PAGE_SIZE - (base & ~PAGE_MASK)) {
			pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
				base, base + size);
			return;
		}
		size -= PAGE_SIZE - (base & ~PAGE_MASK);
		base = PAGE_ALIGN(base);
	}
	size &= PAGE_MASK;

/* IAMROOT-12A:
 * ------------
 * base 주소가 시스템 최대 주소 크기를 넘어가는 경우 
 */
	if (base > MAX_PHYS_ADDR) {
		pr_warning("Ignoring memory block 0x%llx - 0x%llx\n",
				base, base + size);
		return;
	}

/* IAMROOT-12A:
 * ------------
 * 영역이 시스템 최대 주소 크기를 넘어가는 경우 넘어간 영역을 잘라낸다.
 */
	if (base + size - 1 > MAX_PHYS_ADDR) {
		pr_warning("Ignoring memory range 0x%llx - 0x%llx\n",
				((u64)MAX_PHYS_ADDR) + 1, base + size);
		size = MAX_PHYS_ADDR - base + 1;
	}

/* IAMROOT-12A:
 * ------------
 * 영역의 끝 주소가 물리메모리 하단에 있는 경우 리턴한다.
 */
	if (base + size < phys_offset) {
		pr_warning("Ignoring memory block 0x%llx - 0x%llx\n",
			   base, base + size);
		return;
	}

/* IAMROOT-12A:
 * ------------
 * 영역이 물리메모리의 하단에서 겹치는 경우 밑부분을 잘라낸다.
 */
	if (base < phys_offset) {
		pr_warning("Ignoring memory range 0x%llx - 0x%llx\n",
			   base, phys_offset);
		size -= phys_offset - base;
		base = phys_offset;
	}

/* IAMROOT-12A:
 * ------------
 * 최종적으로 memblock 영역에 추가한다. 
 */
	memblock_add(base, size);
}

int __init __weak early_init_dt_reserve_memory_arch(phys_addr_t base,
					phys_addr_t size, bool nomap)
{
	if (nomap)
		return memblock_remove(base, size);
	return memblock_reserve(base, size);
}

/*
 * called from unflatten_device_tree() to bootstrap devicetree itself
 * Architectures can override this definition if memblock isn't used
 */
void * __init __weak early_init_dt_alloc_memory_arch(u64 size, u64 align)
{

/* IAMROOT-12AB:
 * -------------
 * DTB 노드들에 대해 memblock을 사용하여 할당하게 한다. 
 * (__weak 속성을 사용하여 별도의 함수를 제공하지 않는 경우 이 함수가 이용된다)
 */
	return __va(memblock_alloc(size, align));
}
#else
int __init __weak early_init_dt_reserve_memory_arch(phys_addr_t base,
					phys_addr_t size, bool nomap)
{
	pr_err("Reserved memory not supported, ignoring range 0x%pa - 0x%pa%s\n",
		  &base, &size, nomap ? " (nomap)" : "");
	return -ENOSYS;
}
#endif

bool __init early_init_dt_verify(void *params)
{

/* IAMROOT-12A:
 * ------------
 * 가상 주소 값이 null 이면 리턴
 */
	if (!params)
		return false;

/* IAMROOT-12A:
 * ------------
 * 가상주소 값에 DTB 스트럭처가 없으면(dtb magic 넘버가 틀리는 등) 리턴  
 */
	/* check device tree validity */
	if (fdt_check_header(params))
		return false;

/* IAMROOT-12A:
 * ------------
 * - 전역 변수 initial_boot_params에 dtb 가상 주소 값을 저장한다.
 * - 전역 변수 of_fdt_crc32에 dtb의 crc32 값을 저장한다.
 */
	/* Setup flat device-tree pointer */
	initial_boot_params = params;
	of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));
	return true;
}


void __init early_init_dt_scan_nodes(void)
{

/* IAMROOT-12A:
 * ------------
 * 전역 변수 boot_command_line에 /chosen 노드의 bootargs 속성 데이터를 저장한다.
 * 만일 없는 경우 커널이 전달하는 커멘드 라인 값을 받아 올 수도 있다.(커널 설정
 * 값에 따라-> CONFIG_CMDLINE, CONFIG_CMDLINE_FORCE)
 */
	/* Retrieve various information from the /chosen node */
	of_scan_flat_dt(early_init_dt_scan_chosen, boot_command_line);

/* IAMROOT-12A:
 * ------------
 * 
 * 전역 변수 dt_root_size_cells에 루트 노드의 #size-cells 값을 저장한다.
 * 전역 변수 dt_root_addr_cells에 루트 노드의 #address-cells 값을 저장한다.
 */
	/* Initialize {size,address}-cells info */
	of_scan_flat_dt(early_init_dt_scan_root, NULL);

/* IAMROOT-12A:
 * ------------
 * memblock에 memory 노드의 reg 값(base, size)들을 추가한다.
 */
	/* Setup memory, calling early_init_dt_add_memory_arch */
	of_scan_flat_dt(early_init_dt_scan_memory, NULL);
}

bool __init early_init_dt_scan(void *params)
{
	bool status;

	status = early_init_dt_verify(params);
	if (!status)
		return false;

	early_init_dt_scan_nodes();
	return true;
}

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
void __init unflatten_device_tree(void)
{

/* IAMROOT-12AB:
 * -------------
 * initial_boot_params: DTB 시작 주소 
 * 전역 of_root는 루트 노드를 가리킨다.
 */

	__unflatten_device_tree(initial_boot_params, &of_root,
				early_init_dt_alloc_memory_arch);

	/* Get pointer to "/chosen" and "/aliases" nodes for use everywhere */

/* IAMROOT-12AB:
 * -------------
 * 전역 of_aliases, of_chosen, of_stdout 노드를 찾고,
 * 전역 aliases_lookup 리스트에 aliases 노드의 모든 속성값으로 찾은 노드들을 
 *	alias_prop 구조체 + stem[] 형태로 변환하여 추가한다.
 */
	of_alias_scan(early_init_dt_alloc_memory_arch);
}

/**
 * unflatten_and_copy_device_tree - copy and create tree of device_nodes from flat blob
 *
 * Copies and unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used. This should only be used when the FDT memory has not been
 * reserved such is the case when the FDT is built-in to the kernel init
 * section. If the FDT memory is reserved already then unflatten_device_tree
 * should be used instead.
 */
void __init unflatten_and_copy_device_tree(void)
{
	int size;
	void *dt;

	if (!initial_boot_params) {
		pr_warn("No valid device tree found, continuing without\n");
		return;
	}

	size = fdt_totalsize(initial_boot_params);
	dt = early_init_dt_alloc_memory_arch(size,
					     roundup_pow_of_two(FDT_V17_SIZE));

	if (dt) {
		memcpy(dt, initial_boot_params, size);
		initial_boot_params = dt;
	}
	unflatten_device_tree();
}

#ifdef CONFIG_SYSFS
static ssize_t of_fdt_raw_read(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *bin_attr,
			       char *buf, loff_t off, size_t count)
{
	memcpy(buf, initial_boot_params + off, count);
	return count;
}

static int __init of_fdt_raw_init(void)
{
	static struct bin_attribute of_fdt_raw_attr =
		__BIN_ATTR(fdt, S_IRUSR, of_fdt_raw_read, NULL, 0);

	if (!initial_boot_params)
		return 0;

	if (of_fdt_crc32 != crc32_be(~0, initial_boot_params,
				     fdt_totalsize(initial_boot_params))) {
		pr_warn("fdt: not creating '/sys/firmware/fdt': CRC check failed\n");
		return 0;
	}
	of_fdt_raw_attr.size = fdt_totalsize(initial_boot_params);
	return sysfs_create_bin_file(firmware_kobj, &of_fdt_raw_attr);
}
late_initcall(of_fdt_raw_init);
#endif

#endif /* CONFIG_OF_EARLY_FLATTREE */
