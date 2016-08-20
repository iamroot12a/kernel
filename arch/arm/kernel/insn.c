#include <linux/bug.h>
#include <linux/kernel.h>
#include <asm/opcodes.h>

static unsigned long
__arm_gen_branch_thumb2(unsigned long pc, unsigned long addr, bool link)
{
	unsigned long s, j1, j2, i1, i2, imm10, imm11;
	unsigned long first, second;
	long offset;

	offset = (long)addr - (long)(pc + 4);
	if (offset < -16777216 || offset > 16777214) {
		WARN_ON_ONCE(1);
		return 0;
	}

	s	= (offset >> 24) & 0x1;
	i1	= (offset >> 23) & 0x1;
	i2	= (offset >> 22) & 0x1;
	imm10	= (offset >> 12) & 0x3ff;
	imm11	= (offset >>  1) & 0x7ff;

	j1 = (!i1) ^ s;
	j2 = (!i2) ^ s;

	first = 0xf000 | (s << 10) | imm10;
	second = 0x9000 | (j1 << 13) | (j2 << 11) | imm11;
	if (link)
		second |= 1 << 14;

	return __opcode_thumb32_compose(first, second);
}

static unsigned long
__arm_gen_branch_arm(unsigned long pc, unsigned long addr, bool link)
{
	unsigned long opcode = 0xea000000;
	long offset;

/* IAMROOT-12AB:
 * -------------
 * bl 또는 b를 구분한다. (link=1이면 bl)
 */
	if (link)
		opcode |= 1 << 24;

/* IAMROOT-12AB:
 * -------------
 * addr(목적지) - pc(내 코드 위치) = offset
 */
	offset = (long)addr - (long)(pc + 8);
	if (unlikely(offset < -33554432 || offset > 33554428)) {
		WARN_ON_ONCE(1);
		return 0;
	}

/* IAMROOT-12AB:
 * -------------
 * +-32M 범위내에서 상대 점프를 하는 코드를 만들어낸다.
 */
	offset = (offset >> 2) & 0x00ffffff;

	return opcode | offset;
}

unsigned long
__arm_gen_branch(unsigned long pc, unsigned long addr, bool link)
{
/* IAMROOT-12AB:
 * -------------
 * arm: +-32M 범위내에서 상대 점프(b or bl)를 하는 코드를 만들어낸다.
 */
	if (IS_ENABLED(CONFIG_THUMB2_KERNEL))
		return __arm_gen_branch_thumb2(pc, addr, link);
	else
		return __arm_gen_branch_arm(pc, addr, link);
}
