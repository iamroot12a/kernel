/* ld script to make ARM Linux kernel
 * taken from the i386 version by Russell King
 * Written by Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 */

#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>
#include <asm/thread_info.h>
#include <asm/memory.h>
#include <asm/page.h>
#ifdef CONFIG_ARM_KERNMEM_PERMS
#include <asm/pgtable.h>
#endif
	
/* IAMROOT-12A:
 * ------------
 * PROC_INFO 매크로에서 사용된 .proc.info.init 섹션에는 프로세스 정보가 담겨있다.
 * 라즈베리파이2의 경우 armv7 아키텍처를 사용하여 빌드하면 관련된 여러 개의
 * armv7 프로세서 정보들이 모이게된다.
 *
 * 라즈베리파이2의 경우 ARM Cortex-A7 아키텍처를 사용하므로 __v7_ca7mp_proc_info 
 * 구조체를 참고하여야 한다.
 * 관련화일: mm/proc-v7.S
 */
#define PROC_INFO							\
	. = ALIGN(4);							\
	VMLINUX_SYMBOL(__proc_info_begin) = .;				\
	*(.proc.info.init)						\
	VMLINUX_SYMBOL(__proc_info_end) = .;

#define IDMAP_TEXT							\
	ALIGN_FUNCTION();						\
	VMLINUX_SYMBOL(__idmap_text_start) = .;				\
	*(.idmap.text)							\
	VMLINUX_SYMBOL(__idmap_text_end) = .;				\
	. = ALIGN(32);							\
	VMLINUX_SYMBOL(__hyp_idmap_text_start) = .;			\
	*(.hyp.idmap.text)						\
	VMLINUX_SYMBOL(__hyp_idmap_text_end) = .;

/* IAMROOT-12A:
 * ------------
 * CONFIG_HOTPLUG_CPU: how swap이 지원되는 시스템에 따라 아래 두 매크로가
 * enable/disable 교차된다.
 */
#ifdef CONFIG_HOTPLUG_CPU
#define ARM_CPU_DISCARD(x)
#define ARM_CPU_KEEP(x)		x
#else
#define ARM_CPU_DISCARD(x)	x
#define ARM_CPU_KEEP(x)
#endif

#if (defined(CONFIG_SMP_ON_UP) && !defined(CONFIG_DEBUG_SPINLOCK)) || \
	defined(CONFIG_GENERIC_BUG)
#define ARM_EXIT_KEEP(x)	x
#define ARM_EXIT_DISCARD(x)
#else
#define ARM_EXIT_KEEP(x)
#define ARM_EXIT_DISCARD(x)	x
#endif

OUTPUT_ARCH(arm)
ENTRY(stext)

#ifndef __ARMEB__
jiffies = jiffies_64;
#else
jiffies = jiffies_64 + 4;
#endif

SECTIONS
{
	/*
	 * XXX: The linker does not define how output sections are
	 * assigned to input sections when there are multiple statements
	 * matching the same input section name.  There is no documented
	 * order of matching.
	 *
	 * unwind exit sections must be discarded before the rest of the
	 * unwind sections get included.
	 */
	/DISCARD/ : {
		*(.ARM.exidx.exit.text)
		*(.ARM.extab.exit.text)
		ARM_CPU_DISCARD(*(.ARM.exidx.cpuexit.text))
		ARM_CPU_DISCARD(*(.ARM.extab.cpuexit.text))
		ARM_EXIT_DISCARD(EXIT_TEXT)
		ARM_EXIT_DISCARD(EXIT_DATA)
		EXIT_CALL
#ifndef CONFIG_MMU
		*(.fixup)
		*(__ex_table)
#endif
#ifndef CONFIG_SMP_ON_UP
		*(.alt.smp.init)
#endif
		*(.discard)
		*(.discard.*)
	}

#ifdef CONFIG_XIP_KERNEL
	. = XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR);
#else
	. = PAGE_OFFSET + TEXT_OFFSET;
#endif
	.head.text : {
		_text = .;
		HEAD_TEXT
	}

#ifdef CONFIG_ARM_KERNMEM_PERMS
	. = ALIGN(1<<SECTION_SHIFT);
