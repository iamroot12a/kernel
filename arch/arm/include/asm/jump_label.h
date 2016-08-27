#ifndef _ASM_ARM_JUMP_LABEL_H
#define _ASM_ARM_JUMP_LABEL_H

#ifdef __KERNEL__

#include <linux/types.h>

#define JUMP_LABEL_NOP_SIZE 4

#ifdef CONFIG_THUMB2_KERNEL
#define JUMP_LABEL_NOP	"nop.w"
#else
#define JUMP_LABEL_NOP	"nop"
#endif


/* IAMROOT-12AB:
 * -------------
 * static_key_false()에서 호출됨.
 *
 * __jump_table 섹션에 3개의 워드를 저장한다.
 *  - 1st: static key 조건이 위치한 nop 주소
 *  - 2nd: 조건이 성공했을때 branch할 주소 
 *  - 3rd: static key 구조체 포인터
 *
 * %c -> immediate value 속성을 제거하여 컴파일 타임에 에러를 막는다.
 *       (참고: Using Inline Assembly With gcc -p21~p22)
 */

static __always_inline bool arch_static_branch(struct static_key *key)
{
	asm_volatile_goto("1:\n\t"
		 JUMP_LABEL_NOP "\n\t"
		 ".pushsection __jump_table,  \"aw\"\n\t"
		 ".word 1b, %l[l_yes], %c0\n\t"
		 ".popsection\n\t"
		 : :  "i" (key) :  : l_yes);

	return false;
l_yes:
	return true;
}

#endif /* __KERNEL__ */

typedef u32 jump_label_t;


/* IAMROOT-12AB:
 * -------------
 * static brach에 사용되는 점프 엔트리들
 * (커널 부트업 프로세스 시 key 값으로 sorting)
 *
 * code:    __jump_table 섹션에 저장된 nop 주소(branch 조건 비교 코드)
 * target:  __jump_table 섹션에 저장된 조건이 참이 될 때 jump할 함수 주소 
 * key:     __jump_table 섹션에 저장된 static_key 구조체 포인터
 */
struct jump_entry {
	jump_label_t code;
	jump_label_t target;
	jump_label_t key;
};

#endif
