/*
 *  linux/include/asm/setup.h
 *
 *  Copyright (C) 1997-1999 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Structure passed to kernel to tell it about the
 *  hardware it's running on.  See Documentation/arm/Setup
 *  for more info.
 */
#ifndef __ASMARM_SETUP_H
#define __ASMARM_SETUP_H

#include <uapi/asm/setup.h>


/* IAMROOT-12AB:
 * -------------
 * __tag
 *      tagtable이 위치하는 섹션(.taglist.init)
 * 예) tag=ATAG_CORE, fn=parse_tag_core
 *      __tagtable_parse_tag_core  = { ATAG_CORE, parse_tag_core }
 *
 * __used
 *      호출되지 않는 함수나 객체를 심볼에서 삭제하지 않도록 한다.
 */
#define __tag __used __attribute__((__section__(".taglist.init")))
#define __tagtable(tag, fn) \
static const struct tagtable __tagtable_##fn __tag = { tag, fn }

extern int arm_add_memory(u64 start, u64 size);
extern void early_print(const char *str, ...);
extern void dump_machine_table(void);

#endif
