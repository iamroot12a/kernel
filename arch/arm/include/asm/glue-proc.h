/*
 *  arch/arm/include/asm/glue-proc.h
 *
 *  Copyright (C) 1997-1999 Russell King
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef ASM_GLUE_PROC_H
#define ASM_GLUE_PROC_H

#include <asm/glue.h>

/*
 * Work out if we need multiple CPU support
 */
#undef MULTI_CPU
#undef CPU_NAME

/*
 * CPU_NAME - the prefix for CPU related functions
 */

#ifdef CONFIG_CPU_ARM7TDMI
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm7tdmi
# endif
#endif

#ifdef CONFIG_CPU_ARM720T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm720
# endif
#endif

#ifdef CONFIG_CPU_ARM740T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm740
# endif
#endif

#ifdef CONFIG_CPU_ARM9TDMI
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm9tdmi
# endif
#endif

#ifdef CONFIG_CPU_ARM920T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm920
# endif
#endif

#ifdef CONFIG_CPU_ARM922T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm922
# endif
#endif

#ifdef CONFIG_CPU_FA526
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_fa526
# endif
#endif

#ifdef CONFIG_CPU_ARM925T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm925
# endif
#endif

#ifdef CONFIG_CPU_ARM926T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm926
# endif
#endif

#ifdef CONFIG_CPU_ARM940T
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm940
# endif
#endif

#ifdef CONFIG_CPU_ARM946E
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm946
# endif
#endif

#ifdef CONFIG_CPU_SA110
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_sa110
# endif
#endif

#ifdef CONFIG_CPU_SA1100
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_sa1100
# endif
#endif

#ifdef CONFIG_CPU_ARM1020
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm1020
# endif
#endif

#ifdef CONFIG_CPU_ARM1020E
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm1020e
# endif
#endif

#ifdef CONFIG_CPU_ARM1022
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm1022
# endif
#endif

#ifdef CONFIG_CPU_ARM1026
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_arm1026
# endif
#endif

#ifdef CONFIG_CPU_XSCALE
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_xscale
# endif
#endif

#ifdef CONFIG_CPU_XSC3
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_xsc3
# endif
#endif

#ifdef CONFIG_CPU_MOHAWK
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_mohawk
# endif
#endif

#ifdef CONFIG_CPU_FEROCEON
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_feroceon
# endif
#endif

#if defined(CONFIG_CPU_V6) || defined(CONFIG_CPU_V6K)
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_v6
# endif
#endif

#ifdef CONFIG_CPU_V7M
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_v7m
# endif
#endif

#ifdef CONFIG_CPU_PJ4B
# ifdef CPU_NAME
#  undef  MULTI_CPU
#  define MULTI_CPU
# else
#  define CPU_NAME cpu_pj4b
# endif
#endif

/* IAMROOT-12A:
 * ------------
 * DTB가 나올 때  MULTI_CPU 라는 개념도 생겼는데
 * 커널이 특정 CPU 아키텍처로 고정되지 않을 수 있기 때문에
 * 이를 지원하기 위해 커널은 아키텍처를 모른체로 시작한다.
 *
 * 라즈베리파이2: MULTI_CPU를 사용
 */

#ifdef CONFIG_CPU_V7
/*
 * Cortex-A9 needs a different suspend/resume function, so we need
 * multiple CPU support for ARMv7 anyway.
 */
#  undef  MULTI_CPU
#  define MULTI_CPU
#endif

/* IAMROOT-12A:
 * ------------
 * MULTI_CPU를 지원하지 않는 경우 커널이 고정된 아키텍처를 알기 때문에
 * 해당 cpu의 함수를 미리 지정할 수 있다.
 *
 * cpu_proc_init -> cpu_v6_proc_init (arch/arm/mm/proc-V6.S)
 *
 * 라즈베리파이2: 아래 매크로를 사용하지 않는다.
 */

#ifndef MULTI_CPU
#define cpu_proc_init			__glue(CPU_NAME,_proc_init)
#define cpu_proc_fin			__glue(CPU_NAME,_proc_fin)
#define cpu_reset			__glue(CPU_NAME,_reset)
#define cpu_do_idle			__glue(CPU_NAME,_do_idle)
#define cpu_dcache_clean_area		__glue(CPU_NAME,_dcache_clean_area)
#define cpu_do_switch_mm		__glue(CPU_NAME,_switch_mm)
#define cpu_set_pte_ext			__glue(CPU_NAME,_set_pte_ext)
#define cpu_suspend_size		__glue(CPU_NAME,_suspend_size)
#define cpu_do_suspend			__glue(CPU_NAME,_do_suspend)
#define cpu_do_resume			__glue(CPU_NAME,_do_resume)
#endif

#endif
