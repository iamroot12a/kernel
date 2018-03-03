#ifndef __ASM_IDMAP_H
#define __ASM_IDMAP_H

#include <linux/compiler.h>
#include <asm/pgtable.h>

/* Tag a function as requiring to be executed via an identity mapping. */
#define __idmap __section(.idmap.text) noinline notrace

/* IAMROOT-12:
 * -------------
 * early_initcall()로 등록한 init_static_idmap() 함수를 통해 페이지 테이블이
 * 등록된다.
 */
init_static_idmap()
extern pgd_t *idmap_pgd;

void setup_mm_for_reboot(void);

#endif	/* __ASM_IDMAP_H */
