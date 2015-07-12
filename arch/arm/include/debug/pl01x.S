/*
 * IAMROOT Kernel 12차-A팀 (http://www.iamroot.org)
 * ===================================================
 * 디버거 매크로를 사용하기 위해 정의된 파일(라즈베리파이).
 */

/* arch/arm/include/debug/pl01x.S
 *
 * Debugging macro include header
 *
 *  Copyright (C) 1994-1999 Russell King
 *  Moved from linux/arch/arm/kernel/debug.S by Ben Dooks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/
#include <linux/amba/serial.h>

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이: 
 *   -CONFIG_DEBUG_UART_PHYS:	0x20201000
 *   -CONFIG_DEBUG_UART_VIRT:	0xf0201000
 *   -UART01x_DR:		0x00    데이터를 Rx/Tx용 레지스터 offset
 *   -UART01x_FR:		0x18	플래그 레지스터 offset(Read only)
 *   -UART01x_FR_TXFF:		0x20    TXFF 상태값	
 *   -UART01x_FR_BUSY:		0x008   Busy 상태값
 */
#ifdef CONFIG_DEBUG_UART_PHYS
		.macro	addruart, rp, rv, tmp
		ldr	\rp, =CONFIG_DEBUG_UART_PHYS
		ldr	\rv, =CONFIG_DEBUG_UART_VIRT
		.endm
#endif

		.macro	senduart,rd,rx
		strb	\rd, [\rx, #UART01x_DR]
		.endm

		.macro	waituart,rd,rx
1001:		ldr	\rd, [\rx, #UART01x_FR]
 ARM_BE8(	rev	\rd, \rd )
		tst	\rd, #UART01x_FR_TXFF
		bne	1001b
		.endm

		.macro	busyuart,rd,rx
1001:		ldr	\rd, [\rx, #UART01x_FR]
 ARM_BE8(	rev	\rd, \rd )
		tst	\rd, #UART01x_FR_BUSY
		bne	1001b
		.endm