#endif

	.text : {			/* Real text segment		*/
		_stext = .;		/* Text and read-only data	*/
			__exception_text_start = .;
			*(.exception.text)
			__exception_text_end = .;
			IRQENTRY_TEXT
			TEXT_TEXT
			SCHED_TEXT
			LOCK_TEXT
			KPROBES_TEXT
			IDMAP_TEXT
#ifdef CONFIG_MMU
			*(.fixup)
#endif
			*(.gnu.warning)
			*(.glue_7)
			*(.glue_7t)
		. = ALIGN(4);
		*(.got)			/* Global offset table		*/
/* IAMROOT-12A:
 * ------------
 * PROC_INFO 매크로에서 사용된 프로세스 정보들은 일반적으로 .proc.info.init 섹션에
 * 모두 모였다가 .init.proc.info에 다시 합쳐지며 커널 초기화 후 더 이상 사용할 
 * 필요가 없어서 버리게 된다.
 *
 * 그러나 ARM_CPU_KEEP() 매크로는 cpu hot swap을 지원하는 커널용(CONFIG_HOTPLUG_CPU)으로
 * 빌드된 경우 프로세스 정보를 계속 사용하게 하기 위해 프로세스 정보를 버리지 않도록
 * 저장 위치를 지워지지 않는 커널 코드 영역 즉 .text 섹션의 마지막에 빌드한다.
 */
			ARM_CPU_KEEP(PROC_INFO)
	}

#ifdef CONFIG_DEBUG_RODATA
	. = ALIGN(1<<SECTION_SHIFT);
#endif
	RO_DATA(PAGE_SIZE)

	. = ALIGN(4);
	__ex_table : AT(ADDR(__ex_table) - LOAD_OFFSET) {
		__start___ex_table = .;
#ifdef CONFIG_MMU
		*(__ex_table)
#endif
		__stop___ex_table = .;
	}

#ifdef CONFIG_ARM_UNWIND
	/*
	 * Stack unwinding tables
	 */
	. = ALIGN(8);
	.ARM.unwind_idx : {
		__start_unwind_idx = .;
		*(.ARM.exidx*)
		__stop_unwind_idx = .;
	}
	.ARM.unwind_tab : {
		__start_unwind_tab = .;
		*(.ARM.extab*)
		__stop_unwind_tab = .;
	}
#endif

	NOTES

	_etext = .;			/* End of text and rodata section */

#ifndef CONFIG_XIP_KERNEL
# ifdef CONFIG_ARM_KERNMEM_PERMS
	. = ALIGN(1<<SECTION_SHIFT);
# else
	. = ALIGN(PAGE_SIZE);
# endif
	__init_begin = .;
#endif
	/*
	 * The vectors and stubs are relocatable code, and the
	 * only thing that matters is their relative offsets
	 */
	__vectors_start = .;
	.vectors 0 : AT(__vectors_start) {
		*(.vectors)
	}
	. = __vectors_start + SIZEOF(.vectors);
	__vectors_end = .;

	__stubs_start = .;
	.stubs 0x1000 : AT(__stubs_start) {
		*(.stubs)
	}
	. = __stubs_start + SIZEOF(.stubs);
	__stubs_end = .;

	INIT_TEXT_SECTION(8)
	.exit.text : {
		ARM_EXIT_KEEP(EXIT_TEXT)
	}

/* IAMROOT-12A:
 * ------------
 * 라즈베리2 빌드세트는 CONFIG_HOTPLUG_CPU 옵션을 사용하여 빌드하지 않았으므로
 *	프로세서 정보는 ARM_CPU_KEEP() 매크로가 사용된 위치에 저장되지 않고
 *			ARM_CPU_DISCARD() 매크로가 사용된 위치에서 저장됨.
 *
 * .init 영역은 메모리 절약을 위해 커널이 한 번 초기화 때 사용 후 버려지는 영역.
 */
	.init.proc.info : {
		ARM_CPU_DISCARD(PROC_INFO)
	}
	.init.arch.info : {
		__arch_info_begin = .;
		*(.arch.info.init)
		__arch_info_end = .;
	}
	.init.tagtable : {
		__tagtable_begin = .;
		*(.taglist.init)
		__tagtable_end = .;
	}

/* IAMROOT-12A:
 * ------------
 * ALT_UP() 매크로를 사용할 때 마다 .alt.smp.init 섹션에 ALT_SMP() 매크로에서
 * 사용하는 명령의 주소 4바이트와 ALT_UP() 매크로에서 인수로 사용된 명령
 * 4바이트 총 8바이트가 담긴다. 
 *
 * ALT_SMP()사용 후 항상 ALT_UP()이 뒤 따른다.
 */
#ifdef CONFIG_SMP_ON_UP
	.init.smpalt : {
		__smpalt_begin = .;
		*(.alt.smp.init)
		__smpalt_end = .;
	}
#endif

