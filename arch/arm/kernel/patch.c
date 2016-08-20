#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/stop_machine.h>

#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm/smp_plat.h>
#include <asm/opcodes.h>
#include <asm/patch.h>

struct patch {
	void *addr;
	unsigned int insn;
};

static DEFINE_SPINLOCK(patch_lock);

static void __kprobes *patch_map(void *addr, int fixmap, unsigned long *flags)
	__acquires(&patch_lock)
{
	unsigned int uintaddr = (uintptr_t) addr;

/* IAMROOT-12AB:
 * -------------
 * core 커널 text 영역이 아닌 경우 module 커널 text 영역이라고 인식한다.
 */
	bool module = !core_kernel_text(uintaddr);
	struct page *page;

	if (module && IS_ENABLED(CONFIG_DEBUG_SET_MODULE_RONX))
		page = vmalloc_to_page(addr);
	else if (!module && IS_ENABLED(CONFIG_DEBUG_RODATA))
		page = virt_to_page(addr);
	else
		return addr;

	if (flags)
		spin_lock_irqsave(&patch_lock, *flags);
	else
		__acquire(&patch_lock);

/* IAMROOT-12AB:
 * -------------
 * fixmap(fixmap address space 범위에 위치한 fixmap 인덱스)에 물리주소를 매핑한다.
 */
	set_fixmap(fixmap, page_to_phys(page));

	return (void *) (__fix_to_virt(fixmap) + (uintaddr & ~PAGE_MASK));
}

static void __kprobes patch_unmap(int fixmap, unsigned long *flags)
	__releases(&patch_lock)
{
	clear_fixmap(fixmap);

	if (flags)
		spin_unlock_irqrestore(&patch_lock, *flags);
	else
		__release(&patch_lock);
}

void __kprobes __patch_text_real(void *addr, unsigned int insn, bool remap)
{
	bool thumb2 = IS_ENABLED(CONFIG_THUMB2_KERNEL);
	unsigned int uintaddr = (uintptr_t) addr;
	bool twopage = false;
	unsigned long flags;
	void *waddr = addr;
	int size;

	if (remap)

/* IAMROOT-12AB:
 * -------------
 * 주어진 addr이 있는 페이지를 fixmap의 FIX_TEXT_POKE0 인덱스 페이지에 해당하는 주소에 매핑한다.
 */
		waddr = patch_map(addr, FIX_TEXT_POKE0, &flags);
	else
		__acquire(&patch_lock);

	if (thumb2 && __opcode_is_thumb16(insn)) {
		*(u16 *)waddr = __opcode_to_mem_thumb16(insn);
		size = sizeof(u16);
	} else if (thumb2 && (uintaddr & 2)) {
		u16 first = __opcode_thumb32_first(insn);
		u16 second = __opcode_thumb32_second(insn);
		u16 *addrh0 = waddr;
		u16 *addrh1 = waddr + 2;

		twopage = (uintaddr & ~PAGE_MASK) == PAGE_SIZE - 2;
		if (twopage && remap)
			addrh1 = patch_map(addr + 2, FIX_TEXT_POKE1, NULL);

		*addrh0 = __opcode_to_mem_thumb16(first);
		*addrh1 = __opcode_to_mem_thumb16(second);

		if (twopage && addrh1 != addr + 2) {
			flush_kernel_vmap_range(addrh1, 2);
			patch_unmap(FIX_TEXT_POKE1, NULL);
		}

		size = sizeof(u32);
	} else {

/* IAMROOT-12AB:
 * -------------
 * 엔디안 변환에 따른 저장할 인스트럭션으로 변환 
 * rpi2: 변환할 필요 없음
 */
		if (thumb2)
			insn = __opcode_to_mem_thumb32(insn);
		else
			insn = __opcode_to_mem_arm(insn);

		*(u32 *)waddr = insn;
		size = sizeof(u32);
	}

/* IAMROOT-12AB:
 * -------------
 * remap한 경우 항상 waddr과 addr은 다르다.
 * 또한 remap을 안하더라도 thumb쪽에서 waddr이 바뀔 수 있다.
 */
	if (waddr != addr) {
		flush_kernel_vmap_range(waddr, twopage ? size / 2 : size);
		patch_unmap(FIX_TEXT_POKE0, &flags);
	} else
		__release(&patch_lock);

	flush_icache_range((uintptr_t)(addr),
			   (uintptr_t)(addr) + size);
}

static int __kprobes patch_text_stop_machine(void *data)
{
	struct patch *patch = data;

	__patch_text(patch->addr, patch->insn);

	return 0;
}

void __kprobes patch_text(void *addr, unsigned int insn)
{
	struct patch patch = {
		.addr = addr,
		.insn = insn,
	};

	stop_machine(patch_text_stop_machine, &patch, NULL);
}
