/*
 * Copyright 1995, Russell King.
 * Various bits and pieces copyrights include:
 *  Linus Torvalds (test_bit).
 * Big endian support: Copyright 2001, Nicolas Pitre
 *  reworked by rmk.
 *
 * bit 0 is the LSB of an "unsigned long" quantity.
 *
 * Please note that the code in this file should never be included
 * from user space.  Many of these are not implemented in assembler
 * since they would be too costly.  Also, they require privileged
 * instructions (which are not available from user mode) to ensure
 * that they are atomic.
 */

#ifndef __ASM_ARM_BITOPS_H
#define __ASM_ARM_BITOPS_H

#ifdef __KERNEL__

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

#include <linux/compiler.h>
#include <linux/irqflags.h>
#include <asm/barrier.h>

/*
 * These functions are the basis of our bit ops.
 *
 * First, the atomic bitops. These use native endian.
 */

/* IAMROOT-12A:
 * ------------
 * 현재 코어의 소프트웨어 인터럽트를 막고 해당 비트를 enable한 후 
 * 원래 인터럽트 상태로 되돌린다.
 *
 * 현재 코어의 인터럽트를 막게되면 현재 코어 내에서 해당 opearation에
 * 개입하는 것을 막을 수 있다.
 *
 * volatile: 
 *      volatile로 선언된 변수에 대해 컴파일로로 하여금 optimization을 하지 못하게 한다.
 */

static inline void ____atomic_set_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;

/* IAMROOT-12A:
 * ------------
 *     mask: bit=0, mask=1 (0b0000_0001)
 *           bit=1, mask=2 (0b0000_0010)
 *           bit=2, mask=4 (0b0000_0100)
 */
	unsigned long mask = 1UL << (bit & 31);

/* IAMROOT-12A:
 * ------------
 * 이 함수는 arch/arm 전용이므로 항상 ARM 32비트 시스템에서만 호출됨.
 * 32로 나누는 이유는 unsigned long 데이터형이 이 시스템에서 4바이트 이므로
 * 배열 번호를 지정할 수 있게 된다.
 *      p[bit/32]와 동일
 */


/* IAMROOT-12AB:
 * -------------
 * 배열로된 비트맵 형태에 대해 인덱스 주소를 가리키게 한다. 
 */
	p += bit >> 5;

	raw_local_irq_save(flags);
	*p |= mask;
	raw_local_irq_restore(flags);
}

static inline void ____atomic_clear_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;
	unsigned long mask = 1UL << (bit & 31);

	p += bit >> 5;

	raw_local_irq_save(flags);
	*p &= ~mask;
	raw_local_irq_restore(flags);
}

static inline void ____atomic_change_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;
	unsigned long mask = 1UL << (bit & 31);

	p += bit >> 5;

	raw_local_irq_save(flags);
	*p ^= mask;
	raw_local_irq_restore(flags);
}

static inline int
____atomic_test_and_set_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;
	unsigned int res;
	unsigned long mask = 1UL << (bit & 31);

	p += bit >> 5;

	raw_local_irq_save(flags);
	res = *p;
	*p = res | mask;
	raw_local_irq_restore(flags);

	return (res & mask) != 0;
}

static inline int
____atomic_test_and_clear_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;
	unsigned int res;
	unsigned long mask = 1UL << (bit & 31);

	p += bit >> 5;

	raw_local_irq_save(flags);
	res = *p;
	*p = res & ~mask;
	raw_local_irq_restore(flags);

	return (res & mask) != 0;
}

static inline int
____atomic_test_and_change_bit(unsigned int bit, volatile unsigned long *p)
{
	unsigned long flags;
	unsigned int res;
	unsigned long mask = 1UL << (bit & 31);

	p += bit >> 5;

	raw_local_irq_save(flags);
	res = *p;
	*p = res ^ mask;
	raw_local_irq_restore(flags);

	return (res & mask) != 0;
}

#include <asm-generic/bitops/non-atomic.h>

/*
 *  A note about Endian-ness.
 *  -------------------------
 *
 * When the ARM is put into big endian mode via CR15, the processor
 * merely swaps the order of bytes within words, thus:
 *
 *          ------------ physical data bus bits -----------
 *          D31 ... D24  D23 ... D16  D15 ... D8  D7 ... D0
 * little     byte 3       byte 2       byte 1      byte 0
 * big        byte 0       byte 1       byte 2      byte 3
 *
 * This means that reading a 32-bit word at address 0 returns the same
 * value irrespective of the endian mode bit.
 *
 * Peripheral devices should be connected with the data bus reversed in
 * "Big Endian" mode.  ARM Application Note 61 is applicable, and is
 * available from http://www.arm.com/.
 *
 * The following assumes that the data bus connectivity for big endian
 * mode has been followed.
 *
 * Note that bit 0 is defined to be 32-bit word bit 0, not byte 0 bit 0.
 */

