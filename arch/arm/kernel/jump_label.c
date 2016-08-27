#include <linux/kernel.h>
#include <linux/jump_label.h>
#include <asm/patch.h>
#include <asm/insn.h>

#ifdef HAVE_JUMP_LABEL

static void __arch_jump_label_transform(struct jump_entry *entry,
					enum jump_label_type type,
					bool is_static)
{
	void *addr = (void *)entry->code;
	unsigned int insn;

/* IAMROOT-12AB:
 * -------------
 * branch code를 만들거나 nop(mov r0, r0) code를 만들어온다.
 */
	if (type == JUMP_LABEL_ENABLE)
		insn = arm_gen_branch(entry->code, entry->target);
	else
		insn = arm_gen_nop();

/* IAMROOT-12AB:
 * -------------
 * ARM에서는 is_static=true가 되므로 __patch_text_early() 함수를 호출한다.
 */
	if (is_static)
		__patch_text_early(addr, insn);
	else
		patch_text(addr, insn);
}

void arch_jump_label_transform(struct jump_entry *entry,
			       enum jump_label_type type)
{
	__arch_jump_label_transform(entry, type, false);
}

void arch_jump_label_transform_static(struct jump_entry *entry,
				      enum jump_label_type type)
{
/* IAMROOT-12AB:
 * -------------
 * type:
 *	- JUMP_LABEL_ENABLE(1): jump 코드를 생성 
 *	- JUMP_LABEL_DISABLE(0): nop 코드를 생성
 *
 * is_static(3번째 인수): 
 *	- false: patch_text() 함수를 호출 
 *	- true: __patch_text_early() 함수를 호출
 */
	__arch_jump_label_transform(entry, type, true);
}

#endif
