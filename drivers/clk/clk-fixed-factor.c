/*
 * Copyright (C) 2011 Sascha Hauer, Pengutronix <s.hauer@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Standard functionality for the common clock API.
 */
#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>

/*
 * DOC: basic fixed multiplier and divider clock that cannot gate
 *
 * Traits of this clock:
 * prepare - clk_prepare only ensures that parents are prepared
 * enable - clk_enable only ensures that parents are enabled
 * rate - rate is fixed.  clk->rate = parent->rate / div * mult
 * parent - fixed parent.  No clk_set_parent support
 */

#define to_clk_fixed_factor(_hw) container_of(_hw, struct clk_fixed_factor, hw)

static unsigned long clk_factor_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{

/* IAMROOT-12:
 * -------------
 * clk_fixed_factor 구조체 내부에 hw가 있는데 이 주소를 사용하여 
 * clk_fixed_factor 구조체 주소를 알아온다.
 */
	struct clk_fixed_factor *fix = to_clk_fixed_factor(hw);
	unsigned long long int rate;

/* IAMROOT-12:
 * -------------
 * 인수로 받은 parent_rate * mult / div 값을 반환한다.
 * (예: parent_rate(80Mhz) * mult(1) / div(8) = 10Mhz를 반환
 */
	rate = (unsigned long long int)parent_rate * fix->mult;
	do_div(rate, fix->div);
	return (unsigned long)rate;
}

static long clk_factor_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct clk_fixed_factor *fix = to_clk_fixed_factor(hw);

	if (__clk_get_flags(hw->clk) & CLK_SET_RATE_PARENT) {
		unsigned long best_parent;

		best_parent = (rate / fix->mult) * fix->div;
		*prate = __clk_round_rate(__clk_get_parent(hw->clk),
				best_parent);
	}

	return (*prate / fix->div) * fix->mult;
}

static int clk_factor_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	return 0;
}

struct clk_ops clk_fixed_factor_ops = {
	.round_rate = clk_factor_round_rate,
	.set_rate = clk_factor_set_rate,
	.recalc_rate = clk_factor_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_fixed_factor_ops);

struct clk *clk_register_fixed_factor(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	struct clk_fixed_factor *fix;
	struct clk_init_data init;
	struct clk *clk;

	fix = kmalloc(sizeof(*fix), GFP_KERNEL);
	if (!fix) {
		pr_err("%s: could not allocate fixed factor clk\n", __func__);
		return ERR_PTR(-ENOMEM);
	}

	/* struct clk_fixed_factor assignments */
	fix->mult = mult;
	fix->div = div;
	fix->hw.init = &init;

/* IAMROOT-12:
 * -------------
 * local 용도의 구조체로 클럭이 등록된 후 버린다.
 */
	init.name = name;
	init.ops = &clk_fixed_factor_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = &parent_name;

/* IAMROOT-12:
 * -------------
 * 부모 클럭은 무조건 1개 (mux 클럭인 경우에만 n개)
 */
	init.num_parents = 1;

	clk = clk_register(dev, &fix->hw);

	if (IS_ERR(clk))
		kfree(fix);

	return clk;
}
EXPORT_SYMBOL_GPL(clk_register_fixed_factor);

#ifdef CONFIG_OF
/**
 * of_fixed_factor_clk_setup() - Setup function for simple fixed factor clock
 */
void __init of_fixed_factor_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent_name;
	u32 div, mult;

/* IAMROOT-12:
 * -------------
 * "clock-div" 및 "clock-mult" 속성 값을 읽어온다.
 * (divider 값, multiplier 값)
 */
	if (of_property_read_u32(node, "clock-div", &div)) {
		pr_err("%s Fixed factor clock <%s> must have a clock-div property\n",
			__func__, node->name);
		return;
	}

	if (of_property_read_u32(node, "clock-mult", &mult)) {
		pr_err("%s Fixed factor clock <%s> must have a clock-mult property\n",
			__func__, node->name);
		return;
	}


/* IAMROOT-12:
 * -------------
 * "clock-outpu-names" 속성 값(클럭명)을 읽어온다.
 */
	of_property_read_string(node, "clock-output-names", &clk_name);

/* IAMROOT-12:
 * -------------
 * 부모 인덱스는 0으로 부모클럭명을 알아온다.
 * (부모 클럭이 멀티 클럭인 경우 처음 것을 사용한다.)
 */
	parent_name = of_clk_get_parent_name(node, 0);

/* IAMROOT-12:
 * -------------
 * fixed-factor형 클럭을 등록한다.
 */
	clk = clk_register_fixed_factor(NULL, clk_name, parent_name, 0,
					mult, div);

/* IAMROOT-12:
 * -------------
 * 디바이스 트리에서 이 클럭의 parsing은 simple_get을 사용한다.
 * -of_clk_src_simple_get() 함수는 parsing 없이 그대로 인덱스를 전달
 */
	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
}
EXPORT_SYMBOL_GPL(of_fixed_factor_clk_setup);
CLK_OF_DECLARE(fixed_factor_clk, "fixed-factor-clock",
		of_fixed_factor_clk_setup);
#endif