/*
 * Native endian assembly bitops.  nr = 0 -> word 0 bit 0.
 */
extern void _set_bit(int nr, volatile unsigned long * p);
extern void _clear_bit(int nr, volatile unsigned long * p);
extern void _change_bit(int nr, volatile unsigned long * p);
extern int _test_and_set_bit(int nr, volatile unsigned long * p);
extern int _test_and_clear_bit(int nr, volatile unsigned long * p);
extern int _test_and_change_bit(int nr, volatile unsigned long * p);

/*
 * Little endian assembly bitops.  nr = 0 -> byte 0 bit 0.
 */
extern int _find_first_zero_bit_le(const void * p, unsigned size);
extern int _find_next_zero_bit_le(const void * p, int size, int offset);
extern int _find_first_bit_le(const unsigned long *p, unsigned size);
extern int _find_next_bit_le(const unsigned long *p, int size, int offset);

/*
 * Big endian assembly bitops.  nr = 0 -> byte 3 bit 0.
 */
extern int _find_first_zero_bit_be(const void * p, unsigned size);
extern int _find_next_zero_bit_be(const void * p, int size, int offset);
extern int _find_first_bit_be(const unsigned long *p, unsigned size);
extern int _find_next_bit_be(const unsigned long *p, int size, int offset);

#ifndef CONFIG_SMP
/*
 * The __* form of bitops are non-atomic and may be reordered.
 */

/* IAMROOT-12A:
 * ------------
 * __builtin_constant_p: 인수가 상수이면 true, 변수이면 false
 *
 * arch/arm/lib/setbit.S에 _set_bit 매크로가 있다.
 * arch/arm/lib/bitops.h에 bitop 매크로가 있다.
 */

#define ATOMIC_BITOP(name,nr,p)			\
	(__builtin_constant_p(nr) ? ____atomic_##name(nr, p) : _##name(nr,p))
#else
#define ATOMIC_BITOP(name,nr,p)		_##name(nr,p)
#endif

/*
 * Native endian atomic definitions.
 */
#define set_bit(nr,p)			ATOMIC_BITOP(set_bit,nr,p)
#define clear_bit(nr,p)			ATOMIC_BITOP(clear_bit,nr,p)
#define change_bit(nr,p)		ATOMIC_BITOP(change_bit,nr,p)
#define test_and_set_bit(nr,p)		ATOMIC_BITOP(test_and_set_bit,nr,p)
#define test_and_clear_bit(nr,p)	ATOMIC_BITOP(test_and_clear_bit,nr,p)
#define test_and_change_bit(nr,p)	ATOMIC_BITOP(test_and_change_bit,nr,p)

#ifndef __ARMEB__
/*
 * These are the little endian, atomic definitions.
 */
#define find_first_zero_bit(p,sz)	_find_first_zero_bit_le(p,sz)
#define find_next_zero_bit(p,sz,off)	_find_next_zero_bit_le(p,sz,off)
#define find_first_bit(p,sz)		_find_first_bit_le(p,sz)
/*
 * IAMROOT-12AB:
 * -------------
 * p: 비트맵 주소
 * sz: 검색할 비트수
 * off: 검색을 시작할 비트번호(0번부터~)
 * ex) 비트 0에서 1이 발견된 경우 (*p=0000_0001, sz=8, off=0) => 0
 *                                (*p=0000_0100, sz=8, off=0) => 2
 *                발견되지 않으면 (*p=0000_0000, sz=8, off=0) => 8
 */
#define find_next_bit(p,sz,off)		_find_next_bit_le(p,sz,off)

#else
/*
 * These are the big endian, atomic definitions.
 */
#define find_first_zero_bit(p,sz)	_find_first_zero_bit_be(p,sz)
#define find_next_zero_bit(p,sz,off)	_find_next_zero_bit_be(p,sz,off)
#define find_first_bit(p,sz)		_find_first_bit_be(p,sz)
#define find_next_bit(p,sz,off)		_find_next_bit_be(p,sz,off)

#endif

#if __LINUX_ARM_ARCH__ < 5

#include <asm-generic/bitops/ffz.h>
#include <asm-generic/bitops/__fls.h>
#include <asm-generic/bitops/__ffs.h>
#include <asm-generic/bitops/fls.h>
#include <asm-generic/bitops/ffs.h>

#else

static inline int constant_fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}

/*
 * On ARMv5 and above those functions can be implemented around the
 * clz instruction for much better code efficiency.  __clz returns
 * the number of leading zeros, zero input will return 32, and
 * 0x80000000 will return 0.
 */

/* IAMROOT-12AB:
 * -------------
 * count leading zero
 * 예) __clz(0x10)  ->  0b 0000_0000 0000_0000 0000_0000 0001_0000 -> 27
 *                      (0이 앞에서부터 27개)
 */
static inline unsigned int __clz(unsigned int x)
{
	unsigned int ret;

	asm("clz\t%0, %1" : "=r" (ret) : "r" (x));

	return ret;
}

/*
 * fls() returns zero if the input is zero, otherwise returns the bit
 * position of the last set bit, where the LSB is 1 and MSB is 32.
 */

/* IAMROOT-12AB:
 * -------------
 * find last set (1~32)
 * 예) fls(0xf0)    ->  0b 0000_0000 0000_0000 0000_0000 1111_0000
 *                                                       ^
 *                                                       8
 */
static inline int fls(int x)
{
	if (__builtin_constant_p(x))
	       return constant_fls(x);

	return 32 - __clz(x);
}

/*
 * __fls() returns the bit position of the last bit set, where the
 * LSB is 0 and MSB is 31.  Zero input is undefined.
 */
/* IAMROOT-12AB:
 * -------------
 * find last set (0~31)
 * 예) fls(0xf0)    ->  0b 0000_0000 0000_0000 0000_0000 1111_0000
 *                                                       ^
 *                                                       7
 */
static inline unsigned long __fls(unsigned long x)
{
	return fls(x) - 1;
}

/*
 * ffs() returns zero if the input was zero, otherwise returns the bit
 * position of the first set bit, where the LSB is 1 and MSB is 32.
 */

/* IAMROOT-12AB:
 * -------------
 * find first set (1~32, 없으면 0)
 * 예) ffs(0xf0)    ->  0b 0000_0000 0000_0000 0000_0000 1111_0000
 *                                                          ^
 *                                                          5
 */
static inline int ffs(int x)
{
	return fls(x & -x);
}

/*
 * __ffs() returns the bit position of the first bit set, where the
 * LSB is 0 and MSB is 31.  Zero input is undefined.
 */

/* IAMROOT-12AB:
 * -------------
 * find first set (0~31)
 * 예) __ffs(0xf0)    ->  0b 0000_0000 0000_0000 0000_0000 1111_0000
 *                                                          ^
 *                                                          4
 */
static inline unsigned long __ffs(unsigned long x)
{
	return ffs(x) - 1;
}

/* IAMROOT-12AB:
 * -------------
 * find first zero (0~31)
 * 예) ffz(0xf0)    ->  0b 0000_0000 0000_0000 0000_0000 1111_0000
 *     __ffs(~0xf0)     0b 1111_1111 1111_1111 1111_1111 0000_1111
 *                                                               ^
 *                                                               0
 */
#define ffz(x) __ffs( ~(x) )

#endif

#include <asm-generic/bitops/fls64.h>

#include <asm-generic/bitops/sched.h>
#include <asm-generic/bitops/hweight.h>
#include <asm-generic/bitops/lock.h>

#ifdef __ARMEB__

static inline int find_first_zero_bit_le(const void *p, unsigned size)
{
	return _find_first_zero_bit_le(p, size);
}
#define find_first_zero_bit_le find_first_zero_bit_le

static inline int find_next_zero_bit_le(const void *p, int size, int offset)
{
	return _find_next_zero_bit_le(p, size, offset);
}
#define find_next_zero_bit_le find_next_zero_bit_le

static inline int find_next_bit_le(const void *p, int size, int offset)
{
	return _find_next_bit_le(p, size, offset);
}
#define find_next_bit_le find_next_bit_le

#endif

#include <asm-generic/bitops/le.h>

/*
 * Ext2 is defined to use little-endian byte ordering.
 */
#include <asm-generic/bitops/ext2-atomic-setbit.h>

#endif /* __KERNEL__ */

#endif /* _ARM_BITOPS_H */
