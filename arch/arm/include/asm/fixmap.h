#ifndef _ASM_FIXMAP_H
#define _ASM_FIXMAP_H

#define FIXADDR_START		0xffc00000UL
#define FIXADDR_END		0xfff00000UL

/* IAMROOT-12:
 * -------------
 * arm) FIXADDR_TOP=0xffef_f000
 */
#define FIXADDR_TOP		(FIXADDR_END - PAGE_SIZE)

#include <asm/kmap_types.h>

enum fixed_addresses {
	FIX_KMAP_BEGIN,
	FIX_KMAP_END = FIX_KMAP_BEGIN + (KM_TYPE_NR * NR_CPUS) - 1,

	/* Support writing RO kernel text via kprobes, jump labels, etc. */

/* IAMROOT-12AB:
 * -------------
 * FIX_TEXT_POKE0 페이지는 읽기 커널 코드 영역을 patch할 때 사용한다.
 */
	FIX_TEXT_POKE0,
	FIX_TEXT_POKE1,

	__end_of_fixed_addresses
};

void __set_fixmap(enum fixed_addresses idx, phys_addr_t phys, pgprot_t prot);

#include <asm-generic/fixmap.h>

#endif
