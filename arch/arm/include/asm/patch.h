#ifndef _ARM_KERNEL_PATCH_H
#define _ARM_KERNEL_PATCH_H

void patch_text(void *addr, unsigned int insn);
void __patch_text_real(void *addr, unsigned int insn, bool remap);

static inline void __patch_text(void *addr, unsigned int insn)
{

/* IAMROOT-12AB:
 * -------------
 * 커널이 read only로 되어 있는 경우 이 함수가 호출되어 수정하고자 하는 페이지를 
 * fixmap의 FIX_TEXT_POKE0 위치에 매핑한 후 수정을 하게 할 수 있다.
 */
	__patch_text_real(addr, insn, true);
}

static inline void __patch_text_early(void *addr, unsigned int insn)
{
/* IAMROOT-12AB:
 * -------------
 * false로 호출하는 경우 remap을 하지 않게 한다.
 */
	__patch_text_real(addr, insn, false);
}

#endif
