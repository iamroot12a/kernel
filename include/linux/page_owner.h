#ifndef __LINUX_PAGE_OWNER_H
#define __LINUX_PAGE_OWNER_H

#ifdef CONFIG_PAGE_OWNER
extern bool page_owner_inited;
extern struct page_ext_operations page_owner_ops;

extern void __reset_page_owner(struct page *page, unsigned int order);
extern void __set_page_owner(struct page *page,
			unsigned int order, gfp_t gfp_mask);

static inline void reset_page_owner(struct page *page, unsigned int order)
{

/* IAMROOT-12AB:
 * -------------
 * "page_owner=on"을 사용하는 경우 page_ext 구조를 사용하는데 owner를 clear한다.
 *
 * 참고: Sparse 메모리 모델을 사용하는 경우 page_ext_init() 함수가 호출된 후에 
 * 아래 page_owner_inited가 설정되어 아래 reset을 진행할 수 있다.
 */
	if (likely(!page_owner_inited))
		return;

	__reset_page_owner(page, order);
}

static inline void set_page_owner(struct page *page,
			unsigned int order, gfp_t gfp_mask)
{
	if (likely(!page_owner_inited))
		return;

	__set_page_owner(page, order, gfp_mask);
}
#else
static inline void reset_page_owner(struct page *page, unsigned int order)
{
}
static inline void set_page_owner(struct page *page,
			unsigned int order, gfp_t gfp_mask)
{
}

#endif /* CONFIG_PAGE_OWNER */
#endif /* __LINUX_PAGE_OWNER_H */
