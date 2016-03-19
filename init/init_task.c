#include <linux/init_task.h>
#include <linux/export.h>
#include <linux/mqueue.h>
#include <linux/sched.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/rt.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>

#include <asm/pgtable.h>
#include <asm/uaccess.h>

static struct signal_struct init_signals = INIT_SIGNALS(init_signals);
static struct sighand_struct init_sighand = INIT_SIGHAND(init_sighand);

/* Initial task structure */

/* IAMROOT-12AB:
 * -------------
 * 커널이 초기화될 때 처음 사용하는 태스크 정보를 담은 구조체
 * 이 태스크는 swapper라는 이름을 가지고 있는데 옛날에 page table을
 * 교체하는 작업을 수행하여 왔기 때문에 이 이름을 사용하고 있지만
 * 지금은 커널 셋업이 된 후에 영원히 sleep 된다.
 */
struct task_struct init_task = INIT_TASK(init_task);
EXPORT_SYMBOL(init_task);

/*
 * Initial thread structure. Alignment of this is handled by a special
 * linker map entry.
 */

/* IAMROOT-12AB:
 * -------------
 * init_task용 커널 스택
 */
union thread_union init_thread_union __init_task_data =
	{ INIT_THREAD_INFO(init_task) };
