#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Function: v7_pabort
 *
 * Params  : r2 = pt_regs
 *	   : r4 = address of aborted instruction
 *	   : r5 = psr for parent context
 *
 * Returns : r4 - r11, r13 preserved
 *
 * Purpose : obtain information about current prefetch abort.
 */

	.align	5
ENTRY(v7_pabort)

/* IAMROOT-12:
 * -------------
 * IFAR(instruction Fault Address Register) 값을 r0에 읽어온다.
 * IFSR(instruction Fault Status Register) 값을 r1에 읽어온다.
 */
	mrc	p15, 0, r0, c6, c0, 2		@ get IFAR
	mrc	p15, 0, r1, c5, c0, 1		@ get IFSR

/* IAMROOT-12:
 * -------------
 * r0: fault 주소
 * r1: fault 상태
 * r2: pt_regs * <- sp
 */
	b	do_PrefetchAbort
ENDPROC(v7_pabort)
