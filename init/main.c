/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

/*
 * 참여자:
 *     김상덕 - ksd3148@gmail.com
 *     김종철 - jongchul.kim@gmail.com
 *     김형일 - khi8660@naver.com
 *     문영일 - jakeisname@gmail.com
 *     박진영 - 
 *     양유석 - xeite24@gmail.com
 *     유계성 - gsryu99@gmail.com 
 *     윤창호 - zep25dr@gmail.com
 *     이벽산 - lbyeoksan@gmail.com
 *     전성윤 - roland.korea@gmail.com
 *     조현철 - guscjf1112@gmail.com
 *     최영민 - jiggly2k@gmail.com
 *     한대근 - dev.daegeunhan@gmail.com
 *     한상종 - sjhan00000@gmail.com
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/tty.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_ext.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/kmemcheck.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/file.h>
#include <linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/sched_clock.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(unsigned long);
extern void radix_tree_init(void);
#ifndef CONFIG_DEBUG_RODATA
static inline void mark_rodata_ro(void) { }
#endif

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;


/* IAMROOT-12AB:
 * -------------
 * 부팅 과정에서 상태값이 바뀐다.
 *  enum system_states {
 *	SYSTEM_BOOTING,
 *	SYSTEM_RUNNING,
 *	SYSTEM_HALT,
 *	SYSTEM_POWER_OFF,
 *	SYSTEM_RESTART,
 *  } system_state;
 */
enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;
/* Command line for per-initcall parameter parsing */
static char *initcall_command_line;

/* IAMROOT-12:
 * -------------
 * "init=" 지정된 문자열
 */
static char *execute_command;

/* IAMROOT-12:
 * -------------
 * "rdinit=" 지정된 문자열
 */
