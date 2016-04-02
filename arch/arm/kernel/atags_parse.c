/*
 * Tag parsing.
 *
 * Copyright (C) 1995-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * This is the traditional way of passing data to the kernel at boot time.  Rather
 * than passing a fixed inflexible structure to the kernel, we pass a list
 * of variable-sized tags to the kernel.  The first tag must be a ATAG_CORE
 * tag for the list to be recognised (to distinguish the tagged list from
 * a param_struct).  The list is terminated with a zero-length tag (this tag
 * is not parsed in any way).
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/root_dev.h>
#include <linux/screen_info.h>
#include <linux/memblock.h>

#include <asm/setup.h>
#include <asm/system_info.h>
#include <asm/page.h>
#include <asm/mach/arch.h>

#include "atags.h"


/* IAMROOT-12AB:
 * -------------
 * CONFIG_CMDLINE 커널 옵션(menuconfig에서 입력)
 */
static char default_command_line[COMMAND_LINE_SIZE] __initdata = CONFIG_CMDLINE;

#ifndef MEM_SIZE
#define MEM_SIZE	(16*1024*1024)
#endif


/* IAMROOT-12AB:
 * -------------
 * default ATAG 정보
 *	-core=flag=1, 4K page, 0xff ROOT_DEV
 *	-mem32=16M
 */
static struct {
	struct tag_header hdr1;
	struct tag_core   core;
	struct tag_header hdr2;
	struct tag_mem32  mem;
	struct tag_header hdr3;
} default_tags __initdata = {
	{ tag_size(tag_core), ATAG_CORE },
	{ 1, PAGE_SIZE, 0xff },
	{ tag_size(tag_mem32), ATAG_MEM },
	{ MEM_SIZE },
	{ 0, ATAG_NONE }
};


/* IAMROOT-12AB:
 * -------------
 * root_mountflags = flags의 0번 비트가 0인 경우 root_mountflags의 0번 비트를 clear 
 * 하여 root device의 write가 가능하게 한다.
 *	flags=1(read only)
 * ROOT_DEV 전역 변수에 root device 번호를 저장한다.
 */
static int __init parse_tag_core(const struct tag *tag)
{
	if (tag->hdr.size > 2) {
		if ((tag->u.core.flags & 1) == 0)
			root_mountflags &= ~MS_RDONLY;
		ROOT_DEV = old_decode_dev(tag->u.core.rootdev);
	}
	return 0;
}


/* IAMROOT-12AB:
 * -------------
 * __tagtable()을 통해 tagtable 구조체가 태그 섹션에 추가된다.
 */
__tagtable(ATAG_CORE, parse_tag_core);

static int __init parse_tag_mem32(const struct tag *tag)
{
	return arm_add_memory(tag->u.mem.start, tag->u.mem.size);
}

__tagtable(ATAG_MEM, parse_tag_mem32);

#if defined(CONFIG_VGA_CONSOLE) || defined(CONFIG_DUMMY_CONSOLE)
static int __init parse_tag_videotext(const struct tag *tag)
{
	screen_info.orig_x            = tag->u.videotext.x;
	screen_info.orig_y            = tag->u.videotext.y;
	screen_info.orig_video_page   = tag->u.videotext.video_page;
	screen_info.orig_video_mode   = tag->u.videotext.video_mode;
	screen_info.orig_video_cols   = tag->u.videotext.video_cols;
	screen_info.orig_video_ega_bx = tag->u.videotext.video_ega_bx;
	screen_info.orig_video_lines  = tag->u.videotext.video_lines;
	screen_info.orig_video_isVGA  = tag->u.videotext.video_isvga;
	screen_info.orig_video_points = tag->u.videotext.video_points;
	return 0;
}

__tagtable(ATAG_VIDEOTEXT, parse_tag_videotext);
#endif


/* IAMROOT-12AB:
 * -------------
 * 시작 주소, 사이즈(KB 단위), flags(bit0=load, bit1=prompt)
 */
#ifdef CONFIG_BLK_DEV_RAM
static int __init parse_tag_ramdisk(const struct tag *tag)
{
	extern int rd_size, rd_image_start, rd_prompt, rd_doload;

	rd_image_start = tag->u.ramdisk.start;
	rd_doload = (tag->u.ramdisk.flags & 1) == 0;
	rd_prompt = (tag->u.ramdisk.flags & 2) == 0;

	if (tag->u.ramdisk.size)
		rd_size = tag->u.ramdisk.size;

	return 0;
}

__tagtable(ATAG_RAMDISK, parse_tag_ramdisk);
#endif


/* IAMROOT-12AB:
 * -------------
 * 보드의 시리얼넘버를 담는다.
 */
static int __init parse_tag_serialnr(const struct tag *tag)
{
	system_serial_low = tag->u.serialnr.low;
	system_serial_high = tag->u.serialnr.high;
	return 0;
}

__tagtable(ATAG_SERIAL, parse_tag_serialnr);


/* IAMROOT-12AB:
 * -------------
 * 보드 변경 이력 번호(보통 수정할 때마다 버전 증가)
 */
static int __init parse_tag_revision(const struct tag *tag)
{
	system_rev = tag->u.revision.rev;
	return 0;
}

__tagtable(ATAG_REVISION, parse_tag_revision);

