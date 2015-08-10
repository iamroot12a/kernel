/*
 * IAMROOT Kernel 12차-A팀 (http://www.iamroot.org)
 * ===================================================
 * 시작일: 2015.07.11
 * 
 * 주석 단축기 <F9>를 사용하는 방법:
 *   - 홈디렉토리에 위치한 .vimrc 파일에 아래의 내용을 추가
 *     map <F9> <ESC>O<ESC>0i<CR><UP>/-<BS>*<SPACE>IAMROOT-12A:<CR>------------<CR><CR>/<UP><SPACE>
 */

/*
 *  linux/arch/arm/boot/compressed/head.S
 *
 *  Copyright (C) 1996-2002 Russell King
 *  Copyright (C) 2004 Hyok S. Choi (MPU support)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>
	
/* IAMROOT-12A:
 * ------------
 * assembler에게 CPU 아키텍처가 armv7-a라고 인식시킨다. 
 */
.arch	armv7-a
/*
 * Debugging stuff
 *
 * Note that these macros must not contain any code which is not
 * 100% relocatable.  Any attempt to do so will result in a crash.
 * Please select one of the following when turning on debugging.
 */

/* IAMROOT-12A:
 * ------------
 * 임베디드 장치가 완전히 테스트되어 양산으로 들어갈 때엔 커널 사이즈와
 * 속도를 위해 DEBUG 옵션을 disable해야 하는 것을 추천한다. 
 */
#ifdef DEBUG

/* IAMROOT-12A:
 * ------------
 * ARM 하드웨어 디버거로 보통 하드웨어에 있는 JTAG 단자에 연결하여 디버깅을
 * 할 수 있다. 물론 라즈베리파이에도 JTAG 단자가 준비되어 있다.
 */
#if defined(CONFIG_DEBUG_ICEDCC)

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2의 경우 CONFIG_CPU_V7이 true로 정의되어 있다. 
 * 일단 현프로젝트 분석에는 하드웨어 디버거 장비를 사용하지 않으므로
 * 아래 하드웨어 디버거를 이용하는 코드는 분석을 건너뛴다.
 */
#if defined(CONFIG_CPU_V6) || defined(CONFIG_CPU_V6K) || defined(CONFIG_CPU_V7)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c0, c5, 0
		.endm
#elif defined(CONFIG_CPU_XSCALE)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c8, c0, 0
		.endm
#else
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c1, c0, 0
		.endm
#endif

#else

/* IAMROOT-12A:
 * ------------
 * CONFIG_DEBUG_LL_INCLUDE는 Low Level Debugging을 할 수 있는 코드가 담긴 화일을
 * 의미하며, 라즈베리파이의 경우 "debug/pl01x.S" 문자열이 담김.
 *
 * Low Level 디버깅은 대부분 UART 포트를 사용하여 printk등의 본격적인 디버깅 코드를 
 * 사용할 수 없는 초기에 사용된다. 
 * 물론 커널이 아직 초기화 되지 않아 일반적인 커널 함수를 사용할 수 있는 상태가
 * 되면 printk등의 디버깅 코드를 사용할 수 있음.
 *
 * 라즈베리파이의 SoC 내부에 uart 인터페이스가 2개가 있고, 하나는 SoC 내부 통신
 * 용도로 사용되어 외부로 사용할 수 없게 하였고, 나머지 하나만 사용자가 사용할 수
 * 있게 하였다.
 *
 * 라즈베리파이가 사용하는 addruart, senduart, waituwart, busyuart의 매크로는
 * 다음 화일에 정의되어 있음.
 * (arch/arm/include/debug/pl01x.S)
 */
#include CONFIG_DEBUG_LL_INCLUDE

/* IAMROOT-12A:
 * ------------
 * writeb 매크로 정의하여 uart 포트를 통해 바이트를 출력.
 */
		.macro	writeb,	ch, rb
		senduart \ch, \rb
		.endm

#if defined(CONFIG_ARCH_SA1100)
		.macro	loadsp, rb, tmp
		mov	\rb, #0x80000000	@ physical base address
#ifdef CONFIG_DEBUG_LL_SER3
		add	\rb, \rb, #0x00050000	@ Ser3
#else
		add	\rb, \rb, #0x00010000	@ Ser1
#endif
		.endm
#else

/* IAMROOT-12A:
 * ------------
 * loadsp 매크로는 uart 통신을 하기위해 uart 관련 레지스터의 
 * 물리 및 가상 BASE 주소를 알아온다. 
 */
		.macro	loadsp,	rb, tmp
		addruart \rb, \tmp
		.endm
#endif
#endif
#endif

/* IAMROOT-12A:
 * ------------
 * kputc 매크로는 NULL로 끝나는 문자열을 출력한다.
 */
		.macro	kputc,val
		mov	r0, \val
		bl	putc
		.endm

/* IAMROOT-12A:
 * ------------
 * kphex 매크로는 16진수 형태로 요구한 길이 만큼 덤프한다.
 */
		.macro	kphex,val,len
		mov	r0, \val
		mov	r1, #\len
		bl	phex
		.endm

/* IAMROOT-12A:
 * ------------
 * debug_reloc_start 매크로는 각종 정보를 출력한다.
 */
		.macro	debug_reloc_start
#ifdef DEBUG
		kputc	#'\n'
		kphex	r6, 8		/* processor id */
		kputc	#':'
		kphex	r7, 8		/* architecture id */
#ifdef CONFIG_CPU_CP15
		kputc	#':'
		mrc	p15, 0, r0, c1, c0
		kphex	r0, 8		/* control reg */
#endif
		kputc	#'\n'
		kphex	r5, 8		/* decompressed kernel start */
		kputc	#'-'
		kphex	r9, 8		/* decompressed kernel end  */
		kputc	#'>'
		kphex	r4, 8		/* kernel execution address */
		kputc	#'\n'
#endif
		.endm


/* IAMROOT-12A:
 * ------------
 * debug_reloc_end 매크로는 커널의 끝 주소를 출력하고 
 * 커널 시작 부분 256 바이트의 메모리를 덤프한다. 
 */
		.macro	debug_reloc_end
#ifdef DEBUG
		kphex	r5, 8		/* end of kernel */
		kputc	#'\n'
		mov	r0, r4
		bl	memdump		/* dump 256 bytes at start of kernel */
#endif
		.endm

/* IAMROOT-12A:
 * ------------
 * .start 섹션을 선언하고 이 안에 메모리를 할당할 수 있고 실행가능코드라 지정
 */

		.section ".start", #alloc, #execinstr
/*
 * sort out different calling conventions
 */

/* IAMROOT-12A:
 * ------------
 * .align은 디폴트로 해당 아키텍처가 최적화된 바이트 단위로 코드와 데이터를
 *  정렬하게 한다.
 * ARM 32bit CPU들은 디폴트로 모두 4바이트이다. 64비트 CPU들은 8일까? 
 *
 * 코드와 데이터를 4바이트 단위로 정렬하여야 메모리로 부터 데이터를 읽을 때
 * 별도의 추가 코드 없이 또는 instruction clock cycle이 가장 빠른 코드를
 * 사용하게 할 수 있다.
 */
		.align
		.arm				@ Always enter in ARM state

start:

/* IAMROOT-12A:
 * ------------
 * ELF 화일을 start 레이블을 함수로 인식할 수 있도록 한다.
 * 참고: https://sourceware.org/binutils/docs/as/Type.html#Type
 *	 http://bit.ly/1M1pyWY 
 */
		.type	start,#function

/* IAMROOT-12A:
 * ------------
 * Russell King 말로 어느 오래된 부트로더 하나가 0x20번째 주소로 점프를 하는
 * 관계로 커널은 호환성을 갖기위해 아무일도 하지않는 코드를 사용할 수 밖에
 * 없었다고 함.
 *
 * 참고: www.simtec.co.uk/products/SWLINUX/files/booting_article.html
 */
		.rept	7
		mov	r0, r0
		.endr

