#ifndef _LINUX_STDDEF_H
#define _LINUX_STDDEF_H

#include <uapi/linux/stddef.h>


#undef NULL
#define NULL ((void *)0)

enum {
	false	= 0,
	true	= 1
};

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#else

/* IAMROOT-12AB:
 * ------------
 * 해당 스트럭처(TYPE)에서 멤버(MEMBER)의 offset를 구한다.
 *
 * 예) struct abc {
 *          int a;
 *          int b;
 *          int c;
 *     };
 *
 *    4 =  offsetof(struct abc, b);
 */

#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif
#endif