static char *ramdisk_execute_command;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situaiton where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static int __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	int had_early_param = 0;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = 1;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return 1;
			} else if (p->setup_func(line + n))
				return 1;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val, const char *unused)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val, const char *unused)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val, unused);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val, const char *unused)
{
	repair_env_string(param, val, unused);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{

/* IAMROOT-12AB:
 * -------------
 * saved_command_line 
 *	/proc에서 볼 수 있게 연결되었고 수정되지 않는다.
 * initcall_command_line
 *	initcall 관련 parsing용으로 사용되며 변경될 수 있다.	
 * static_command_line
 *	parising용으로 사용되며 변경될 수 있다.
 */
	saved_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	initcall_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	static_command_line = memblock_virt_alloc(strlen(command_line) + 1, 0);
	strcpy(saved_command_line, boot_command_line);
	strcpy(static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __init_refok rest_init(void)
{
	int pid;

	rcu_scheduler_starting();
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	kernel_thread(kernel_init, NULL, CLONE_FS);
	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */

/* IAMROOT-12:
 * -------------
 * 부트업 cpu의 현재 init 태스크의 스케줄러를 idle로 교체한다.
 */
	init_idle_bootup_task(current);
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val, const char *unused)
{
	const struct obs_kernel_param *p;


/* IAMROOT-12AB:
 * -------------
 * 요청 파라메터가 파람블록에서 early=1인 항목이 발견되거나
 *        "        console 이면서 파람블록에 earlycon이라는 이름으로 항목이 있는 경우
 *
 * earlycon: EARLYCON_DECLARE()로 만들어진 파라메터 함수
 *	예) EARLYCON_DECLARE(pl011, pl011_early_console_setup);
 */
	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{

/* IAMROOT-12AB:
 * -------------
 * cmdline을 파싱하여 param과 val값을 가지고 강제로 unknown 핸들러인
 * do_early_param(param, val, "early options")를 호출하게 한다.
 */

	parse_args("early options", cmdline, NULL, 0, 0, 0, do_early_param);
}


/* IAMROOT-12AB:
 * -------------
 * cmdline에 있는 각 파라메터에 연결되어 있는 early 함수들을 호출
 */

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

/*
 *	Activate the first processor.
 */

static void __init boot_cpu_init(void)
{

/* IAMROOT-12A:
 * ------------
 * CPU 상태를 관리하는 몇 개의 비트맵 전역 변수에 현재 CPU의 상태를 설정한다.
 *
 * 현재 동작하는 프로세서 번호가 리턴 (0 부터~~)
 */
	int cpu = smp_processor_id();
	/* Mark the boot cpu "present", "online" etc for SMP and UP case */

/* IAMROOT-12A:
 * ------------
 * online: 
 *	현재 online 상태이고, 스케쥴 되는 상태인 cpu 집합.
 *	present cpu들 중 cpu_up() 호출되었을 때 추가됨
 * active:
 *	스케쥴을 받을 수있는 상태. 
 *	    cpu up 하면 거의 동시에 active(true),  online(true)
 *	    cpu down 하면 active(false) 후 -> online(false)
 *	CONFIG_NR_CPUS 최대값 이하.
 *	runqueue migration 등에 사용되는 cpu 집합.
 * present:
 *	현재 시스템에서 존재하는 cpu 집합. possible의 부분집합
 *	online 또는 offline 일 수 있음.
 * possible:
 *	현재 동작하거나 확장할 수 있는 상태비트로 boot시 고정.
 *	NR_CPUS 만큼 설정됨
 *	(처음 부팅 시 존재 하지 않아도 hotplug로 인해 최대 확장 가능한 수)
 *
 * 추정 순서:
 *	시스템을 NR_CPUS = 8, 부팅 시 4개 동작, CPU 카드가 별도로 추가되지 않은 상태
 *	
 *			possible	present		active		online 
 *			--------------------------------------------------------
 * 1. 첫 부팅 후:	11111111	11110000	11110000	11110000
 * 2. CPU 카드 추가:	11111111	11111111	11110000	11110000
 * 3. CPU up 요청:	11111111	11111111	11111111	11111111
 * 4. CPU down 요청:	11111111	11111111	11110000	11111111
 *    (시간차)		11111111	11111111	11110000	11110000
 * 5. CPU 카드 제거:	11111111	11110000	11110000	11110000
 *
 */

	set_cpu_online(cpu, true);
	set_cpu_active(cpu, true);
	set_cpu_present(cpu, true);
	set_cpu_possible(cpu, true);
}

void __init __weak smp_setup_processor_id(void)
{
/* IAMROOT-12A:
 * ------------
 *  __weak: __attribute__((weak))
 *          해당 symbol을 weak symbol로 만든다.
 *          링커가 링크를 수행할 때 다른 곳에
 *          같은 이름의 strong symbol이 존재하면
 *          strong symbol을 참조하고 strong symbol이 존재하지
 *          않으면 weak symbol을 참조한다.
 *          참고: http://www.valvers.com/programming/c/gcc-weak-function-attributes/
 *
 * __weak attribute 때문에 이 함수가 아니라 arch/arm/kernel/setup.c에
 * 정의되어 있는 smp_setup_processor_id가 호출될 것이다.
 */
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_info_cache_init(void)
{

/* IAMROOT-12:
 * -------------
 * rpi2: 커널 스택을 포함한 thread_info 사이즈가 페이지 사이즈를 초과하는
 *       경우에는 별도의 kmem cache를 사용할 필요가 없다. 
 *
 *       arm64등에서 64K 페이지등을 사용하는 경우에는 메모리 낭비를 줄이기 
 *       위해 kmem cache를 사용할 필요가 있다.
 */
}
#endif

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */

/* IAMROOT-12AB:
 * -------------
 * 1) &debug_guardpage_ops
 * 2) &page_poisoning_ops
 * 3) &page_owner_ops
 *
 * flatmem에서 상기 3개 항목에대한 invoke_need_callbacks()와 
 * invoke_init_callbacks() 콜백 함수를 호출한다.
 *
 * sparse mem에서는 page_ext를 나중에 초기화 한다.(page_ext_init())
 */
	page_ext_init_flatmem();
	mem_init();
	kmem_cache_init();
	percpu_init_late();
	pgtable_init();
	vmalloc_init();
}

/* IAMROOT-12A:
 * ------------
 * asmlinkage: extern "C"
 *	어셈블리 코드에서 C 함수를 호출할 때 함수 인자의 전달을 레지스터가 아닌i
 *	스택을 이용하도록 해주는 속성지정 매크로이다.
 * __visible: __attribute__((externally_visible)) 
 *            externally_visible 속성을 사용하는 경우 LTO 옵션을 사용하여 링크를
 *            하는 경우에도 하나의 완전한 함수나 객체로 외부에 보여질 수 있도록
 *            심볼화하여 해당 함수나 객체가 inline화 되지 않도록 막는다
 * __init: __section(.init.text) __cold notrace
 *         init.text 섹션에 해당 코드를 배치한다.
 * __cold: 호출될 가능성이 희박한 함수를 뜻함.
 *	   속도보다 사이즈에 더 최적화를 수행한다.
 * notrace: __attribute__((no_instrument_function))
 *          -finstrument-functions 컴파일 옵션을 사용할 때에도
 *          해당 함수에 대한 profiling을 비활성화한다.
 *          참고(https://gcc.gnu.org/onlinedocs/gcc-3.1/gcc/Function-Attributes.html)
 */
asmlinkage __visible void __init start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	/*
	 * Need to run as early as possible, to initialize the
	 * lockdep hash:
	 */
	lockdep_init();

/* IAMROOT-12A:
 * ------------
 * init_task는 init process를 나타내는 구조체이고 현재
 * 초기화가 가능한 부분만 초기화되어 있는 상태인 것으로 보임.
 */
	set_task_stack_end_magic(&init_task);
	smp_setup_processor_id();
	debug_objects_early_init();

	/*
	 * Set up the the initial canary ASAP:
	 */

/* IAMROOT-12AB:
 * -------------
 * CONFIG_CC_STACKPROTECTOR
 * -fstack-protector-all 옵션을 사용하면 기능이 활성화된다.
 */

	boot_init_stack_canary();

	cgroup_init_early();

	local_irq_disable();

/* IAMROOT-12A:
 * ------------
 * boot process 중반까지 irq가 diable되어 있는데 이 때 특정 함수에 들어가는
 * 경우 부트 프로세스가 진행중임을 알려주는 플래그이다.
 */

	early_boot_irqs_disabled = true;

/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	setup_arch(&command_line);
	mm_init_cpumask(&init_mm);
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	setup_per_cpu_areas();
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */

	build_all_zonelists(NULL, NULL);
	page_alloc_init();

	pr_notice("Kernel command line: %s\n", boot_command_line);
	parse_early_param();

/* IAMROOT-12AB:
 * -------------
 * -1 레벨 커널 파라메터들을 파싱한다.
 */
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, &unknown_bootoption);

/* IAMROOT-12AB:
 * -------------
 * cmd_line에 --를 주는 경우 그 뒷 부분에 있는 모든 커널 파라메터들은
 * argv_init[] 배열에 추가되고 이 배열은 init process에서 사용될 수 있다.
 */
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   set_init_arg);

	jump_label_init();

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	setup_log_buf(0);
	pidhash_init();
	vfs_caches_init_early();
	sort_main_extable();
	trap_init();
	mm_init();

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	preempt_disable();
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	idr_init_cache();
	rcu_init();

	/* trace_printk() and trace points may be used after this */
	trace_init();

	context_tracking_init();
	radix_tree_init();
	/* init some links before init_ISA_irqs() */
	early_irq_init();
	init_IRQ();
	tick_init();
	rcu_init_nohz();
	init_timers();
	hrtimers_init();
	softirq_init();
	timekeeping_init();
	time_init();
	sched_clock_postinit();
	perf_event_init();
	profile_init();
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");
	early_boot_irqs_disabled = false;
	local_irq_enable();

	kmem_cache_init_late();

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	console_init();
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	lockdep_info();

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	page_ext_init();
	debug_objects_mem_init();
	kmemleak_init();
	setup_per_cpu_pageset();
	numa_policy_init();
	if (late_time_init)
		late_time_init();
	sched_clock_init();
	calibrate_delay();
	pidmap_init();
	anon_vma_init();
	acpi_early_init();
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
#ifdef CONFIG_X86_ESPFIX64
	/* Should be run before the first non-init thread is created */
	init_espfix_bsp();
#endif
	thread_info_cache_init();
	cred_init();
	fork_init(totalram_pages);
	proc_caches_init();
	buffer_init();
	key_init();
	security_init();
	dbg_late_init();
	vfs_caches_init(totalram_pages);
	signals_init();
	/* rootfs populating might need page-writeback */
	page_writeback_init();
	proc_root_init();
	nsfs_init();
	cgroup_init();
	cpuset_init();
	taskstats_init_early();
	delayacct_init();

	check_bugs();

	sfi_init_late();

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_late_init();
		efi_free_boot_services();
	}

	ftrace_init();

	/* Do the rest non-__init'ed, we're now alive */
	rest_init();
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{

/* IAMROOT-12:
 * -------------
 * CONFIG_CONSTRUCTORS 커널 설정이 동작되는 경우 커널 모듈의 constructor를
 * 호출한다. (gcc가 만드는 constructor)
 */
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = alloc_bootmem(sizeof(*entry));
			entry->buf = alloc_bootmem(strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct list_head *tmp;
	struct blacklist_entry *entry;
	char *fn_name;

	fn_name = kasprintf(GFP_KERNEL, "%pf", fn);
	if (!fn_name)
		return false;

	list_for_each(tmp, &blacklisted_initcalls) {
		entry = list_entry(tmp, struct blacklist_entry, next);
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			kfree(fn_name);
			return true;
		}
	}

	kfree(fn_name);
	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	ktime_t calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	printk(KERN_DEBUG "calling  %pF @ %i\n", fn, task_pid_nr(current));
	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	printk(KERN_DEBUG "initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;
	char msgbuf[64];

	if (initcall_blacklisted(fn))
		return -EPERM;


/* IAMROOT-12:
 * -------------
 * "initcall_debug" 커널 파라메터가 설정된 경우 호출하는 함수 정보를 출력한다.
 */
	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	initcall_t *fn;

	strcpy(initcall_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   initcall_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   &repair_env_string);

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(*fn);
}

static void __init do_initcalls(void)
{
	int level;

/* IAMROOT-12:
 * -------------
 * 0레벨 부터 7레벨 까지로 등록한 함수들을 차례로 호출한다.
 *
 * pure_initcall(fn)		- 0레벨 
 * core_initcall(fn)            - 1레벨   
 * core_initcall_sync(fn)          
 * postcore_initcall(fn)           
 * postcore_initcall_sync(fn)      
 * arch_initcall(fn)               
 * arch_initcall_sync(fn)          
 * subsys_initcall(fn)             
 * subsys_initcall_sync(fn)        
 * fs_initcall(fn)                 
 * fs_initcall_sync(fn)            
 * rootfs_initcall(fn)             
 * device_initcall(fn)             
 * device_initcall_sync(fn)        
 * late_initcall(fn)		- 7레벨          
 * late_initcall_sync(fn)       - "   
 */
	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();

/* IAMROOT-12:
 * -------------
 * khelper 워크큐를 준비한다.
 */

	usermodehelper_init();
	shmem_init();
	driver_init();
	init_irq_proc();

/* IAMROOT-12:
 * -------------
 * 각 커널 모듈의 constructor를 호출한다.
 */
	do_ctors();
	usermodehelper_enable();

/* IAMROOT-12:
 * -------------
 * 0레벨부터 7레벨까지의 *_initcall 함수들을 차례로 호출한다.
 */
	do_initcalls();
	random_int_secret_init();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_t *fn;

/* IAMROOT-12:
 * -------------
 * early_initcall() 매크로로 만들어진 함수들을 호출한다.
 *	(.initcall.init 섹션에 등록된다.)
 *
 *	- cpu_stop_init()
 *	- init_events()
 *	- init_workqueues()
 *	- migration_init()
 *	- jump_label_init_module()
 *	- relay_init()
 *	- check_cpu_stall_init()
 *	- rcu_register_oom_notifier()
 *	- rcu_spawn_gp_kthread()
 *	- spawn_ksoftirqd()
 *	- cpu_suspend_alloc_sp() - arm
 *	- init_static_idmap() - arm
 *	- dynamic_debug_init()
 *	- rand_initialize()
 *	- dummy_timer_register()
 *	- cci_init() - arm
 */
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(*fn);
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(getname_kernel(init_filename),
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

static int __ref kernel_init(void *unused)
{
	int ret;

/* IAMROOT-12:
 * -------------
 * smp 관련 초기화 함수, early_initcall, 0~7레벨의 initcall 함수들을 호출한다.
 */
	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();

/* IAMROOT-12:
 * -------------
 * 더 이상 사용하지 않는 init 섹션의 커널 코드와 데이터를 제거한다.
 */
	free_initmem();

/* IAMROOT-12:
 * -------------
 * CONFIG_DEBUG_RODATA 커널 옵션을 사용시 사용한다.
 */
	mark_rodata_ro();

/* IAMROOT-12:
 * -------------
 * 부팅 상태 변경
 */
	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	flush_delayed_fput();

/* IAMROOT-12:
 * -------------
 * 1) "rdinit=" 지정된 문자열이 있는 경우
 */
	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */

/* IAMROOT-12:
 * -------------
 * 2) "init=" 지정된 문자열이 있는 경우
 */

	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

/* IAMROOT-12:
 * -------------
 * 3) 그 외 아래 디렉토리 순서대로 init 태스크를 호출한다.
 */
	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */

/* IAMROOT-12:
 * -------------
 * rest_init()이 끝난 후 진입하도록 동기화한다.
 */
	wait_for_completion(&kthreadd_done);

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */

/* IAMROOT-12:
 * -------------
 * kthread_init이 사용할 수 있는 노드를 모든 메모리 노드로 설정한다.
 */
	set_mems_allowed(node_states[N_MEMORY]);
	/*
	 * init can run on any cpu.
	 */

/* IAMROOT-12:
 * -------------
 * kthread_init이 모든 cpu에서 동작할 수 있도록 설정한다.
 */
	set_cpus_allowed_ptr(current, cpu_all_mask);

	cad_pid = task_pid(current);


/* IAMROOT-12:
 * -------------
 * "maxcpus=" 커널 파라메터로 제한된 cpu 수 
 *	- 최초 커널 컴파일 시에는 NR_CPUS 값을 사용한다.
 */
	smp_prepare_cpus(setup_max_cpus);

/* IAMROOT-12:
 * -------------
 * early_initcall() 매크로로 등록한 함수들을 모두 호출한다.
 */
	do_pre_smp_initcalls();
	lockup_detector_init();

	smp_init();

/* IAMROOT-12:
 * -------------
 * 스케줄러와 관련된 smp 초기화
 */
	sched_init_smp();

/* IAMROOT-12:
 * -------------
 * 0레벨부터 7레벨까지의 *_initcall 함수들을 차례로 호출한다.
 */
	do_basic_setup();

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */

	integrity_load_keys();
	load_default_modules();
}