/* IAMROOT-12A:
 * ------------
 * ARM 매크로와 THUMB 매크로는 CONFIG_THUMB2_KERNEL 옵션에 따라 둘 중
 * 하나로만 사용될 수 있다.
 *
 * ARM코드로 만들어진 부트로더가 동작중인 경우 커널도 ARM코드로 시작하는 것이
 * 올바르기 때문에 커널 빌드를 ARM 코드로 해야 함. 만일 특수하게 만든 
 * 부트로더가 THUMB2코드로 동작하고 커널로 넘어오게 된 경우를 고려하여
 * 이 때는 커널 빌드 시 THUMB2 모드를 사용할 수 있게 해야 함.
 *
 * THUMB 코드는 2바이트이지만 THUMB2 코드는 4바이트임.
 *
 * b 1f -> 이 의미는 branch 1 forward라는 의미로 전진방향으로 1이라는
 * 레이블을 찾아 이동하라는 의미임
 *
 * BSYM 매크로는 THUMB2 코드로 진행되는 경우 레이블 1: + 1을 가리킨다.
 * bx 명령어에서는 레이블 주소의 하위 비트(LSB)가 1이면 THUMB 모드로
 * 전환하고 0이면 ARM 모드로 전환한다.
 */
   ARM(		mov	r0, r0		)
   ARM(		b	1f		)
 THUMB(		adr	r12, BSYM(1f)	)
 THUMB(		bx	r12		)

/* IAMROOT-12A:
 * ------------
 * start에 위치한 시작 코드:
 *   -0x00~0x1f: 그냥 노는(?) 코드 
 *   -0x20: brach code (b 1f)
 *   -0x24: zImage magic number (0x016f2818)
 *   -0x28: zImage 시작 주소    (rp ex: 0x00000000)
 *   -0x2c: zImage 끝 주소      (rp ex: 0x003cca28)
 *   -0x30: 엔디안 식별 코드
 */ 
		.word	_magic_sig	@ Magic numbers to help the loader
		.word	_magic_start	@ absolute load/run zImage address
		.word	_magic_end	@ zImage end address
		.word	0x04030201	@ endianness flag

 THUMB(		.thumb			)
1:

/* IAMROOT-12A:
 * ------------
 * ARMv6 및 ARMv7 아키텍처가 BIG ENDIAN을 지원함.
 * ARM_BE8은 BIG ENDIAN을 지원하는 경우 BIG ENDIAN 모드로 동작하게 함
 * 참고로 라즈베리 파이 2는 ARMv7 이므로 BIG ENDIAN을 지원함
 */
 ARM_BE8(	setend	be )			@ go BE8 if compiled for BE8
		mrs	r9, cpsr

/* IAMROOT-12A:
 * ------------
 * CONFIG_ARM_VIRT_EXT은 가상화 기술로 ARMv7 아키텍처가 지원함.
 */
#ifdef CONFIG_ARM_VIRT_EXT
		bl	__hyp_stub_install	@ get into SVC mode, reversibly
#endif
		mov	r7, r1			@ save architecture ID
		mov	r8, r2			@ save atags pointer

		/*
		 * Booting from Angel - need to enter SVC mode and disable
		 * FIQs/IRQs (numeric definitions from angel arm.h source).
		 * We only do this if we were in user mode on entry.
		 */

/* IAMROOT-12A:
 * ------------
 * cpsr 레지스터를 읽어와서 bit0과 bit1이 모두 0인 경우는 usr 모드
 * tst 명령으로 플래그를 판단하여 usr 모드가 아닌경우 not_angel 레이블로 이동
 * 
 * r0에 0x17을 갖고 소프트웨어 인터럽트 0x123456를 호출하면 엔젤 부트로더에
 * 있는 루틴에서 Svc모드로 바꿀거라 예상됨. THUMB2 코드가 지원되는 경우
 * swi 대신 THUMB2용 소프트웨어 인터럽트를 호출한다.
 */
		mrs	r2, cpsr		@ get current mode
		tst	r2, #3			@ not user?
		bne	not_angel
		mov	r0, #0x17		@ angel_SWIreason_EnterSVC
 ARM(		swi	0x123456	)	@ angel_SWI_ARM
 THUMB(		svc	0xab		)	@ angel_SWI_THUMB

not_angel:

/* IAMROOT-12A:
 * ------------
 * 서비스 모드로 진입하기 위한 매크로 (arch/arm/include/asm/assembler.h)
 */
		safe_svcmode_maskall r0
		msr	spsr_cxsf, r9		@ Save the CPU boot mode in
						@ SPSR		 
		/* 
		 * Note that some cache flushing and other stuff may
		 * be needed here - is there an Angel SWI call for this?
		 */

		/*
		 * some architecture specific code can be inserted
		 * by the linker here, but it should preserve r7, r8, and r9.
		 */

		.text

/* IAMROOT-12A:
 * ------------
 * CONFIG_AUTO_ZRELADDR 옵션이 사용될 경우 코드(head.o~압축된커널)가 
 * 시작할 위치 영역을 자동계산한다.
 *
 * r4 = BASE_ADDR + 0x00008000(라즈베리파이의 #TEXT_OFFSET)
 * Base address의 align은 128M 간격으로 설정됨.
 */
#ifdef CONFIG_AUTO_ZRELADDR
		@ determine final kernel image address
		mov	r4, pc
		and	r4, r4, #0xf8000000
		add	r4, r4, #TEXT_OFFSET
#else
		ldr	r4, =zreladdr
#endif

		/*
		 * Set up a page table only if it won't overwrite ourself.
		 * That means r4 < pc || r4 - 16k page directory > &_end.
		 * Given that r4 > &_end is most unfrequent, we add a rough
		 * additional 1MB of room for a possible appended DTB.
		 */

/* IAMROOT-12A:
 * ------------
 * 목적: 현재 실행되는 zImage가 설치될 커널 즉 TEXT_OFFSET보다 하위에서 실행되는
 * 경우 영역이 중복이 되어 코드 리로케이션이 될 때 캐시의 사용이 문제가 있다고
 * 판단하여 캐시 사용을 유보한다.
 *
 * 아래 두 줄: 사용할 타겟(decompressed)커널 영역과 현재 실행되고 있는 이미지의
 * 실행 위치를 확인하여 실행 위치(pc)가 윗쪽에 위치하면 캐시 사용에 문제가 없어
 * 캐시를 on.
 *
 * 라즈베리파이2 같은 경우 zImage와 decompressed kernel의 주소가 같아(0x8000) 
 * 캐시를 on하여 진행한다.
 */
		mov	r0, pc
		cmp	r0, r4

/* IAMROOT-12A:
 * ------------
 * r0(중복 체크를 위해 계산된 이미지의 끝부분) <= r4(zreladdr)이면 
 * 영역이 겹치지 않는다고 판단하여 캐시를 on.
 */
		ldrcc	r0, LC0+32
		addcc	r0, r0, pc
		cmpcc	r4, r0

/* IAMROOT-12A:
 * ------------
 * 캐시 사용을 유보한 사실을 r4의 LSB 1비트를 사용하여 보관하여 나중에
 * decompress 루틴을 실행하기 전에 유보된 캐시를 on 시킴.
 * (0=cache on status, 1 =cache off status-캐시 설정이 유보된 상태)
 */
		orrcc	r4, r4, #1		@ remember we skipped cache_on
		blcs	cache_on

/* IAMROOT-12A:
 * ------------
 * restart: 이 위치는 압축이 풀려지는 위치와 현재 압축된 커널의 공간이 중복
 * 되는 경우 현재의 코드(압축된커널 + 재배치코드(restart-끝))를 이동시킨 후
 * 다시 restart 이 위치로 점프된다. 
 */

restart:	adr	r0, LC0
		ldmia	r0, {r1, r2, r3, r6, r10, r11, r12}