/* IAMROOT-12A:
 * ------------
 * __pv_stub() 매크로를 사용할 때 마다 pv_table 섹션에 __pv_stub() 매크로에서
 * 사용하는 add or sub 명령의 주소가 담긴다.
 *
 * __pv_stub() 매크로는 __virt_to_phys() 및 __phys_to_virt() 인라인 함수에서 사용된다.
 */
	.init.pv_table : {
		__pv_table_begin = .;
		*(.pv_table)
		__pv_table_end = .;
	}
	.init.data : {
#ifndef CONFIG_XIP_KERNEL
		INIT_DATA
#endif
		INIT_SETUP(16)
		INIT_CALLS
		CON_INITCALL
		SECURITY_INITCALL
		INIT_RAM_FS
	}
#ifndef CONFIG_XIP_KERNEL
	.exit.data : {
		ARM_EXIT_KEEP(EXIT_DATA)
	}
#endif

#ifdef CONFIG_SMP
	PERCPU_SECTION(L1_CACHE_BYTES)
#endif

#ifdef CONFIG_XIP_KERNEL
	__data_loc = ALIGN(4);		/* location in binary */
	. = PAGE_OFFSET + TEXT_OFFSET;
#else
#ifdef CONFIG_ARM_KERNMEM_PERMS
	. = ALIGN(1<<SECTION_SHIFT);
#else
	. = ALIGN(THREAD_SIZE);
#endif
	__init_end = .;
	__data_loc = .;
#endif

	.data : AT(__data_loc) {
		_data = .;		/* address in memory */
		_sdata = .;

		/*
		 * first, the init task union, aligned
		 * to an 8192 byte boundary.
		 */
		INIT_TASK_DATA(THREAD_SIZE)

#ifdef CONFIG_XIP_KERNEL
		. = ALIGN(PAGE_SIZE);
		__init_begin = .;
		INIT_DATA
		ARM_EXIT_KEEP(EXIT_DATA)
		. = ALIGN(PAGE_SIZE);
		__init_end = .;
#endif

		NOSAVE_DATA
		CACHELINE_ALIGNED_DATA(L1_CACHE_BYTES)
		READ_MOSTLY_DATA(L1_CACHE_BYTES)

		/*
		 * and the usual data section
		 */
		DATA_DATA
		CONSTRUCTORS

		_edata = .;
	}
	_edata_loc = __data_loc + SIZEOF(.data);

#ifdef CONFIG_HAVE_TCM
        /*
	 * We align everything to a page boundary so we can
	 * free it after init has commenced and TCM contents have
	 * been copied to its destination.
	 */
	.tcm_start : {
		. = ALIGN(PAGE_SIZE);
		__tcm_start = .;
		__itcm_start = .;
	}

	/*
	 * Link these to the ITCM RAM
	 * Put VMA to the TCM address and LMA to the common RAM
	 * and we'll upload the contents from RAM to TCM and free
	 * the used RAM after that.
	 */
	.text_itcm ITCM_OFFSET : AT(__itcm_start)
	{
		__sitcm_text = .;
		*(.tcm.text)
		*(.tcm.rodata)
		. = ALIGN(4);
		__eitcm_text = .;
	}

	/*
	 * Reset the dot pointer, this is needed to create the
	 * relative __dtcm_start below (to be used as extern in code).
	 */
	. = ADDR(.tcm_start) + SIZEOF(.tcm_start) + SIZEOF(.text_itcm);

	.dtcm_start : {
		__dtcm_start = .;
	}

	/* TODO: add remainder of ITCM as well, that can be used for data! */
	.data_dtcm DTCM_OFFSET : AT(__dtcm_start)
	{
		. = ALIGN(4);
		__sdtcm_data = .;
		*(.tcm.data)
		. = ALIGN(4);
		__edtcm_data = .;
	}

	/* Reset the dot pointer or the linker gets confused */
	. = ADDR(.dtcm_start) + SIZEOF(.data_dtcm);

	/* End marker for freeing TCM copy in linked object */
	.tcm_end : AT(ADDR(.dtcm_start) + SIZEOF(.data_dtcm)){
		. = ALIGN(PAGE_SIZE);
		__tcm_end = .;
	}
#endif

	BSS_SECTION(0, 0, 0)
	_end = .;

	STABS_DEBUG
}

/*
 * These must never be empty
 * If you have to comment these two assert statements out, your
 * binutils is too old (for other reasons as well)
 */
ASSERT((__proc_info_end - __proc_info_begin), "missing CPU support")
ASSERT((__arch_info_end - __arch_info_begin), "no machine record defined")
/*
 * The HYP init code can't be more than a page long.
 * The above comment applies as well.
 */
ASSERT(((__hyp_idmap_text_end - __hyp_idmap_text_start) <= PAGE_SIZE), "HYP init code too big")
