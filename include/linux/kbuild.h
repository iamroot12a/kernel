#ifndef __LINUX_KBUILD_H
#define __LINUX_KBUILD_H


/* IAMROOT-12:
 * ------------- 
 * asm-offset.c -> asm-offset.S를 만든 후 kbuild에서 아래 파일을 만든다.
 * include/generated/asm-offsets.h 에 DEFINE() 매크로로 만든 상수가 들어간다.
 *
 * 예) DEFINE(PROCESSOR_PABT_FUNC, offsetof(struct processor, _prefetch_abort));
 * #define PROCESSOR_PABT_FUNC 4
 */

#define DEFINE(sym, val) \
        asm volatile("\n->" #sym " %0 " #val : : "i" (val))

#define BLANK() asm volatile("\n->" : : )

#define OFFSET(sym, str, mem) \
	DEFINE(sym, offsetof(struct str, mem))

#define COMMENT(x) \
	asm volatile("\n->#" x)

#endif
