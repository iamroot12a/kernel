#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/page_ext.h>
#include <linux/poison.h>
#include <linux/ratelimit.h>

static bool page_poisoning_enabled __read_mostly;

static bool need_page_poisoning(void)
{
/* IAMROOT-12AB:
 * -------------
 * _debug_pagealloc_enabled 상태를 반환한다.
 * 커널 파라메터 "debug_pagealloc=on"을 사용하여 커널 컴파일 없이 디버깅을 할 수 있다.
 * (CONFIG_DEBUG_PAGEALLOC 커널 옵션을 미리 사용하여 컴파일 되어 있어야 한다.)
 */
	if (!debug_pagealloc_enabled())
		return false;

	return true;
}

static void init_page_poisoning(void)
{
	if (!debug_pagealloc_enabled())
		return;

/* IAMROOT-12AB:
 * -------------
 * page_poisoning_enabled를 true로 설정하여 추후 페이지 할당자에서 
 * poison을 이용한 디버깅을 할 수 있도록 한다.
 */
	page_poisoning_enabled = true;
}

struct page_ext_operations page_poisoning_ops = {
	.need = need_page_poisoning,
	.init = init_page_poisoning,
};

static inline void set_page_poison(struct page *page)
{
	struct page_ext *page_ext;

/* IAMROOT-12AB:
 * -------------
 * page_ext->flags의 PAGE_EXT_DEBUG_POISON 플래그를 설정하여 
 * 해당 페이지가 poison 디버깅중임을 알린다.
 */
	page_ext = lookup_page_ext(page);
	__set_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
}

static inline void clear_page_poison(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
	__clear_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
}

static inline bool page_poison(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
	return test_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
}

static void poison_page(struct page *page)
{

/* IAMROOT-12AB:
 * -------------
 * 커널에서 특정 물리메모리 페이지를 직접 사용할 경우 kmap_atomic() 함수를 
 * 사용하면 lowmem에 대해서는 매핑없이(이미 1:1 매핑이 되어 있으므로)
 * 접근하고, highmem에 대해서는 fixmap을 이용하여 매핑 후 접근할 수 있도록 한다.
 */
	void *addr = kmap_atomic(page);

/* IAMROOT-12AB:
 * -------------
 * page_ext->flags의 PAGE_EXT_DEBUG_POISON 플래그를 설정하여 
 * 해당 페이지가 poison 디버깅중임을 알리고 해당 페이지를 
 * PAGE_POISON(0xaa)로 채운다.
 */
	set_page_poison(page);
	memset(addr, PAGE_POISON, PAGE_SIZE);
	kunmap_atomic(addr);
}

static void poison_pages(struct page *page, int n)
{
	int i;

	for (i = 0; i < n; i++)
		poison_page(page + i);
}

/* IAMROOT-12AB:
 * -------------
 * 비트를 비교하여 1비트만 다른 경우 true를 반환한다.
 */
static bool single_bit_flip(unsigned char a, unsigned char b)
{
	unsigned char error = a ^ b;

	return error && !(error & (error - 1));
}

static void check_poison_mem(unsigned char *mem, size_t bytes)
{
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 10);
	unsigned char *start;
	unsigned char *end;

/* IAMROOT-12AB:
 * -------------
 * 지정된 주소부터 bytes 만큼 c가 아닌 값이 있는지 찾아 그 주소를 반환한다.
 * 정상적으로 모든 값이 c가 있는 경우 함수를 빠져나간다.
 */
	start = memchr_inv(mem, PAGE_POISON, bytes);
	if (!start)
		return;

	for (end = mem + bytes - 1; end > start; end--) {
		if (*end != PAGE_POISON)
			break;
	}

/* IAMROOT-12AB:
 * -------------
 * ratelimit(5초에 10번)을 초과하는 경우 함수를 빠져나간다.
 */
	if (!__ratelimit(&ratelimit))
		return;

/* IAMROOT-12AB:
 * -------------
 * 1비트만 다른 경우와 그 이상의 비트가 오류가 있는지에 따라 구분하여 출력한다.
 */
	else if (start == end && single_bit_flip(*start, PAGE_POISON))
		printk(KERN_ERR "pagealloc: single bit error\n");
	else
		printk(KERN_ERR "pagealloc: memory corruption\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 16, 1, start,
			end - start + 1, 1);
	dump_stack();
}

static void unpoison_page(struct page *page)
{
	void *addr;

	if (!page_poison(page))
		return;

/* IAMROOT-12AB:
 * -------------
 * 페이지 프레임에서 POISON 데이터를 확인하여 이상이 있는 경우 메시지를 
 * 출력하여 경고한다. 그 후 page_ext->flags의 PAGE_EXT_DEBUG_POISON 플래그를 
 * clear한다.
 */
	addr = kmap_atomic(page);
	check_poison_mem(addr, PAGE_SIZE);
	clear_page_poison(page);
	kunmap_atomic(addr);
}

static void unpoison_pages(struct page *page, int n)
{
	int i;

	for (i = 0; i < n; i++)
		unpoison_page(page + i);
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!page_poisoning_enabled)
		return;

/* IAMROOT-12AB:
 * -------------
 *  CONFIG_PAGE_POISONING 커널 옵션과 "debug_pagealloc=on" 커널 파라메터가 
 *  설정된 경우 아래 페이지 poison 관련 디버깅을 수행할 수 있다.
 *
 *  enable이 false일 때 페이지에 poison(0xaa)를 기록하고,
 *  enable이 true일 때 페이지에 기록된 poison(0xaa)이 변경된 경우 경고 메시지를 
 *  출력하여 알린다. 
 */
	if (enable)
		unpoison_pages(page, numpages);
	else
		poison_pages(page, numpages);
}