/* IAMROOT-12A:
 * ------------
 * 스택을 설정한다. sp <- L_user_stack_end(stack + 4K)
 */

		ldr	sp, [r0, #28]

		/*
		 * We might be running at a different address.  We need
		 * to fix up various pointers.
		 */

/* IAMROOT-12A:
 * ------------
 * r0는 현재 실행되는 상태의 LC0의 주소 값이 담기고,
 * r1은 처음 이미지가 만들어질 때 당시의 주소 값(0을 기준으로)이 담겨 있고
 * 이 둘의 차이 값이 r0(delta) 값이 된다.
 * 재배치가 이루어진 후 이곳으로 재진입하게 되면 기존 LC0 주소와 재배치되어 다시
 * 한 번 주소가 변한 LC0 주소간에 차이가 발생하는데 이를 delta라고 하여 r0에
 * 저장하며, 필요한 레지스터들의 주소에 더해 교정을 하는데 사용한다.
 * 처음 진입한 경우 delta 값은 LC0의 주소값 - 빌드시 LC0의 주소값이다.
 * zImage가 재배치된 후 재진입하게 되면 delta 값은 또 달라진다. 
 * 처음 저장된 LC0(r1)주소는 zImage가 빌드될 때 결정되었던 LC0 주소값이다.
 * 재배치가 된 경우 r6와 r10을 교정한다.
 *
 * -r6 = _edata + delta
 * -r10 = _input_data_end-4 + delta
 *
 *  _input_data_end-4 = 압축된 커널(piggy)의 마지막 어드레스를 의미함.
 *                      데이터 내용은 압축된 커널의 길이.
 */
		sub	r0, r0, r1		@ calculate the delta offset
		add	r6, r6, r0		@ _edata
		add	r10, r10, r0		@ inflated kernel size location

		/*
		 * The kernel build system appends the size of the
		 * decompressed kernel at the end of the compressed data
		 * in little-endian form.
		 */

/* IAMROOT-12A:
 * ------------
 * 커널 길이(decompress kernel)
 * 커널의 길이가 기록될 때 리틀엔디안으로 기록이되며 이를 엔디안과 관계 없이 알아온다.
 * (r10(리틀엔디안으로 길이가 저장된 주소) -> r9(커널길이)
 *
 * 예) 길이=0x44332211 (리틀엔디안형식으로 기록된 저장소: 11 22 33 44)
 *		0x00000011 <- 처음 바이트만 r9에 가져옮
 *		0x00000022 <- 두번째 바이트를 lr에 가져옮
 *		0x00002211 <- lr레지스터 내용을 8회 shift후 r9 레지스터와 or 연산
 *		0x00000033 <- 세번째 바이트를 lr에 가져옮
 *		0x00000044 <- 네번째 바이트를 r10에 가져옮
 *		0x00332211 <- lr레지스터 내용을 16회 shift후 r9 레지스터와 or 연산
 *		0x44332211 <- r10레지스터 내용을 24회 shift후 or r9 레지스터와 연산
 *
 * 만일 SoC가 리틀엔디안으로 구동되고 있는 경우 변환전과 변환후가 동일.
 * 만일 SoC가 빅엔디안으로 구동되고 있는 경우 변환전과 변환후가 다름.
 */
		ldrb	r9, [r10, #0]
		ldrb	lr, [r10, #1]
		orr	r9, r9, lr, lsl #8
		ldrb	lr, [r10, #2] 
		ldrb	r10, [r10, #3]
		orr	r9, r9, lr, lsl #16
		orr	r9, r9, r10, lsl #24

/* IAMROOT-12A:
 * ------------
 * CONFIG_ZBOOT_ROM: ROM이나 플래쉬에 있는 vmlinux를 RAM으로복사하지 않고
 * 직접 구동하여 사용하려고 할 때 사용.
 * (라즈베리파이는 각 패키징 업체가 준비해 놓은 설정을 사용하는데 특정
 * 패키징을 사용하지 않고 다운로드한 라즈베리파이 원 소스의 기본 설정으로
 * 빌드하였을 때 이 옵션은 사용하지 않는 것으로 되어 있음.
 * 라즈베리안이라는 기본 패키징도 마찬가지로 이 옵션을 사용하지 않음)
 */

#ifndef CONFIG_ZBOOT_ROM
		/* malloc space is above the relocated stack (64k max) */


/* IAMROOT-12A:
 * ------------
 * RAM에서 구동하는 경우
 * 기존에 설정한 스택 주소를 delta만큼 재조정
 * r10에 64K의 버퍼 마지막을 저장(delta 재조정된 스택 + 64K)
 */

		add	sp, sp, r0
		add	r10, sp, #0x10000
#else


/* IAMROOT-12A:
 * ------------
 * 부트롬 또는 플래쉬에서 vmlinux 코드를 직접 동작시켜 압축을 풀려고 할때는
 * decompress할 곳이 주어져 고정된다. 그럼에도 누군가 이를 RAM에서 구동하게
 * 할 수 있는데 이러한 경우에도 _edata (ELF에서 데이터의 섹션의 끝)을 참고한다.
 * r10은 이미지의 끝을 의미하며 r6(_edata)의 값으로 지정한다.
 */

		/*
		 * With ZBOOT_ROM the bss/stack is non relocatable,
		 * but someone could still run this code from RAM,
		 * in which case our reference is _edata.
		 */
		mov	r10, r6
#endif

		mov	r5, #0			@ init dtb size to 0
#ifdef CONFIG_ARM_APPENDED_DTB
/*
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = final kernel address (possibly with LSB set)
 *   r5  = appended dtb size (still unknown)
 *   r6  = _edata
 *   r7  = architecture ID
 *   r8  = atags/device tree pointer
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 *
 * if there are device trees (dtb) appended to zImage, advance r10 so that the
 * dtb data will get relocated along with the kernel if necessary.
 */

/* IAMROOT-12A:
 * ------------
 * DTB(Device Tree Blob)이 존재하는지 확인한다
 * r6(_edata = _bss_start)가 가리키는 곳에 0xd00dfeed라는 DTB magic number)가 
 * 있는지 확인하여 없으면 dtb_check_done으로 이동
 */

		ldr	lr, [r6, #0]
#ifndef __ARMEB__
		ldr	r1, =0xedfe0dd0		@ sig is 0xd00dfeed big endian
#else
		ldr	r1, =0xd00dfeed
#endif
		cmp	lr, r1
		bne	dtb_check_done		@ not found

#ifdef CONFIG_ARM_ATAG_DTB_COMPAT
		/*
		 * OK... Let's do some funky business here.
		 * If we do have a DTB appended to zImage, and we do have
		 * an ATAG list around, we want the later to be translated
		 * and folded into the former here. No GOT fixup has occurred
		 * yet, but none of the code we're about to call uses any
		 * global variable.
		*/

/* IAMROOT-12A:
 * ------------
 * r5 <- DTB size를 저장 (fdt_header->dt_total_size(fdt_size))
 */

		/* Get the initial DTB size */
		ldr	r5, [r6, #4]
#ifndef __ARMEB__


/* IAMROOT-12A:
 * ------------
 * 시스템이 리틀엔디안으로 동작하는 경우 사이즈(기존 빅엔디안으로 저장된)를
 * 리틀엔디안으로 변환.
 *
 *         r5          A    B    C    D
 *              ror    C    D    A    B
 *              eor   A^C  B^D  C^A  D^B
 *              bic   A^C   0   C^A  D^B
 *              ror    D    A    B    C       
 *              lsr    0   A^C   0   C^A
 *         r5          D    C    B    A
 */
		/* convert to little endian */
		eor	r1, r5, r5, ror #16
		bic	r1, r1, #0x00ff0000
		mov	r5, r5, ror #8
		eor	r5, r5, r1, lsr #8
#endif

/* IAMROOT-12A:
 * ------------
 * DTB 사이즈를 50% 증가. (r5 += shift to right) 증가시킨다.
 */

		/* 50% DTB growth should be good enough */
		add	r5, r5, r5, lsr #1

/* IAMROOT-12A:
 * ------------
 * 사이즈를 64비트 align 하기위해 7바이트를 추가
 * 이렇게 7바이트를 더해놔야 마지막 데이터를 안전하게(8바이트) 꺼낼 수 있다.
 */
		/* preserve 64-bit alignment */
		add	r5, r5, #7
		bic	r5, r5, #7

/* IAMROOT-12A:
 * ------------
 * DTB사이즈가 32KB보다 작으면 32K로 설정, 1M보다 크면 1M로 제한한다.
 */

		/* clamp to 32KB min and 1MB max */
		cmp	r5, #(1 << 15)
		movlo	r5, #(1 << 15)
		cmp	r5, #(1 << 20)
		movhi	r5, #(1 << 20)

/* IAMROOT-12A:
 * ------------
 * 스택을 임시적으로 DTB 데이터 작업 영역 위로 설정한다. 
 */
		/* temporarily relocate the stack past the DTB work space */
		add	sp, sp, r5

/* IAMROOT-12A:
 * ------------
 * stmfd = full descending = decrement before
 * r8=atags/device tree pointer
 * r6=_edata
 * r5=dtb size
 * int atags_to_fdt(void *atag_list, void *fdt, int total_space)
 *
 * TODO: atags_to_fdt 함수는 리로케이션 분석 후 예정.
 *
 */
		stmfd	sp!, {r0-r3, ip, lr}
		mov	r0, r8
		mov	r1, r6
		mov	r2, r5
		bl	atags_to_fdt

		/*
		 * If returned value is 1, there is no ATAG at the location
		 * pointed by r8.  Try the typical 0x100 offset from start
		 * of RAM and hope for the best.
		 */
/* IAMROOT-12A:
 * ------------
 * 만일 설정된 장소에서 ATAG를 발견하지 못하면 1이 리턴되는데, 
 * 이 때 다시 한 번 시작램+0x100에서 ATAG 발견을 시도한다.
 * 물론 리눅스는 ATAG를 시작램(물리메모리)+0x100에 위치하도록
 * 추천함.
 * r4=final kernel address 
 */
		cmp	r0, #1
		sub	r0, r4, #TEXT_OFFSET
		bic	r0, r0, #1
		add	r0, r0, #0x100
		mov	r1, r6
		mov	r2, r5
		bleq	atags_to_fdt

		ldmfd	sp!, {r0-r3, ip, lr}
		sub	sp, sp, r5
#endif

		mov	r8, r6			@ use the appended device tree

/* IAMROOT-12A:
 * ------------
 * decompressed 커널이 사용해야 하는 bss의 사이즈를 r5에 로드
 * (라즈베리파이2:0x000bd75c)
 * r1에 kernel bss size - zImage 대부분(_edata - addr(wont_overwrite))을
 * 구한 후 양수이면 r9에(target kernel size)에 그값을 더한다.
 */

		/*
		 * Make sure that the DTB doesn't end up in the final
		 * kernel's .bss area. To do so, we adjust the decompressed
		 * kernel size to compensate if that .bss size is larger
		 * than the relocated code.
		 */
		ldr	r5, =_kernel_bss_size
		adr	r1, wont_overwrite
		sub	r1, r6, r1
		subs	r1, r5, r1
		addhi	r9, r9, r1

/* IAMROOT-12A:
 * ------------
 * DTB 사이즈를 다시 r5에 로드한다.
 */

		/* Get the current DTB size */
		ldr	r5, [r6, #4]
#ifndef __ARMEB__
		/* convert r5 (dtb size) to little endian */
		eor	r1, r5, r5, ror #16
		bic	r1, r1, #0x00ff0000
		mov	r5, r5, ror #8
		eor	r5, r5, r1, lsr #8
#endif

		/* preserve 64-bit alignment */
		add	r5, r5, #7
		bic	r5, r5, #7

		/* relocate some pointers past the appended dtb */
		add	r6, r6, r5
		add	r10, r10, r5
		add	sp, sp, r5
dtb_check_done:
#endif


/* IAMROOT-12A:
 * ------------
 * 이 루틴은 현재 동작되는 이미지 영역과 final 커널의 경계 영역이 중복되는지 체크
 *
 * final kernel address 밑의 PTE 공간이 zImage의 상부와 겹치는지 확인하려고
 * r10(end of this image + delta)에 16K를 더한다.
 * r4(final kernel address)가 r10(end of this image + delta)보다
 * 크거나 같으면 wont_overwrite(영역이 중복되지 않음)로 이동하라는
 * 
 * final kernel address가 현재 수행되는 이미지보다 충분히 위에 있어 중복되지
 * 않다고 판단하였기에 이 경우 리로케이션이 필요 없다.
 *
 * final kernel은 대부분 SDRAM의 물리 영역 처음 + offset(0x8000 등)에 위치한다.
 *                               TEXT_BASE + TEXT_OFFSEET : 시스템마다 다름.
 * 그렇기에 final kernal 주소보다 더 아래에 zImage가 위치한 경우는 보통 플래쉬나
 * ROM이 SDRAM 보다 물리메모리의 하위에 배치되었고 이주소에서 zImage가
 * 직접 실행된 경우 이 조건을 만족시킨다. 상당 수의 임베디드 장치들은 zImage가
 * SDRAM으로 옮겨지지 않고 이렇게 동작하여 wont_overwrite로 이동된다.
 */

/*
 * Check to see if we will overwrite ourselves.
 *   r4  = final kernel address (possibly with LSB set)
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 * We basically want:
 *   r4 - 16k page directory >= r10 -> OK
 *   r4 + image length <= address of wont_overwrite -> OK
 * Note: the possible LSB in r4 is harmless here.
 */
		add	r10, r10, #16384
		cmp	r4, r10
		bhs	wont_overwrite


/* IAMROOT-12A:
 * ------------
 * r10 = r4(final kernel address) + r9(decompressed 커널의 사이즈)
 * if r10 <= r9(wont_overwrite)인 경우 wont_overwrite로 이동하라는
 * 
 * decompressed될 커널의 끝 주소가 현재 동작중인 코드위치보다 아래에 있어
 * 중복되지 않을 때 wont_overwrite로 이동.
 *
 * 이 케이스는 u-boot 같은 부트로더와 zImage가 SDRAM의 상부에 배치되고
 * final kernael image가 SDRAM의 최하부에 배치되어 실행될 때 이러한 경우를
 * 접하게 되며, SDRAM보다 높은 주소에 배치된 ROM이나 플래쉬에서 zImage가
 * 직접 실행하게 되면 역시 이와 같은 조건으로 wont_overwrite로 이동된다.
 * 역시 상당 수의 임베디드 장치들도 이렇게 재배치 없이 decompressed 루틴으로
 * 진입하여 이용되곤 한다.
 */

		add	r10, r4, r9
		adr	r9, wont_overwrite
		cmp	r10, r9
		bls	wont_overwrite

/*
 * Relocate ourselves past the end of the decompressed kernel.
 *   r6  = _edata
 *   r10 = end of the decompressed kernel
 * Because we always copy ahead, we need to do it from the end and go
 * backward in case the source and destination overlap.
 */
		/*
		 * Bump to the next 256-byte boundary with the size of
		 * the relocation code added. This avoids overwriting
		 * ourself when the offset is small.
		 */

/* IAMROOT-12A:
 * ------------
 * 이 루틴부터 리로케이션 시작
 *
 * restart부터 시작되는 리로케이션 관련 코드는 압축풀릴 커널의 끝을 기준으로
 * 추가 계산된 리로케이션코드 크기에 256바이트 단위로 trim을 한 주소가
 * 리로케이션 코드 영역이다.
 *
 * 왜 빈 공간을 256바이트로 두었을까???
 *
 * 라즈베리파이 예)
 * r10 += 0x900 (0x928 - 0xc0 + 0x100) & 0xffffff00
 * r10 &= 0xffffff00
 */

		add	r10, r10, #((reloc_code_end - restart + 256) & ~255)
		bic	r10, r10, #255

/* IAMROOT-12A:
 * ------------
 * r5에 restart의 위치 0xffffffe0 (뒤 5비트를 잘라냄) <- 0xc0
 */

		/* Get start of code we want to copy and align it down. */
		adr	r5, restart
		bic	r5, r5, #31

/* Relocate the hyp vector base if necessary */
#ifdef CONFIG_ARM_VIRT_EXT
		mrs	r0, spsr
		and	r0, r0, #MODE_MASK
		cmp	r0, #HYP_MODE
		bne	1f

		bl	__hyp_get_vectors
		sub	r0, r0, r5
		add	r0, r0, r10
		bl	__hyp_set_vectors
1:
#endif

/* IAMROOT-12A:
 * ------------
 * r9=사본 재배치 코드 주소의 끝을 계산한다.
 * 0~4번 비트를 clear하는 이유는 코드를 복사할 때 32바이트씩 복사하기 위해
 * 주소의 align을 32로 한다. 그리고 잘리는 바이트가 없도록 그 전에 31바이트를
 * 미리 더해준다.
 */

		sub	r9, r6, r5		@ size to copy
		add	r9, r9, #31		@ rounded up to a multiple
		bic	r9, r9, #31		@ ... of 32 bytes
		add	r6, r9, r5
		add	r9, r9, r10

/* IAMROOT-12A:
 * ------------
 * 복사할 주소의 상위 메모리로부터 하위로 32바이트 단위 Copy (for cache line)
 * r6(원본 재배치 코드 주소의 끝)가 가리키는 주소의 값 바로 아래부터
 * 8개의 레지스터에 로드
 * r9(사본재배치 코드 주소의 끝)이 가리키는 주소 바로 아래부터 8개의
 * 레지스터값을 기록될
 * if r6 > r5(0xc0) 이면 반복
 */

1:		ldmdb	r6!, {r0 - r3, r10 - r12, lr}
		cmp	r6, r5
		stmdb	r9!, {r0 - r3, r10 - r12, lr}
		bhi	1b

/* IAMROOT-12A:
 * ------------
 * r6에는 offset이 담긴다.
 * 이 offset을 이용하여 잠시 뒤 재배치된 restart 루틴으로 점프해야 할
 * 때 사용된다.
 */

		/* Preserve offset to relocated code. */
		sub	r6, r9, r6

#ifndef CONFIG_ZBOOT_ROM
		/* cache_clean_flush may use the stack, so relocate it */

/* IAMROOT-12A:
 * ------------
 * 캐시 클린 플러쉬 루틴을 이용하려면 스택이 필요해서 재배치 코드의 위쪽으로
 * 스택을 설정한다.
 */
		add	sp, sp, r6
#endif

		bl	cache_clean_flush

/* IAMROOT-12A:
 * ------------
 * 리로케이트된 코드가 있는 곳의 restart:로 점프
 */

		adr	r0, BSYM(restart)
		add	r0, r0, r6
		mov	pc, r0

/* IAMROOT-12A:
 * ------------
 * 압축된 커널 이미지를 포함하는 코드가 decompress된 커널과 중복되지
 * 않았다고 판단되면 이 루틴으로 들어오게된다.
 * 이 루틴 이하에서는 본격적으로 압축을 풀고 커널 시작(start_kernel)
 * 으로 가기위한 준비를 한다.
 */

wont_overwrite:
/*
 * If delta is zero, we are running at the address we were linked at.
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = kernel execution address (possibly with LSB set)
 *   r5  = appended dtb size (0 if not present)
 *   r7  = architecture ID
 *   r8  = atags pointer
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 */

/* IAMROOT-12A:
 * ------------
 * if (delta = 0) & (r5(dtb size) = 0)
 * delta가 0이면서 dtb_size도 0이면 BSS 및 GOT 영역들의 위치가 바뀔 필요가
 * 없으므로 리로케이션 로직을 수행하지 않았던 경우이다.
 */
		orrs	r1, r0, r5
		beq	not_relocated

/* IAMROOT-12A:
 * ------------
 * GOT 영역을 알리는 시작과 끝에 delta를 더해 교정함.
 */
		add	r11, r11, r0
		add	r12, r12, r0

#ifndef CONFIG_ZBOOT_ROM
		/*
		 * If we're running fully PIC === CONFIG_ZBOOT_ROM = n,
		 * we need to fix up pointers into the BSS region.
		 * Note that the stack pointer has already been fixed up.
		 */

/* IAMROOT-12A:
 * ------------
 * BSS 영역을 알리는 시작과 끝에 delta를 더해 교정함.
 */
		add	r2, r2, r0
		add	r3, r3, r0

/* IAMROOT-12A:
 * ------------
 * GOT 엔트리는 재배치가 일어나면 연결된 주소가 다 엉뚱한 곳을 가리키기 때문에
 * 링크가 존재하는 모든 엔트리들은 delta 및 dtb size 만큼 더해서 교정해야 한다.
 * 엔트리들은 PLT를 가리킬 수도 있고, 외부 함수를 가리킬 수 있다.
 * 그러나 zImage는 외부함수가 연결될 가능성은 없고 내부에서 BSS 영역에 존재하는
 * 관련 함수등이 있을거라 예상된다.
 *
 * r11(GOT start) 주소에서 GOT 엔트리를 읽어 r1에 대입
 * r1(엔트리) += delta로 교정
 * dtb size 만큼 추가로 교정할지 판단
 * 4바이트 만큼 이동하여 다음 엔트리 주소가 마지막이 아니면 다시 레이블 1로 루프.
 */

		/*
		 * Relocate all entries in the GOT table.
		 * Bump bss entries to _edata + dtb size
		 */
1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		add	r1, r1, r0		@ This fixes up C references
		cmp	r1, r2			@ if entry >= bss_start &&
		cmphs	r3, r1			@       bss_end > entry
		addhi	r1, r1, r5		@    entry += dtb size
		str	r1, [r11], #4		@ next entry
		cmp	r11, r12
		blo	1b

/* IAMROOT-12A:
 * ------------
 * BSS 영역을 알리는 시작과 끝에 DTB 사이즈만큼 추가 교정(delta는 이미 추가하였었음)
 */

		/* bump our bss pointers too */
		add	r2, r2, r5
		add	r3, r3, r5

#else

		/*
		 * Relocate entries in the GOT table.  We only relocate
		 * the entries that are outside the (relocated) BSS region.
		 */

/* IAMROOT-12A:
 * ------------
 * ROM또는 플래쉬에서 직접 수행되는 경우
 * TODO: 언제 이 영역이 리로케이션 되었는지 확인 필요....
 *
 * r11(GOT start)주소에서 GOT엔트리를 읽어 r1에 대입
 * if r1(엔트리) < r2(bss_start) ||
 *    r3(_end) < r1(엔트리)
 *       r1 += r0(delta)
 *
 * 직역)
 *		if r1 >= r2 then 
 *		    if r3 < r1 then
 *		       r1 += r0
 *		else
 *		    r1 += r0
 */

1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		cmp	r1, r2			@ entry < bss_start ||
		cmphs	r3, r1			@ _end < entry
		addlo	r1, r1, r0		@ table.  This fixes up the
		str	r1, [r11], #4		@ C references.
		cmp	r11, r12
		blo	1b
#endif


/* IAMROOT-12A:
 * ------------
 * BSS 영역 데이터를0x00으로 초기화 한다.
 * (라즈베리파이2: BSS 공간이 0x1c개 만큼인데 일부 over해서 초기화 함
 * 그래도 됨? -- 사실 16바이트 align을 기대하고 readelf로 분석을 해보니
 * 해당 공간은 8바이트 align 상태여서 뭔가 맞지 않는다는느낌이다.
 * 어쨋든 그 최대 파괴되는 일부 바이트가 위치한 공간은 스택 중 가장 아래에
 * 위치한 공간이고 확실히 아직 스택을 많이 사용한 적이 없으므로 전혀 상관
 * 없다고 함. 다나 Russel king이 우리들 코드 분석하는데 애 좀 먹도록 이 부분
 * 주석도 없이 그냥 사용했다는 것이.....
 * 캐시가 on된 상태에서 캐시라인을 충분히 사용하므로 4번의 Word를 연속 저장하여
 * 성능을 높이려는 것은 충분히 이해가 가는데 왜 어찌 일부 초과한 상태라도 
 * 이해 바란다는 주석이 없을까?
 *
 * 참고: .align
 * default align은 32비트 시스템은 4바이트, 64비트 시스템은 8바이트
 * .align 지시자는 인수의 해석을 시스템에 따라 달리한다.
 * 어떤 시스템에서는 n 바이트 단위로 지정을 하고, 또 다른 시스템에서는
 * 2^n 바이트 단위로 동작한다.
 * - n 바이트 단위: 
 *    a29k, hppa, m68k, sparc, Xtensa, Renesas / SuperH SH, i386 using ELF format
 * - 2^n 바이트 단위: 
 *    i386 using a.out (old format), arm, strong ARMv3
 * 대문자로 표현된 ALIGN(8)과 같은 매크로는 8바이트를 의미.
 */

not_relocated:	mov	r0, #0
1:		str	r0, [r2], #4		@ clear bss
		str	r0, [r2], #4
		str	r0, [r2], #4
		str	r0, [r2], #4
		cmp	r2, r3
		blo	1b

/* IAMROOT-12A:
 * ------------
 * r4 레지스터의 0번 비트가 set되어 있지 않으면 cache_on을 호출
 * r4 레지스터는 커널 시작 주소로 계속 사용되어야 하므로 하위 1비트를
 * 다른 용도로 잠시 사용하였던 것을 없애기 위해 clear 해야 한다.
 */
		/*
		 * Did we skip the cache setup earlier?
		 * That is indicated by the LSB in r4.
		 * Do it now if so.
		 */
		tst	r4, #1
		bic	r4, r4, #1
		blne	cache_on

/*
 * The C runtime environment should now be setup sufficiently.
 * Set up some pointers, and start decompressing.
 *   r4  = kernel execution address
 *   r7  = architecture ID
 *   r8  = atags pointer
 */

/* IAMROOT-12A:
 * ------------
 * decompress_kernel(kernel execution address, sp, sp+0x10000, architecture ID) 호출
 */
		mov	r0, r4
		mov	r1, sp			@ malloc space above stack
		add	r2, sp, #0x10000	@ 64k max
		mov	r3, r7
		bl	decompress_kernel

/* IAMROOT-12A:
 * ------------
 * decompress가 끝났으면 커널 설정을 하러가기 전에 캐쉬를 클린 +플러쉬 + off 한다.
 */

		bl	cache_clean_flush
		bl	cache_off

		mov	r1, r7			@ restore architecture number
		mov	r2, r8			@ restore atags pointer

#ifdef CONFIG_ARM_VIRT_EXT
		mrs	r0, spsr		@ Get saved CPU boot mode
		and	r0, r0, #MODE_MASK
		cmp	r0, #HYP_MODE		@ if not booted in HYP mode...
		bne	__enter_kernel		@ boot kernel directly

		adr	r12, .L__hyp_reentry_vectors_offset
		ldr	r0, [r12]
		add	r0, r0, r12

		bl	__hyp_set_vectors
		__HVC(0)			@ otherwise bounce to hyp mode

		b	.			@ should never be reached

		.align	2
.L__hyp_reentry_vectors_offset:	.long	__hyp_reentry_vectors - .
#else


/* IAMROOT-12A:
 * ------------
 * 한 번 가면 다시 돌아올일 없음.
 */

		b	__enter_kernel
#endif

/* IAMROOT-12A:
 * .size <- .(현재) - LC0가 object의 크기로 정의됨. (for ELF)
 * _end <- bss의 끝(bss영역은 compressed-zImage 바로 상위)
 */

		.align	2
		.type	LC0, #object
LC0:		.word	LC0			@ r1
		.word	__bss_start		@ r2
		.word	_end			@ r3
		.word	_edata			@ r6
		.word	input_data_end - 4	@ r10 (inflated size location)
		.word	_got_start		@ r11
		.word	_got_end		@ ip
		.word	.L_user_stack_end	@ sp
		.word	_end - restart + 16384 + 1024*1024
		.size	LC0, . - LC0

/* IAMROOT-12A:
 * ------------
 * RIskPC인 경우에만 사용.
 */

#ifdef CONFIG_ARCH_RPC
		.globl	params
params:		ldr	r0, =0x10000100		@ params_phys for RPC
		mov	pc, lr
		.ltorg
		.align
#endif

/*
 * Turn on the cache.  We need to setup some page tables so that we
 * can have both the I and D caches on.
 *
 * We place the page tables 16k down from the kernel execution address,
 * and we hope that nothing else is using it.  If we're using it, we
 * will go pop!
 *
 * On entry,
 *  r4 = kernel execution address
 *  r7 = architecture number
 *  r8 = atags pointer
 * On exit,
 *  r0, r1, r2, r3, r9, r10, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		.align	5
cache_on:	mov	r3, #8			@ cache_on function
		b	call_cache_fn

/*
 * Initialize the highest priority protection region, PR7
 * to cover all 32bit address and cacheable and bufferable.
 */
__armv4_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting
		mcr 	p15, 0, r0, c6, c7, 1

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ D-cache on
		mcr	p15, 0, r0, c2, c0, 1	@ I-cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 1	@ I-access permission
		mcr	p15, 0, r0, c5, c0, 0	@ D-access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ ...I .... ..D. WC.M
		orr	r0, r0, #0x002d		@ .... .... ..1. 11.1
		orr	r0, r0, #0x1000		@ ...1 .... .... ....

		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mov	pc, lr

__armv3_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 0	@ access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		/*
		 * ?? ARMv3 MMU does not allow reading the control register,
		 * does this really work on ARMv3 MPU?
		 */
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ .... .... .... WC.M
		orr	r0, r0, #0x000d		@ .... .... .... 11.1
		/* ?? this overwrites the value constructed above? */
		mov	r0, #0
		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		/* ?? invalidate for the second time? */
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
#define CB_BITS 0x08
#else
#define CB_BITS 0x0c
#endif

__setup_mmu:	sub	r3, r4, #16384		@ Page directory size
		bic	r3, r3, #0xff		@ Align the pointer
		bic	r3, r3, #0x3f00
/*
 * Initialise the page tables, turning on the cacheable and bufferable
 * bits for the RAM area only.
 */
		mov	r0, r3
		mov	r9, r0, lsr #18
		mov	r9, r9, lsl #18		@ start of RAM
		add	r10, r9, #0x10000000	@ a reasonable RAM size
		mov	r1, #0x12		@ XN|U + section mapping
		orr	r1, r1, #3 << 10	@ AP=11
		add	r2, r3, #16384
1:		cmp	r1, r9			@ if virt > start of RAM
		cmphs	r10, r1			@   && end of RAM > virt
		bic	r1, r1, #0x1c		@ clear XN|U + C + B
		orrlo	r1, r1, #0x10		@ Set XN|U for non-RAM
		orrhs	r1, r1, r6		@ set RAM section settings
		str	r1, [r0], #4		@ 1:1 mapping
		add	r1, r1, #1048576
		teq	r0, r2
		bne	1b
/*
 * If ever we are running from Flash, then we surely want the cache
 * to be enabled also for our execution instance...  We map 2MB of it
 * so there is no map overlap problem for up to 1 MB compressed kernel.
 * If the execution is in RAM then we would only be duplicating the above.
 */
		orr	r1, r6, #0x04		@ ensure B is set for this
		orr	r1, r1, #3 << 10
		mov	r2, pc
		mov	r2, r2, lsr #20
		orr	r1, r1, r2, lsl #20
		add	r0, r3, r2, lsl #2
		str	r1, [r0], #4
		add	r1, r1, #1048576
		str	r1, [r0]
		mov	pc, lr
ENDPROC(__setup_mmu)

@ Enable unaligned access on v6, to allow better code generation
@ for the decompressor C code:
__armv6_mmu_cache_on:
		mrc	p15, 0, r0, c1, c0, 0	@ read SCTLR
		bic	r0, r0, #2		@ A (no unaligned access fault)
		orr	r0, r0, #1 << 22	@ U (v6 unaligned access model)
		mcr	p15, 0, r0, c1, c0, 0	@ write SCTLR
		b	__armv4_mmu_cache_on

__arm926ejs_mmu_cache_on:
#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
		mov	r0, #4			@ put dcache in WT mode
		mcr	p15, 7, r0, c15, c0, 0
#endif

__armv4_mmu_cache_on:
		mov	r12, lr
#ifdef CONFIG_MMU
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x0030
 ARM_BE8(	orr	r0, r0, #1 << 25 )	@ big-endian page tables
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
		mov	pc, r12

__armv7_mmu_cache_on:
		mov	r12, lr
#ifdef CONFIG_MMU
		mrc	p15, 0, r11, c0, c1, 4	@ read ID_MMFR0
		tst	r11, #0xf		@ VMSA
		movne	r6, #CB_BITS | 0x02	@ !XN
		blne	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		tst	r11, #0xf		@ VMSA
		mcrne	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		bic	r0, r0, #1 << 28	@ clear SCTLR.TRE
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x003c		@ write buffer
		bic	r0, r0, #2		@ A (no unaligned access fault)
		orr	r0, r0, #1 << 22	@ U (v6 unaligned access model)
						@ (needed for ARM1176)
#ifdef CONFIG_MMU
 ARM_BE8(	orr	r0, r0, #1 << 25 )	@ big-endian page tables
		mrcne   p15, 0, r6, c2, c0, 2   @ read ttb control reg
		orrne	r0, r0, #1		@ MMU enabled
		movne	r1, #0xfffffffd		@ domain 0 = client
		bic     r6, r6, #1 << 31        @ 32-bit translation system
		bic     r6, r6, #3 << 0         @ use only ttbr0
		mcrne	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcrne	p15, 0, r1, c3, c0, 0	@ load domain access control
		mcrne   p15, 0, r6, c2, c0, 2   @ load ttb control
#endif
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back
		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

__fa526_cache_on:
		mov	r12, lr
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7, 0	@ Invalidate whole cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x1000		@ I-cache enable
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mov	pc, r12

__common_mmu_cache_on:
#ifndef CONFIG_THUMB2_KERNEL
#ifndef DEBUG
		orr	r0, r0, #0x000d		@ Write buffer, mmu
#endif
		mov	r1, #-1
		mcr	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcr	p15, 0, r1, c3, c0, 0	@ load domain access control
		b	1f
		.align	5			@ cache line aligned
1:		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back to
		sub	pc, lr, r0, lsr #32	@ properly flush pipeline
#endif

#define PROC_ENTRY_SIZE (4*5)

/*
 * Here follow the relocatable cache support functions for the
 * various processors.  This is a generic hook for locating an
 * entry and jumping to an instruction at the specified offset
 * from the start of the block.  Please note this is all position
 * independent code.
 *
 *  r1  = corrupted
 *  r2  = corrupted
 *  r3  = block offset
 *  r9  = corrupted
 *  r12 = corrupted
 */

call_cache_fn:	adr	r12, proc_types
#ifdef CONFIG_CPU_CP15
		mrc	p15, 0, r9, c0, c0	@ get processor ID
#else
		ldr	r9, =CONFIG_PROCESSOR_ID
#endif
1:		ldr	r1, [r12, #0]		@ get value
		ldr	r2, [r12, #4]		@ get mask
		eor	r1, r1, r9		@ (real ^ match)
		tst	r1, r2			@       & mask
 ARM(		addeq	pc, r12, r3		) @ call cache function
 THUMB(		addeq	r12, r3			)
 THUMB(		moveq	pc, r12			) @ call cache function
		add	r12, r12, #PROC_ENTRY_SIZE
		b	1b

/*
 * Table for cache operations.  This is basically:
 *   - CPU ID match
 *   - CPU ID mask
 *   - 'cache on' method instruction
 *   - 'cache off' method instruction
 *   - 'cache flush' method instruction
 *
 * We match an entry using: ((real_id ^ match) & mask) == 0
 *
 * Writethrough caches generally only need 'on' and 'off'
 * methods.  Writeback caches _must_ have the flush method
 * defined.
 */
		.align	2
		.type	proc_types,#object
proc_types:
		.word	0x41000000		@ old ARM ID
		.word	0xff00f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007000		@ ARM7/710
		.word	0xfff8fe00
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41807200		@ ARM720T (writethrough)
		.word	0xffffff00
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007400		@ ARM74x
		.word	0xff00ff00
		W(b)	__armv3_mpu_cache_on
		W(b)	__armv3_mpu_cache_off
		W(b)	__armv3_mpu_cache_flush
		
		.word	0x41009400		@ ARM94x
		.word	0xff00ff00
		W(b)	__armv4_mpu_cache_on
		W(b)	__armv4_mpu_cache_off
		W(b)	__armv4_mpu_cache_flush

		.word	0x41069260		@ ARM926EJ-S (v5TEJ)
		.word	0xff0ffff0
		W(b)	__arm926ejs_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x00007000		@ ARM7 IDs
		.word	0x0000f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		@ Everything from here on will be the new ID system.

		.word	0x4401a100		@ sa110 / sa1100
		.word	0xffffffe0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x6901b110		@ sa1110
		.word	0xfffffff0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56056900
		.word	0xffffff00		@ PXA9xx
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56158000		@ PXA168
		.word	0xfffff000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x56050000		@ Feroceon
		.word	0xff0f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

#ifdef CONFIG_CPU_FEROCEON_OLD_ID
		/* this conflicts with the standard ARMv5TE entry */
		.long	0x41009260		@ Old Feroceon
		.long	0xff00fff0
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv5tej_mmu_cache_flush
#endif

		.word	0x66015261		@ FA526
		.word	0xff01fff1
		W(b)	__fa526_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__fa526_cache_flush

		@ These match on the architecture ID

		.word	0x00020000		@ ARMv4T
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00050000		@ ARMv5TE
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00060000		@ ARMv5TEJ
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x0007b000		@ ARMv6
		.word	0x000ff000
		W(b)	__armv6_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv6_mmu_cache_flush

		.word	0x000f0000		@ new CPU Id
		.word	0x000f0000
		W(b)	__armv7_mmu_cache_on
		W(b)	__armv7_mmu_cache_off
		W(b)	__armv7_mmu_cache_flush

		.word	0			@ unrecognised type
		.word	0
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.size	proc_types, . - proc_types

		/*
		 * If you get a "non-constant expression in ".if" statement"
		 * error from the assembler on this line, check that you have
		 * not accidentally written a "b" instruction where you should
		 * have written W(b).
		 */
		.if (. - proc_types) % PROC_ENTRY_SIZE != 0
		.error "The size of one or more proc_types entries is wrong."
		.endif

/*
 * Turn off the Cache and MMU.  ARMv3 does not support
 * reading the control register, but ARMv4 does.
 *
 * On exit,
 *  r0, r1, r2, r3, r9, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		.align	5
cache_off:	mov	r3, #12			@ cache_off function
		b	call_cache_fn

__armv4_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c6, 0	@ flush D-Cache
		mcr	p15, 0, r0, c7, c5, 0	@ flush I-Cache
		mov	pc, lr

__armv3_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0, 0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

__armv4_mmu_cache_off:
#ifdef CONFIG_MMU
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7	@ invalidate whole cache v4
		mcr	p15, 0, r0, c8, c7	@ invalidate whole TLB v4
#endif
		mov	pc, lr

__armv7_mmu_cache_off:
		mrc	p15, 0, r0, c1, c0
#ifdef CONFIG_MMU
		bic	r0, r0, #0x000d
#else
		bic	r0, r0, #0x000c
#endif
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r12, lr
		bl	__armv7_mmu_cache_flush
		mov	r0, #0
#ifdef CONFIG_MMU
		mcr	p15, 0, r0, c8, c7, 0	@ invalidate whole TLB
#endif
		mcr	p15, 0, r0, c7, c5, 6	@ invalidate BTC
		mcr	p15, 0, r0, c7, c10, 4	@ DSB
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

/*
 * Clean and flush the cache to maintain consistency.
 *
 * On exit,
 *  r1, r2, r3, r9, r10, r11, r12 corrupted
 * This routine must preserve:
 *  r4, r6, r7, r8
 */
		.align	5
cache_clean_flush:
		mov	r3, #16
		b	call_cache_fn

__armv4_mpu_cache_flush:
		tst	r4, #1
		movne	pc, lr
		mov	r2, #1
		mov	r3, #0
		mcr	p15, 0, ip, c7, c6, 0	@ invalidate D cache
		mov	r1, #7 << 5		@ 8 segments
1:		orr	r3, r1, #63 << 26	@ 64 entries
2:		mcr	p15, 0, r3, c7, c14, 2	@ clean & invalidate D index
		subs	r3, r3, #1 << 26
		bcs	2b			@ entries 63 to 0
		subs 	r1, r1, #1 << 5
		bcs	1b			@ segments 7 to 0

		teq	r2, #0
		mcrne	p15, 0, ip, c7, c5, 0	@ invalidate I cache
		mcr	p15, 0, ip, c7, c10, 4	@ drain WB
		mov	pc, lr
		
__fa526_cache_flush:
		tst	r4, #1
		movne	pc, lr
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean and invalidate D cache
		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv6_mmu_cache_flush:
		mov	r1, #0
		tst	r4, #1
		mcreq	p15, 0, r1, c7, c14, 0	@ clean+invalidate D
		mcr	p15, 0, r1, c7, c5, 0	@ invalidate I+BTB
		mcreq	p15, 0, r1, c7, c15, 0	@ clean+invalidate unified
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv7_mmu_cache_flush:
		tst	r4, #1
		bne	iflush
		mrc	p15, 0, r10, c0, c1, 5	@ read ID_MMFR1
		tst	r10, #0xf << 16		@ hierarchical cache (ARMv7)
		mov	r10, #0
		beq	hierarchical
		mcr	p15, 0, r10, c7, c14, 0	@ clean+invalidate D
		b	iflush
hierarchical:
		mcr	p15, 0, r10, c7, c10, 5	@ DMB
		stmfd	sp!, {r0-r7, r9-r11}
		mrc	p15, 1, r0, c0, c0, 1	@ read clidr
		ands	r3, r0, #0x7000000	@ extract loc from clidr
		mov	r3, r3, lsr #23		@ left align loc bit field
		beq	finished		@ if loc is 0, then no need to clean
		mov	r10, #0			@ start clean at cache level 0
loop1:
		add	r2, r10, r10, lsr #1	@ work out 3x current cache level
		mov	r1, r0, lsr r2		@ extract cache type bits from clidr
		and	r1, r1, #7		@ mask of the bits for current cache only
		cmp	r1, #2			@ see what cache we have at this level
		blt	skip			@ skip if no cache, or just i-cache
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
		mcr	p15, 0, r10, c7, c5, 4	@ isb to sych the new cssr&csidr
		mrc	p15, 1, r1, c0, c0, 0	@ read the new csidr
		and	r2, r1, #7		@ extract the length of the cache lines
		add	r2, r2, #4		@ add 4 (line length offset)
		ldr	r4, =0x3ff
		ands	r4, r4, r1, lsr #3	@ find maximum number on the way size
		clz	r5, r4			@ find bit position of way size increment
		ldr	r7, =0x7fff
		ands	r7, r7, r1, lsr #13	@ extract max number of the index size
loop2:
		mov	r9, r4			@ create working copy of max way size
loop3:
 ARM(		orr	r11, r10, r9, lsl r5	) @ factor way and cache number into r11
 ARM(		orr	r11, r11, r7, lsl r2	) @ factor index number into r11
 THUMB(		lsl	r6, r9, r5		)
 THUMB(		orr	r11, r10, r6		) @ factor way and cache number into r11
 THUMB(		lsl	r6, r7, r2		)
 THUMB(		orr	r11, r11, r6		) @ factor index number into r11
		mcr	p15, 0, r11, c7, c14, 2	@ clean & invalidate by set/way
		subs	r9, r9, #1		@ decrement the way
		bge	loop3
		subs	r7, r7, #1		@ decrement the index
		bge	loop2
skip:
		add	r10, r10, #2		@ increment cache number
		cmp	r3, r10
		bgt	loop1
finished:
		ldmfd	sp!, {r0-r7, r9-r11}
		mov	r10, #0			@ swith back to cache level 0
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
iflush:
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
		mcr	p15, 0, r10, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
		mcr	p15, 0, r10, c7, c5, 4	@ ISB
		mov	pc, lr

__armv5tej_mmu_cache_flush:
		tst	r4, #1
		movne	pc, lr
1:		mrc	p15, 0, r15, c7, c14, 3	@ test,clean,invalidate D cache
		bne	1b
		mcr	p15, 0, r0, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv4_mmu_cache_flush:
		tst	r4, #1
		movne	pc, lr
		mov	r2, #64*1024		@ default: 32K dcache size (*2)
		mov	r11, #32		@ default: 32 byte line size
		mrc	p15, 0, r3, c0, c0, 1	@ read cache type
		teq	r3, r9			@ cache ID register present?
		beq	no_cache_id
		mov	r1, r3, lsr #18
		and	r1, r1, #7
		mov	r2, #1024
		mov	r2, r2, lsl r1		@ base dcache size *2
		tst	r3, #1 << 14		@ test M bit
		addne	r2, r2, r2, lsr #1	@ +1/2 size if M == 1
		mov	r3, r3, lsr #12
		and	r3, r3, #3
		mov	r11, #8
		mov	r11, r11, lsl r3	@ cache line size in bytes
no_cache_id:
		mov	r1, pc
		bic	r1, r1, #63		@ align to longest cache line
		add	r2, r1, r2
1:
 ARM(		ldr	r3, [r1], r11		) @ s/w flush D cache
 THUMB(		ldr     r3, [r1]		) @ s/w flush D cache
 THUMB(		add     r1, r1, r11		)
		teq	r1, r2
		bne	1b

		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c6, 0	@ flush D cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv3_mmu_cache_flush:
__armv3_mpu_cache_flush:
		tst	r4, #1
		movne	pc, lr
		mov	r1, #0
		mcr	p15, 0, r1, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

/*
 * Various debugging routines for printing hex characters and
 * memory, which again must be relocatable.
 */
#ifdef DEBUG
		.align	2
		.type	phexbuf,#object
phexbuf:	.space	12
		.size	phexbuf, . - phexbuf

@ phex corrupts {r0, r1, r2, r3}
phex:		adr	r3, phexbuf
		mov	r2, #0
		strb	r2, [r3, r1]
1:		subs	r1, r1, #1
		movmi	r0, r3
		bmi	puts
		and	r2, r0, #15
		mov	r0, r0, lsr #4
		cmp	r2, #10
		addge	r2, r2, #7
		add	r2, r2, #'0'
		strb	r2, [r3, r1]
		b	1b

@ puts corrupts {r0, r1, r2, r3}
puts:		loadsp	r3, r1
1:		ldrb	r2, [r0], #1
		teq	r2, #0
		moveq	pc, lr
2:		writeb	r2, r3
		mov	r1, #0x00020000
3:		subs	r1, r1, #1
		bne	3b
		teq	r2, #'\n'
		moveq	r2, #'\r'
		beq	2b
		teq	r0, #0
		bne	1b
		mov	pc, lr
@ putc corrupts {r0, r1, r2, r3}
putc:
		mov	r2, r0
		mov	r0, #0
		loadsp	r3, r1
		b	2b

@ memdump corrupts {r0, r1, r2, r3, r10, r11, r12, lr}
memdump:	mov	r12, r0
		mov	r10, lr
		mov	r11, #0
2:		mov	r0, r11, lsl #2
		add	r0, r0, r12
		mov	r1, #8
		bl	phex
		mov	r0, #':'
		bl	putc
1:		mov	r0, #' '
		bl	putc
		ldr	r0, [r12, r11, lsl #2]
		mov	r1, #8
		bl	phex
		and	r0, r11, #7
		teq	r0, #3
		moveq	r0, #' '
		bleq	putc
		and	r0, r11, #7
		add	r11, r11, #1
		teq	r0, #7
		bne	1b
		mov	r0, #'\n'
		bl	putc
		cmp	r11, #64
		blt	2b
		mov	pc, r10
#endif

		.ltorg

#ifdef CONFIG_ARM_VIRT_EXT
.align 5
__hyp_reentry_vectors:
		W(b)	.			@ reset
		W(b)	.			@ undef
		W(b)	.			@ svc
		W(b)	.			@ pabort
		W(b)	.			@ dabort
		W(b)	__enter_kernel		@ hyp
		W(b)	.			@ irq
		W(b)	.			@ fiq
#endif /* CONFIG_ARM_VIRT_EXT */

__enter_kernel:
		mov	r0, #0			@ must be 0
 ARM(		mov	pc, r4	)		@ call kernel
 THUMB(		bx	r4	)		@ entry point is always ARM

reloc_code_end:

		.align
		.section ".stack", "aw", %nobits
.L_user_stack:	.space	4096
.L_user_stack_end:
