#ifndef __ASM_ARM_CACHETYPE_H
#define __ASM_ARM_CACHETYPE_H

/* IAMROOT-12A:
 * ------------
 * L1 cache type에 대해서
 *
 * bit0~2: d-cache 
 * bit3~5: i-cache 
 *
 * ARMv7에서의 data cache 타입은 무조건 VIPT_ALIASING으로 고정
 * 다만 i-cache는 시스템 레지스터를 읽어서 판단함.
 */
#define CACHEID_VIVT			(1 << 0)
#define CACHEID_VIPT_NONALIASING	(1 << 1)
#define CACHEID_VIPT_ALIASING		(1 << 2)
#define CACHEID_VIPT			(CACHEID_VIPT_ALIASING|CACHEID_VIPT_NONALIASING)
#define CACHEID_ASID_TAGGED		(1 << 3)
#define CACHEID_VIPT_I_ALIASING		(1 << 4)
#define CACHEID_PIPT			(1 << 5)

extern unsigned int cacheid;

#define cache_is_vivt()			cacheid_is(CACHEID_VIVT)
#define cache_is_vipt()			cacheid_is(CACHEID_VIPT)
#define cache_is_vipt_nonaliasing()	cacheid_is(CACHEID_VIPT_NONALIASING)
#define cache_is_vipt_aliasing()	cacheid_is(CACHEID_VIPT_ALIASING)
#define icache_is_vivt_asid_tagged()	cacheid_is(CACHEID_ASID_TAGGED)
#define icache_is_vipt_aliasing()	cacheid_is(CACHEID_VIPT_I_ALIASING)
#define icache_is_pipt()		cacheid_is(CACHEID_PIPT)

/*
 * __LINUX_ARM_ARCH__ is the minimum supported CPU architecture
 * Mask out support which will never be present on newer CPUs.
 * - v6+ is never VIVT
 * - v7+ VIPT never aliases on D-side
 */
#if __LINUX_ARM_ARCH__ >= 7

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: __CACHEID_ARCH_MIN = 0x3a 
 */

#define __CACHEID_ARCH_MIN	(CACHEID_VIPT_NONALIASING |\
				 CACHEID_ASID_TAGGED |\
				 CACHEID_VIPT_I_ALIASING |\
				 CACHEID_PIPT)
#elif __LINUX_ARM_ARCH__ >= 6
#define	__CACHEID_ARCH_MIN	(~CACHEID_VIVT)
#else
#define __CACHEID_ARCH_MIN	(~0)
#endif

/*
 * Mask out support which isn't configured
 */

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: 
 *      __CACHEID_ALWAYS = 0
 *      __CACHEID_NEVER  = (CACHEID_VIVT(1))
 */

#if defined(CONFIG_CPU_CACHE_VIVT) && !defined(CONFIG_CPU_CACHE_VIPT)
#define __CACHEID_ALWAYS	(CACHEID_VIVT)
#define __CACHEID_NEVER		(~CACHEID_VIVT)
#elif !defined(CONFIG_CPU_CACHE_VIVT) && defined(CONFIG_CPU_CACHE_VIPT)
#define __CACHEID_ALWAYS	(0)
#define __CACHEID_NEVER		(CACHEID_VIVT)
#else
#define __CACHEID_ALWAYS	(0)
#define __CACHEID_NEVER		(0)
#endif

static inline unsigned int __attribute__((pure)) cacheid_is(unsigned int mask)
{

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: cacheid_is(CACHEID_PIPT) 로 물어본 경우
 *      return ( (0) & 0x30 ) |
 *             (~1 & 0x3a & 0x30 & 2);
 */
	return (__CACHEID_ALWAYS & mask) |
	       (~__CACHEID_NEVER & __CACHEID_ARCH_MIN & mask & cacheid);
}

#endif