static int __init parse_tag_cmdline(const struct tag *tag)
{
#if defined(CONFIG_CMDLINE_EXTEND)

/* IAMROOT-12AB:
 * -------------
 * 부트로더로 부터 받은 cmdline 문자열을 default_command_line에 추가한다.
 */
	strlcat(default_command_line, " ", COMMAND_LINE_SIZE);
	strlcat(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#elif defined(CONFIG_CMDLINE_FORCE)
	pr_warn("Ignoring tag cmdline (using the default kernel command line)\n");
/* IAMROOT-12AB:
 * -------------
 * 부트로더로 부터 받은 cmdline 문자열을 사용하지 않고 default_command_line을 사용
 */
#else

/* IAMROOT-12AB:
 * -------------
 * 부트로더로 부터 받은 cmdline 문자열을 default_command_line에 복사한다.
 */
	strlcpy(default_command_line, tag->u.cmdline.cmdline,
		COMMAND_LINE_SIZE);
#endif
	return 0;
}

__tagtable(ATAG_CMDLINE, parse_tag_cmdline);

/*
 * Scan the tag table for this tag, and call its parse function.
 * The tag table is built by the linker from all the __tagtable
 * declarations.
 */
static int __init parse_tag(const struct tag *tag)
{
	extern struct tagtable __tagtable_begin, __tagtable_end;
	struct tagtable *t;

	for (t = &__tagtable_begin; t < &__tagtable_end; t++)
		if (tag->hdr.tag == t->tag) {
			t->parse(tag);
			break;
		}

	return t < &__tagtable_end;
}

/*
 * Parse all tags in the list, checking both the global and architecture
 * specific tag tables.
 */
static void __init parse_tags(const struct tag *t)
{

/* IAMROOT-12AB:
 * -------------
 * tag 사이즈가 0이 아닌 동안 루프를 돌며 사이즈를 계속 더한다.
 */
	for (; t->hdr.size; t = tag_next(t))
		if (!parse_tag(t))
			pr_warn("Ignoring unrecognised tag 0x%08x\n",
				t->hdr.tag);
}


/* IAMROOT-12AB:
 * -------------
 * ATAG_MEM을 찾아서 ATAG_NONE으로 바꾼다.
 */
static void __init squash_mem_tags(struct tag *tag)
{
	for (; tag->hdr.size; tag = tag_next(tag))
		if (tag->hdr.tag == ATAG_MEM)
			tag->hdr.tag = ATAG_NONE;
}

const struct machine_desc * __init
setup_machine_tags(phys_addr_t __atags_pointer, unsigned int machine_nr)
{
	struct tag *tags = (struct tag *)&default_tags;
	const struct machine_desc *mdesc = NULL, *p;
	char *from = default_command_line;

/* IAMROOT-12AB:
 * -------------
 * default tag에 길이는 16M로 되어 있지만 시작 주소가 0으로 되어 있어서
 * 이를 PHYS_OFFSET로 변경한다.
 */
	default_tags.mem.start = PHYS_OFFSET;

	/*
	 * locate machine in the list of supported machines.
	 */
	for_each_machine_desc(p)
		if (machine_nr == p->nr) {
			pr_info("Machine: %s\n", p->name);
			mdesc = p;
			break;
		}

	if (!mdesc) {
		early_print("\nError: unrecognized/unsupported machine ID"
			    " (r1 = 0x%08x).\n\n", machine_nr);
		dump_machine_table(); /* does not return */
	}


/* IAMROOT-12AB:
 * -------------
 * atag 물리주소를 가상주소 변환
 * 만일 주어진 atag 포인터가 없는 경우 머신에 있는 atag_offset를 알아와서
 * PAGE_OFFSET에 더한다.
 */
	if (__atags_pointer)
		tags = phys_to_virt(__atags_pointer);
	else if (mdesc->atag_offset)
		tags = (void *)(PAGE_OFFSET + mdesc->atag_offset);


/* IAMROOT-12AB:
 * -------------
 * PARAM_STRUCT는 ATAG 나오기 전에 사용했던 구조
 */
#if defined(CONFIG_DEPRECATED_PARAM_STRUCT)
	/*
	 * If we have the old style parameters, convert them to
	 * a tag list.
	 */

/* IAMROOT-12AB:
 * -------------
 * ATAG_CORE가 발견되지 않으면 param 구조라고 판단하여
 * 기존 param 구조를 atag 구조로 바꾼다.
 */
	if (tags->hdr.tag != ATAG_CORE)
		convert_to_tag_list(tags);
#endif
	if (tags->hdr.tag != ATAG_CORE) {
		early_print("Warning: Neither atags nor dtb found\n");
		tags = (struct tag *)&default_tags;
	}


/* IAMROOT-12AB:
 * -------------
 * 머신별로 구성 내용이 바뀔 수 있는 경우 fixup을 수행하여 교정한다.
 * 참고: mach-msm/board-msm7x30.c - msm7x30_fixup() 참고
 */
	if (mdesc->fixup)
		mdesc->fixup(tags, &from);

	if (tags->hdr.tag == ATAG_CORE) {

/* IAMROOT-12AB:
 * -------------
 * memory memblock의 total_size가 존재하는 경우 ATAG_MEM을 찾아 삭제한다.
 * (아마 일부 머신에서 mdesc->fixup()을 통해 메모리가 등록되지 않았을까
 *  판단됨.)
 */
		if (memblock_phys_mem_size())
			squash_mem_tags(tags);

/* IAMROOT-12AB:
 * -------------
 * tags를 백업한다.
 */
		save_atags(tags);

/* IAMROOT-12AB:
 * -------------
 * tag를 해석하여 관련 함수를 호출한다.
 */
		parse_tags(tags);
	}

	/* parse_early_param needs a boot_command_line */

/* IAMROOT-12AB:
 * -------------
 * boot_command_line에 default_command_line을 복사한다.
 */
	strlcpy(boot_command_line, from, COMMAND_LINE_SIZE);

	return mdesc;
}
