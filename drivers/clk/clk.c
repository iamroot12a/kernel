/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Linaro Ltd <mturquette@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Standard functionality for the common clock API.  See Documentation/clk.txt
 */

#include <linux/clk-provider.h>
#include <linux/clk/clk-conf.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/sched.h>

#include "clk.h"

static DEFINE_SPINLOCK(enable_lock);
static DEFINE_MUTEX(prepare_lock);

static struct task_struct *prepare_owner;
static struct task_struct *enable_owner;

static int prepare_refcnt;
static int enable_refcnt;

static HLIST_HEAD(clk_root_list);
static HLIST_HEAD(clk_orphan_list);
static LIST_HEAD(clk_notifier_list);

static long clk_core_get_accuracy(struct clk_core *clk);
static unsigned long clk_core_get_rate(struct clk_core *clk);
static int clk_core_get_phase(struct clk_core *clk);
static bool clk_core_is_prepared(struct clk_core *clk);
static bool clk_core_is_enabled(struct clk_core *clk);
static struct clk_core *clk_core_lookup(const char *name);

/***    private data structures    ***/

struct clk_core {
	const char		*name;
	const struct clk_ops	*ops;
	struct clk_hw		*hw;
	struct module		*owner;
	struct clk_core		*parent;
	const char		**parent_names;
	struct clk_core		**parents;
	u8			num_parents;
	u8			new_parent_index;
	unsigned long		rate;
	unsigned long		req_rate;
	unsigned long		new_rate;
	struct clk_core		*new_parent;
	struct clk_core		*new_child;
	unsigned long		flags;
	unsigned int		enable_count;
	unsigned int		prepare_count;
	unsigned long		accuracy;
	int			phase;
	struct hlist_head	children;
	struct hlist_node	child_node;
	struct hlist_node	debug_node;
	struct hlist_head	clks;
	unsigned int		notifier_count;
#ifdef CONFIG_DEBUG_FS
	struct dentry		*dentry;
#endif
	struct kref		ref;
};

/* IAMROOT-12:
 * -------------
 * clk 구조체에는 가장 중요한 클럭 정보를 담은 clk_core를 가리키고 
 * child_node들을 담고 있다.
 */
struct clk {
	struct clk_core	*core;
	const char *dev_id;
	const char *con_id;
	unsigned long min_rate;
	unsigned long max_rate;
	struct hlist_node child_node;
};

/***           locking             ***/
static void clk_prepare_lock(void)
{
	if (!mutex_trylock(&prepare_lock)) {
		if (prepare_owner == current) {
			prepare_refcnt++;
			return;
		}
		mutex_lock(&prepare_lock);
	}
	WARN_ON_ONCE(prepare_owner != NULL);
	WARN_ON_ONCE(prepare_refcnt != 0);
	prepare_owner = current;
	prepare_refcnt = 1;
}

static void clk_prepare_unlock(void)
{
	WARN_ON_ONCE(prepare_owner != current);
	WARN_ON_ONCE(prepare_refcnt == 0);

	if (--prepare_refcnt)
		return;
	prepare_owner = NULL;
	mutex_unlock(&prepare_lock);
}

static unsigned long clk_enable_lock(void)
{
	unsigned long flags;

	if (!spin_trylock_irqsave(&enable_lock, flags)) {
		if (enable_owner == current) {
			enable_refcnt++;
			return flags;
		}
		spin_lock_irqsave(&enable_lock, flags);
	}
	WARN_ON_ONCE(enable_owner != NULL);
	WARN_ON_ONCE(enable_refcnt != 0);
	enable_owner = current;
	enable_refcnt = 1;
	return flags;
}

static void clk_enable_unlock(unsigned long flags)
{
	WARN_ON_ONCE(enable_owner != current);
	WARN_ON_ONCE(enable_refcnt == 0);

	if (--enable_refcnt)
		return;
	enable_owner = NULL;
	spin_unlock_irqrestore(&enable_lock, flags);
}

/***        debugfs support        ***/

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *rootdir;
static int inited = 0;
static DEFINE_MUTEX(clk_debug_lock);
static HLIST_HEAD(clk_debug_list);

static struct hlist_head *all_lists[] = {
	&clk_root_list,
	&clk_orphan_list,
	NULL,
};

static struct hlist_head *orphan_list[] = {
	&clk_orphan_list,
	NULL,
};

static void clk_summary_show_one(struct seq_file *s, struct clk_core *c,
				 int level)
{
	if (!c)
		return;

	seq_printf(s, "%*s%-*s %11d %12d %11lu %10lu %-3d\n",
		   level * 3 + 1, "",
		   30 - level * 3, c->name,
		   c->enable_count, c->prepare_count, clk_core_get_rate(c),
		   clk_core_get_accuracy(c), clk_core_get_phase(c));
}

static void clk_summary_show_subtree(struct seq_file *s, struct clk_core *c,
				     int level)
{
	struct clk_core *child;

	if (!c)
		return;

	clk_summary_show_one(s, c, level);

	hlist_for_each_entry(child, &c->children, child_node)
		clk_summary_show_subtree(s, child, level + 1);
}

static int clk_summary_show(struct seq_file *s, void *data)
{
	struct clk_core *c;
	struct hlist_head **lists = (struct hlist_head **)s->private;

	seq_puts(s, "   clock                         enable_cnt  prepare_cnt        rate   accuracy   phase\n");
	seq_puts(s, "----------------------------------------------------------------------------------------\n");

	clk_prepare_lock();

	for (; *lists; lists++)
		hlist_for_each_entry(c, *lists, child_node)
			clk_summary_show_subtree(s, c, 0);

	clk_prepare_unlock();

	return 0;
}


static int clk_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, clk_summary_show, inode->i_private);
}

static const struct file_operations clk_summary_fops = {
	.open		= clk_summary_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void clk_dump_one(struct seq_file *s, struct clk_core *c, int level)
{
	if (!c)
		return;

	seq_printf(s, "\"%s\": { ", c->name);
	seq_printf(s, "\"enable_count\": %d,", c->enable_count);
	seq_printf(s, "\"prepare_count\": %d,", c->prepare_count);
	seq_printf(s, "\"rate\": %lu", clk_core_get_rate(c));
	seq_printf(s, "\"accuracy\": %lu", clk_core_get_accuracy(c));
	seq_printf(s, "\"phase\": %d", clk_core_get_phase(c));
}

static void clk_dump_subtree(struct seq_file *s, struct clk_core *c, int level)
{
	struct clk_core *child;

	if (!c)
		return;

	clk_dump_one(s, c, level);

	hlist_for_each_entry(child, &c->children, child_node) {
		seq_printf(s, ",");
		clk_dump_subtree(s, child, level + 1);
	}

	seq_printf(s, "}");
}

static int clk_dump(struct seq_file *s, void *data)
{
	struct clk_core *c;
	bool first_node = true;
	struct hlist_head **lists = (struct hlist_head **)s->private;

	seq_printf(s, "{");

	clk_prepare_lock();

	for (; *lists; lists++) {
		hlist_for_each_entry(c, *lists, child_node) {
			if (!first_node)
				seq_puts(s, ",");
			first_node = false;
			clk_dump_subtree(s, c, 0);
		}
	}

	clk_prepare_unlock();

	seq_printf(s, "}");
	return 0;
}


static int clk_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, clk_dump, inode->i_private);
}

static const struct file_operations clk_dump_fops = {
	.open		= clk_dump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int clk_debug_create_one(struct clk_core *clk, struct dentry *pdentry)
{
	struct dentry *d;
	int ret = -ENOMEM;

	if (!clk || !pdentry) {
		ret = -EINVAL;
		goto out;
	}

	d = debugfs_create_dir(clk->name, pdentry);
	if (!d)
		goto out;

	clk->dentry = d;

	d = debugfs_create_u32("clk_rate", S_IRUGO, clk->dentry,
			(u32 *)&clk->rate);
	if (!d)
		goto err_out;

	d = debugfs_create_u32("clk_accuracy", S_IRUGO, clk->dentry,
			(u32 *)&clk->accuracy);
	if (!d)
		goto err_out;

	d = debugfs_create_u32("clk_phase", S_IRUGO, clk->dentry,
			(u32 *)&clk->phase);
	if (!d)
		goto err_out;

	d = debugfs_create_x32("clk_flags", S_IRUGO, clk->dentry,
			(u32 *)&clk->flags);
	if (!d)
		goto err_out;

	d = debugfs_create_u32("clk_prepare_count", S_IRUGO, clk->dentry,
			(u32 *)&clk->prepare_count);
	if (!d)
		goto err_out;

	d = debugfs_create_u32("clk_enable_count", S_IRUGO, clk->dentry,
			(u32 *)&clk->enable_count);
	if (!d)
		goto err_out;

	d = debugfs_create_u32("clk_notifier_count", S_IRUGO, clk->dentry,
			(u32 *)&clk->notifier_count);
	if (!d)
		goto err_out;

	if (clk->ops->debug_init) {
		ret = clk->ops->debug_init(clk->hw, clk->dentry);
		if (ret)
			goto err_out;
	}

	ret = 0;
	goto out;

err_out:
	debugfs_remove_recursive(clk->dentry);
	clk->dentry = NULL;
out:
	return ret;
}

/**
 * clk_debug_register - add a clk node to the debugfs clk tree
 * @clk: the clk being added to the debugfs clk tree
 *
 * Dynamically adds a clk to the debugfs clk tree if debugfs has been
 * initialized.  Otherwise it bails out early since the debugfs clk tree
 * will be created lazily by clk_debug_init as part of a late_initcall.
 */
static int clk_debug_register(struct clk_core *clk)
{
	int ret = 0;

	mutex_lock(&clk_debug_lock);
	hlist_add_head(&clk->debug_node, &clk_debug_list);

	if (!inited)
		goto unlock;

	ret = clk_debug_create_one(clk, rootdir);
unlock:
	mutex_unlock(&clk_debug_lock);

	return ret;
}

 /**
 * clk_debug_unregister - remove a clk node from the debugfs clk tree
 * @clk: the clk being removed from the debugfs clk tree
 *
 * Dynamically removes a clk and all it's children clk nodes from the
 * debugfs clk tree if clk->dentry points to debugfs created by
 * clk_debug_register in __clk_init.
 */
static void clk_debug_unregister(struct clk_core *clk)
{
	mutex_lock(&clk_debug_lock);
	hlist_del_init(&clk->debug_node);
	debugfs_remove_recursive(clk->dentry);
	clk->dentry = NULL;
	mutex_unlock(&clk_debug_lock);
}

struct dentry *clk_debugfs_add_file(struct clk_hw *hw, char *name, umode_t mode,
				void *data, const struct file_operations *fops)
{
	struct dentry *d = NULL;

	if (hw->core->dentry)
		d = debugfs_create_file(name, mode, hw->core->dentry, data,
					fops);

	return d;
}
EXPORT_SYMBOL_GPL(clk_debugfs_add_file);

/**
 * clk_debug_init - lazily create the debugfs clk tree visualization
 *
 * clks are often initialized very early during boot before memory can
 * be dynamically allocated and well before debugfs is setup.
 * clk_debug_init walks the clk tree hierarchy while holding
 * prepare_lock and creates the topology as part of a late_initcall,
 * thus insuring that clks initialized very early will still be
 * represented in the debugfs clk tree.  This function should only be
 * called once at boot-time, and all other clks added dynamically will
 * be done so with clk_debug_register.
 */
static int __init clk_debug_init(void)
{
	struct clk_core *clk;
	struct dentry *d;

	rootdir = debugfs_create_dir("clk", NULL);

	if (!rootdir)
		return -ENOMEM;

	d = debugfs_create_file("clk_summary", S_IRUGO, rootdir, &all_lists,
				&clk_summary_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("clk_dump", S_IRUGO, rootdir, &all_lists,
				&clk_dump_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("clk_orphan_summary", S_IRUGO, rootdir,
				&orphan_list, &clk_summary_fops);
	if (!d)
		return -ENOMEM;

	d = debugfs_create_file("clk_orphan_dump", S_IRUGO, rootdir,
				&orphan_list, &clk_dump_fops);
	if (!d)
		return -ENOMEM;

	mutex_lock(&clk_debug_lock);
	hlist_for_each_entry(clk, &clk_debug_list, debug_node)
		clk_debug_create_one(clk, rootdir);

	inited = 1;
	mutex_unlock(&clk_debug_lock);

	return 0;
}
late_initcall(clk_debug_init);
#else
static inline int clk_debug_register(struct clk_core *clk) { return 0; }
static inline void clk_debug_reparent(struct clk_core *clk,
				      struct clk_core *new_parent)
{
}
static inline void clk_debug_unregister(struct clk_core *clk)
{
}
#endif

/* caller must hold prepare_lock */
static void clk_unprepare_unused_subtree(struct clk_core *clk)
{
	struct clk_core *child;

	hlist_for_each_entry(child, &clk->children, child_node)
		clk_unprepare_unused_subtree(child);

	if (clk->prepare_count)
		return;

	if (clk->flags & CLK_IGNORE_UNUSED)
		return;

	if (clk_core_is_prepared(clk)) {
		if (clk->ops->unprepare_unused)
			clk->ops->unprepare_unused(clk->hw);
		else if (clk->ops->unprepare)
			clk->ops->unprepare(clk->hw);
	}
}

/* caller must hold prepare_lock */
static void clk_disable_unused_subtree(struct clk_core *clk)
{
	struct clk_core *child;
	unsigned long flags;

	hlist_for_each_entry(child, &clk->children, child_node)
		clk_disable_unused_subtree(child);

	flags = clk_enable_lock();

	if (clk->enable_count)
		goto unlock_out;

	if (clk->flags & CLK_IGNORE_UNUSED)
		goto unlock_out;

	/*
	 * some gate clocks have special needs during the disable-unused
	 * sequence.  call .disable_unused if available, otherwise fall
	 * back to .disable
	 */
	if (clk_core_is_enabled(clk)) {
		if (clk->ops->disable_unused)
			clk->ops->disable_unused(clk->hw);
		else if (clk->ops->disable)
			clk->ops->disable(clk->hw);
	}

unlock_out:
	clk_enable_unlock(flags);
}

static bool clk_ignore_unused;
static int __init clk_ignore_unused_setup(char *__unused)
{
	clk_ignore_unused = true;
	return 1;
}
__setup("clk_ignore_unused", clk_ignore_unused_setup);

static int clk_disable_unused(void)
{
	struct clk_core *clk;

	if (clk_ignore_unused) {
		pr_warn("clk: Not disabling unused clocks\n");
		return 0;
	}

	clk_prepare_lock();

	hlist_for_each_entry(clk, &clk_root_list, child_node)
		clk_disable_unused_subtree(clk);

	hlist_for_each_entry(clk, &clk_orphan_list, child_node)
		clk_disable_unused_subtree(clk);

	hlist_for_each_entry(clk, &clk_root_list, child_node)
		clk_unprepare_unused_subtree(clk);

	hlist_for_each_entry(clk, &clk_orphan_list, child_node)
		clk_unprepare_unused_subtree(clk);

	clk_prepare_unlock();

	return 0;
}
late_initcall_sync(clk_disable_unused);

/***    helper functions   ***/

const char *__clk_get_name(struct clk *clk)
{
	return !clk ? NULL : clk->core->name;
}
EXPORT_SYMBOL_GPL(__clk_get_name);

struct clk_hw *__clk_get_hw(struct clk *clk)
{
	return !clk ? NULL : clk->core->hw;
}
EXPORT_SYMBOL_GPL(__clk_get_hw);

u8 __clk_get_num_parents(struct clk *clk)
{
	return !clk ? 0 : clk->core->num_parents;
}
EXPORT_SYMBOL_GPL(__clk_get_num_parents);

struct clk *__clk_get_parent(struct clk *clk)
{
	if (!clk)
		return NULL;

	/* TODO: Create a per-user clk and change callers to call clk_put */
	return !clk->core->parent ? NULL : clk->core->parent->hw->clk;
}
EXPORT_SYMBOL_GPL(__clk_get_parent);

static struct clk_core *clk_core_get_parent_by_index(struct clk_core *clk,
							 u8 index)
{

/* IAMROOT-12:
 * -------------
 * 요청한 인덱스가 부모 수보다 많으면 null을 반환한다.
 */
	if (!clk || index >= clk->num_parents)
		return NULL;

/* IAMROOT-12:
 * -------------
 * 부모배열이 만들어지지 않은 경우 인덱스에 해당하는 부모이름으로 검색하여 반환한다
 */
	else if (!clk->parents)
		return clk_core_lookup(clk->parent_names[index]);

/* IAMROOT-12:
 * -------------
 * 해당 인덱스에 대한 부모가 지정되지 않은 경우 해당하는 부모이름으로 검색하여 
 * 해당 인덱스에 저장하고 반환한다.
 */
	else if (!clk->parents[index])
		return clk->parents[index] =
			clk_core_lookup(clk->parent_names[index]);

/* IAMROOT-12:
 * -------------
 * 해당 인덱스에 이미 부모가 지정되어 있는 경우 검색없이 바로 그 클럭을 반환한다
 */
	else
		return clk->parents[index];
}

struct clk *clk_get_parent_by_index(struct clk *clk, u8 index)
{
	struct clk_core *parent;

	if (!clk)
		return NULL;

	parent = clk_core_get_parent_by_index(clk->core, index);

	return !parent ? NULL : parent->hw->clk;
}
EXPORT_SYMBOL_GPL(clk_get_parent_by_index);

unsigned int __clk_get_enable_count(struct clk *clk)
{
	return !clk ? 0 : clk->core->enable_count;
}

static unsigned long clk_core_get_rate_nolock(struct clk_core *clk)
{
	unsigned long ret;

	if (!clk) {
		ret = 0;
		goto out;
	}

/* IAMROOT-12:
 * -------------
 * 루트 클럭이거나 부모 클럭이 있는 경우 현재 클럭의 rate 값을 반환하고,
 * 부모 클럭이 지정되지 않은 경우 0을 반환한다.
 */
	ret = clk->rate;

	if (clk->flags & CLK_IS_ROOT)
		goto out;

	if (!clk->parent)
		ret = 0;

out:
	return ret;
}

unsigned long __clk_get_rate(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_get_rate_nolock(clk->core);
}
EXPORT_SYMBOL_GPL(__clk_get_rate);

static unsigned long __clk_get_accuracy(struct clk_core *clk)
{
	if (!clk)
		return 0;

	return clk->accuracy;
}

unsigned long __clk_get_flags(struct clk *clk)
{
	return !clk ? 0 : clk->core->flags;
}
EXPORT_SYMBOL_GPL(__clk_get_flags);

static bool clk_core_is_prepared(struct clk_core *clk)
{
	int ret;

	if (!clk)
		return false;

	/*
	 * .is_prepared is optional for clocks that can prepare
	 * fall back to software usage counter if it is missing
	 */
	if (!clk->ops->is_prepared) {
		ret = clk->prepare_count ? 1 : 0;
		goto out;
	}

	ret = clk->ops->is_prepared(clk->hw);
out:
	return !!ret;
}

bool __clk_is_prepared(struct clk *clk)
{
	if (!clk)
		return false;

	return clk_core_is_prepared(clk->core);
}

static bool clk_core_is_enabled(struct clk_core *clk)
{
	int ret;

	if (!clk)
		return false;

	/*
	 * .is_enabled is only mandatory for clocks that gate
	 * fall back to software usage counter if .is_enabled is missing
	 */
	if (!clk->ops->is_enabled) {
		ret = clk->enable_count ? 1 : 0;
		goto out;
	}

	ret = clk->ops->is_enabled(clk->hw);
out:
	return !!ret;
}

bool __clk_is_enabled(struct clk *clk)
{
	if (!clk)
		return false;

	return clk_core_is_enabled(clk->core);
}
EXPORT_SYMBOL_GPL(__clk_is_enabled);

static struct clk_core *__clk_lookup_subtree(const char *name,
					     struct clk_core *clk)
{
	struct clk_core *child;
	struct clk_core *ret;

/* IAMROOT-12:
 * -------------
 * clk_core->name과 요청한 name이 동일하면 해당 클럭코어를 반환한다.
 */
	if (!strcmp(clk->name, name))
		return clk;

/* IAMROOT-12:
 * -------------
 * 자식 클럭을 대상으로 검색한다. (재귀루틴)
 */
	hlist_for_each_entry(child, &clk->children, child_node) {
		ret = __clk_lookup_subtree(name, child);
		if (ret)
			return ret;
	}

	return NULL;
}

static struct clk_core *clk_core_lookup(const char *name)
{
	struct clk_core *root_clk;
	struct clk_core *ret;

	if (!name)
		return NULL;

	/* search the 'proper' clk tree first */

/* IAMROOT-12:
 * -------------
 * 루트만 등록되는 리스트에서 name으로 검색한다.
 */
	hlist_for_each_entry(root_clk, &clk_root_list, child_node) {
		ret = __clk_lookup_subtree(name, root_clk);
		if (ret)
			return ret;
	}

	/* if not found, then search the orphan tree */

/* IAMROOT-12:
 * -------------
 * 고아 클럭만 등록되는 리스트에서 name으로 검색한다.
 */
	hlist_for_each_entry(root_clk, &clk_orphan_list, child_node) {
		ret = __clk_lookup_subtree(name, root_clk);
		if (ret)
			return ret;
	}

	return NULL;
}

static bool mux_is_better_rate(unsigned long rate, unsigned long now,
			   unsigned long best, unsigned long flags)
{
	if (flags & CLK_MUX_ROUND_CLOSEST)
		return abs(now - rate) < abs(best - rate);

	return now <= rate && now > best;
}

static long
clk_mux_determine_rate_flags(struct clk_hw *hw, unsigned long rate,
			     unsigned long min_rate,
			     unsigned long max_rate,
			     unsigned long *best_parent_rate,
			     struct clk_hw **best_parent_p,
			     unsigned long flags)
{
	struct clk_core *core = hw->core, *parent, *best_parent = NULL;
	int i, num_parents;
	unsigned long parent_rate, best = 0;

	/* if NO_REPARENT flag set, pass through to current parent */
	if (core->flags & CLK_SET_RATE_NO_REPARENT) {
		parent = core->parent;
		if (core->flags & CLK_SET_RATE_PARENT)
			best = __clk_determine_rate(parent ? parent->hw : NULL,
						    rate, min_rate, max_rate);
		else if (parent)
			best = clk_core_get_rate_nolock(parent);
		else
			best = clk_core_get_rate_nolock(core);
		goto out;
	}

	/* find the parent that can provide the fastest rate <= rate */
	num_parents = core->num_parents;
	for (i = 0; i < num_parents; i++) {
		parent = clk_core_get_parent_by_index(core, i);
		if (!parent)
			continue;
		if (core->flags & CLK_SET_RATE_PARENT)
			parent_rate = __clk_determine_rate(parent->hw, rate,
							   min_rate,
							   max_rate);
		else
			parent_rate = clk_core_get_rate_nolock(parent);
		if (mux_is_better_rate(rate, parent_rate, best, flags)) {
			best_parent = parent;
			best = parent_rate;
		}
	}

out:
	if (best_parent)
		*best_parent_p = best_parent->hw;
	*best_parent_rate = best;

	return best;
}

struct clk *__clk_lookup(const char *name)
{
	struct clk_core *core = clk_core_lookup(name);

	return !core ? NULL : core->hw->clk;
}

static void clk_core_get_boundaries(struct clk_core *clk,
				    unsigned long *min_rate,
				    unsigned long *max_rate)
{
	struct clk *clk_user;

	*min_rate = 0;
	*max_rate = ULONG_MAX;

/* IAMROOT-12:
 * -------------
 * core->clks에 등록된 clk들에서 가장 높은 min_rate를 구한다.
 */
	hlist_for_each_entry(clk_user, &clk->clks, child_node)
		*min_rate = max(*min_rate, clk_user->min_rate);

/* IAMROOT-12:
 * -------------
 * core->clks에 등록된 clk들에서 가장 낮은 max_rate를 구한다.
 */
	hlist_for_each_entry(clk_user, &clk->clks, child_node)
		*max_rate = min(*max_rate, clk_user->max_rate);
}

/*
 * Helper for finding best parent to provide a given frequency. This can be used
 * directly as a determine_rate callback (e.g. for a mux), or from a more
 * complex clock that may combine a mux with other operations.
 */
long __clk_mux_determine_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long min_rate,
			      unsigned long max_rate,
			      unsigned long *best_parent_rate,
			      struct clk_hw **best_parent_p)
{
	return clk_mux_determine_rate_flags(hw, rate, min_rate, max_rate,
					    best_parent_rate,
					    best_parent_p, 0);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate);

long __clk_mux_determine_rate_closest(struct clk_hw *hw, unsigned long rate,
			      unsigned long min_rate,
			      unsigned long max_rate,
			      unsigned long *best_parent_rate,
			      struct clk_hw **best_parent_p)
{
	return clk_mux_determine_rate_flags(hw, rate, min_rate, max_rate,
					    best_parent_rate,
					    best_parent_p,
					    CLK_MUX_ROUND_CLOSEST);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate_closest);

/***        clk api        ***/

static void clk_core_unprepare(struct clk_core *clk)
{
	if (!clk)
		return;

	if (WARN_ON(clk->prepare_count == 0))
		return;

	if (--clk->prepare_count > 0)
		return;

	WARN_ON(clk->enable_count > 0);

	if (clk->ops->unprepare)
		clk->ops->unprepare(clk->hw);

	clk_core_unprepare(clk->parent);
}

/**
 * clk_unprepare - undo preparation of a clock source
 * @clk: the clk being unprepared
 *
 * clk_unprepare may sleep, which differentiates it from clk_disable.  In a
 * simple case, clk_unprepare can be used instead of clk_disable to gate a clk
 * if the operation may sleep.  One example is a clk which is accessed over
 * I2c.  In the complex case a clk gate operation may require a fast and a slow
 * part.  It is this reason that clk_unprepare and clk_disable are not mutually
 * exclusive.  In fact clk_disable must be called before clk_unprepare.
 */
void clk_unprepare(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return;

	clk_prepare_lock();
	clk_core_unprepare(clk->core);
	clk_prepare_unlock();
}
EXPORT_SYMBOL_GPL(clk_unprepare);

static int clk_core_prepare(struct clk_core *clk)
{
	int ret = 0;

/* IAMROOT-12:
 * -------------
 * top 클럭인 경우 부모가 없으므로 상위 재귀 호출에서 빠져나올때 사용한다.
 */
	if (!clk)
		return 0;

/* IAMROOT-12:
 * -------------
 * 클럭이 준비되지 않은 경우 최상위 부모의 클럭을 준비시킨다.
 */
	if (clk->prepare_count == 0) {
		ret = clk_core_prepare(clk->parent);
		if (ret)
			return ret;

/* IAMROOT-12:
 * -------------
 * 최상위 클럭 소스부터 다음에 위치한 gate 클럭의 문을 모두 연다(prepare).
 * 에러가 발생하지 않는한 계속 그 이하 gate를 열게된다.
 */
		if (clk->ops->prepare) {
			ret = clk->ops->prepare(clk->hw);
			if (ret) {
				clk_core_unprepare(clk->parent);
				return ret;
			}
		}
	}

	clk->prepare_count++;

	return 0;
}

/**
 * clk_prepare - prepare a clock source
 * @clk: the clk being prepared
 *
 * clk_prepare may sleep, which differentiates it from clk_enable.  In a simple
 * case, clk_prepare can be used instead of clk_enable to ungate a clk if the
 * operation may sleep.  One example is a clk which is accessed over I2c.  In
 * the complex case a clk ungate operation may require a fast and a slow part.
 * It is this reason that clk_prepare and clk_enable are not mutually
 * exclusive.  In fact clk_prepare must be called before clk_enable.
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_prepare(struct clk *clk)
{
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();
	ret = clk_core_prepare(clk->core);
	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_prepare);

static void clk_core_disable(struct clk_core *clk)
{
	if (!clk)
		return;

	if (WARN_ON(clk->enable_count == 0))
		return;

	if (--clk->enable_count > 0)
		return;

	if (clk->ops->disable)
		clk->ops->disable(clk->hw);

	clk_core_disable(clk->parent);
}

static void __clk_disable(struct clk *clk)
{
	if (!clk)
		return;

	clk_core_disable(clk->core);
}

/**
 * clk_disable - gate a clock
 * @clk: the clk being gated
 *
 * clk_disable must not sleep, which differentiates it from clk_unprepare.  In
 * a simple case, clk_disable can be used instead of clk_unprepare to gate a
 * clk if the operation is fast and will never sleep.  One example is a
 * SoC-internal clk which is controlled via simple register writes.  In the
 * complex case a clk gate operation may require a fast and a slow part.  It is
 * this reason that clk_unprepare and clk_disable are not mutually exclusive.
 * In fact clk_disable must be called before clk_unprepare.
 */
void clk_disable(struct clk *clk)
{
	unsigned long flags;

	if (IS_ERR_OR_NULL(clk))
		return;

	flags = clk_enable_lock();
	__clk_disable(clk);
	clk_enable_unlock(flags);
}
EXPORT_SYMBOL_GPL(clk_disable);

static int clk_core_enable(struct clk_core *clk)
{
	int ret = 0;

	if (!clk)
		return 0;

/* IAMROOT-12:
 * -------------
 * prepare 후 enable 가능하다.
 * 나머지 코드는 clk_core_prepare()와 동일한 진행을 한다.
 */
	if (WARN_ON(clk->prepare_count == 0))
		return -ESHUTDOWN;

	if (clk->enable_count == 0) {
		ret = clk_core_enable(clk->parent);

		if (ret)
			return ret;

		if (clk->ops->enable) {
			ret = clk->ops->enable(clk->hw);
			if (ret) {
				clk_core_disable(clk->parent);
				return ret;
			}
		}
	}

	clk->enable_count++;
	return 0;
}

static int __clk_enable(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_enable(clk->core);
}

/**
 * clk_enable - ungate a clock
 * @clk: the clk being ungated
 *
 * clk_enable must not sleep, which differentiates it from clk_prepare.  In a
 * simple case, clk_enable can be used instead of clk_prepare to ungate a clk
 * if the operation will never sleep.  One example is a SoC-internal clk which
 * is controlled via simple register writes.  In the complex case a clk ungate
 * operation may require a fast and a slow part.  It is this reason that
 * clk_enable and clk_prepare are not mutually exclusive.  In fact clk_prepare
 * must be called before clk_enable.  Returns 0 on success, -EERROR
 * otherwise.
 */
int clk_enable(struct clk *clk)
{
	unsigned long flags;
	int ret;

	flags = clk_enable_lock();
	ret = __clk_enable(clk);
	clk_enable_unlock(flags);

	return ret;
}
EXPORT_SYMBOL_GPL(clk_enable);

static unsigned long clk_core_round_rate_nolock(struct clk_core *clk,
						unsigned long rate,
						unsigned long min_rate,
						unsigned long max_rate)
{
	unsigned long parent_rate = 0;
	struct clk_core *parent;
	struct clk_hw *parent_hw;

	if (!clk)
		return 0;

	parent = clk->parent;
	if (parent)
		parent_rate = parent->rate;

	if (clk->ops->determine_rate) {
		parent_hw = parent ? parent->hw : NULL;
		return clk->ops->determine_rate(clk->hw, rate,
						min_rate, max_rate,
						&parent_rate, &parent_hw);
	} else if (clk->ops->round_rate)
		return clk->ops->round_rate(clk->hw, rate, &parent_rate);
	else if (clk->flags & CLK_SET_RATE_PARENT)
		return clk_core_round_rate_nolock(clk->parent, rate, min_rate,
						  max_rate);
	else
		return clk->rate;
}

/**
 * __clk_determine_rate - get the closest rate actually supported by a clock
 * @hw: determine the rate of this clock
 * @rate: target rate
 * @min_rate: returned rate must be greater than this rate
 * @max_rate: returned rate must be less than this rate
 *
 * Caller must hold prepare_lock.  Useful for clk_ops such as .set_rate and
 * .determine_rate.
 */
unsigned long __clk_determine_rate(struct clk_hw *hw,
				   unsigned long rate,
				   unsigned long min_rate,
				   unsigned long max_rate)
{
	if (!hw)
		return 0;

	return clk_core_round_rate_nolock(hw->core, rate, min_rate, max_rate);
}
EXPORT_SYMBOL_GPL(__clk_determine_rate);

/**
 * __clk_round_rate - round the given rate for a clk
 * @clk: round the rate of this clock
 * @rate: the rate which is to be rounded
 *
 * Caller must hold prepare_lock.  Useful for clk_ops such as .set_rate
 */
unsigned long __clk_round_rate(struct clk *clk, unsigned long rate)
{
	unsigned long min_rate;
	unsigned long max_rate;

	if (!clk)
		return 0;

	clk_core_get_boundaries(clk->core, &min_rate, &max_rate);

	return clk_core_round_rate_nolock(clk->core, rate, min_rate, max_rate);
}
EXPORT_SYMBOL_GPL(__clk_round_rate);

/**
 * clk_round_rate - round the given rate for a clk
 * @clk: the clk for which we are rounding a rate
 * @rate: the rate which is to be rounded
 *
 * Takes in a rate as input and rounds it to a rate that the clk can actually
 * use which is then returned.  If clk doesn't support round_rate operation
 * then the parent rate is returned.
 */
long clk_round_rate(struct clk *clk, unsigned long rate)
{
	unsigned long ret;

	if (!clk)
		return 0;

	clk_prepare_lock();
	ret = __clk_round_rate(clk, rate);
	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_round_rate);

/**
 * __clk_notify - call clk notifier chain
 * @clk: struct clk * that is changing rate
 * @msg: clk notifier type (see include/linux/clk.h)
 * @old_rate: old clk rate
 * @new_rate: new clk rate
 *
 * Triggers a notifier call chain on the clk rate-change notification
 * for 'clk'.  Passes a pointer to the struct clk and the previous
 * and current rates to the notifier callback.  Intended to be called by
 * internal clock code only.  Returns NOTIFY_DONE from the last driver
 * called if all went well, or NOTIFY_STOP or NOTIFY_BAD immediately if
 * a driver returns that.
 */
static int __clk_notify(struct clk_core *clk, unsigned long msg,
		unsigned long old_rate, unsigned long new_rate)
{
	struct clk_notifier *cn;
	struct clk_notifier_data cnd;
	int ret = NOTIFY_DONE;

	cnd.old_rate = old_rate;
	cnd.new_rate = new_rate;

/* IAMROOT-12:
 * -------------
 * 클럭 notify 체인에 등록한 클럭에서 인수로 지정된 클럭이 있는 경우 
 * 그 클럭에 등록된 함수를 호출하고 결과를 알아온다.
 * (결과: NOTIFY_DONE, NOTIFY_STOP, NOTIFY_BAD)
 */
	list_for_each_entry(cn, &clk_notifier_list, node) {
		if (cn->clk->core == clk) {
			cnd.clk = cn->clk;
			ret = srcu_notifier_call_chain(&cn->notifier_head, msg,
					&cnd);
		}
	}

	return ret;
}

/**
 * __clk_recalc_accuracies
 * @clk: first clk in the subtree
 *
 * Walks the subtree of clks starting with clk and recalculates accuracies as
 * it goes.  Note that if a clk does not implement the .recalc_accuracy
 * callback then it is assumed that the clock will take on the accuracy of it's
 * parent.
 *
 * Caller must hold prepare_lock.
 */
static void __clk_recalc_accuracies(struct clk_core *clk)
{
	unsigned long parent_accuracy = 0;
	struct clk_core *child;

/* IAMROOT-12:
 * -------------
 * 현재 클럭 및 자식 클럭들의 정확도를 재산출하게 한다.
 */

/* IAMROOT-12:
 * -------------
 * 부모가 지정된 경우 부모의 정확도를 가져온다.
 */
	if (clk->parent)
		parent_accuracy = clk->parent->accuracy;

/* IAMROOT-12:
 * -------------
 * 가져온 부모이 정확도로 (*recalc_accuracy) 함수를 호출하여 재계산된 
 * 정확도를 클럭에 설정한다.
 *
 * (*recalc_accuracy)가 제공되지 않는 경우에는 부모의 정확도 값을 사용한다.
 */
	if (clk->ops->recalc_accuracy)
		clk->accuracy = clk->ops->recalc_accuracy(clk->hw,
							  parent_accuracy);
	else
		clk->accuracy = parent_accuracy;

/* IAMROOT-12:
 * -------------
 * 현재 클럭의 children 클럭들을 모두 재계산한다. (재귀호출)
 */
	hlist_for_each_entry(child, &clk->children, child_node)
		__clk_recalc_accuracies(child);
}

static long clk_core_get_accuracy(struct clk_core *clk)
{
	unsigned long accuracy;

	clk_prepare_lock();
	if (clk && (clk->flags & CLK_GET_ACCURACY_NOCACHE))
		__clk_recalc_accuracies(clk);

	accuracy = __clk_get_accuracy(clk);
	clk_prepare_unlock();

	return accuracy;
}

/**
 * clk_get_accuracy - return the accuracy of clk
 * @clk: the clk whose accuracy is being returned
 *
 * Simply returns the cached accuracy of the clk, unless
 * CLK_GET_ACCURACY_NOCACHE flag is set, which means a recalc_rate will be
 * issued.
 * If clk is NULL then returns 0.
 */
long clk_get_accuracy(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_get_accuracy(clk->core);
}
EXPORT_SYMBOL_GPL(clk_get_accuracy);

static unsigned long clk_recalc(struct clk_core *clk,
				unsigned long parent_rate)
{

/* IAMROOT-12:
 * -------------
 * (*recalc_rate) 함수가 제공된 경우 호출하여 재산출된 rate를 가져오고 
 * 함수가 없으면 부모 rate 값을 그대로 반환한다.
 */
	if (clk->ops->recalc_rate)
		return clk->ops->recalc_rate(clk->hw, parent_rate);
	return parent_rate;
}

/**
 * __clk_recalc_rates
 * @clk: first clk in the subtree
 * @msg: notification type (see include/linux/clk.h)
 *
 * Walks the subtree of clks starting with clk and recalculates rates as it
 * goes.  Note that if a clk does not implement the .recalc_rate callback then
 * it is assumed that the clock will take on the rate of its parent.
 *
 * clk_recalc_rates also propagates the POST_RATE_CHANGE notification,
 * if necessary.
 *
 * Caller must hold prepare_lock.
 */
static void __clk_recalc_rates(struct clk_core *clk, unsigned long msg)
{
	unsigned long old_rate;
	unsigned long parent_rate = 0;
	struct clk_core *child;

/* IAMROOT-12:
 * -------------
 * 부모 클럭의 rate로 현재 클럭 및 자식들 클럭들을 모두 재계산한다. 
 */
	old_rate = clk->rate;

	if (clk->parent)
		parent_rate = clk->parent->rate;

	clk->rate = clk_recalc(clk, parent_rate);

	/*
	 * ignore NOTIFY_STOP and NOTIFY_BAD return values for POST_RATE_CHANGE
	 * & ABORT_RATE_CHANGE notifiers
	 */

/* IAMROOT-12:
 * -------------
 * notify 체인에 등록된 클럭에 대해 등록된 함수를 호출하고 그 결과는 무시한다.
 */
	if (clk->notifier_count && msg)
		__clk_notify(clk, msg, old_rate, clk->rate);

	hlist_for_each_entry(child, &clk->children, child_node)
		__clk_recalc_rates(child, msg);
}

static unsigned long clk_core_get_rate(struct clk_core *clk)
{
	unsigned long rate;

	clk_prepare_lock();

	if (clk && (clk->flags & CLK_GET_RATE_NOCACHE))
		__clk_recalc_rates(clk, 0);

	rate = clk_core_get_rate_nolock(clk);
	clk_prepare_unlock();

	return rate;
}

/**
 * clk_get_rate - return the rate of clk
 * @clk: the clk whose rate is being returned
 *
 * Simply returns the cached rate of the clk, unless CLK_GET_RATE_NOCACHE flag
 * is set, which means a recalc_rate will be issued.
 * If clk is NULL then returns 0.
 */
unsigned long clk_get_rate(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_get_rate(clk->core);
}
EXPORT_SYMBOL_GPL(clk_get_rate);

static int clk_fetch_parent_index(struct clk_core *clk,
				  struct clk_core *parent)
{
	int i;

/* IAMROOT-12:
 * -------------
 * 현재 클럭의 부모 클럭들에 대해 지정되지 않은 경우 부모 수 만큼 
 * clk_core 포인터를 할당한다.
 */
	if (!clk->parents) {
		clk->parents = kcalloc(clk->num_parents,
					sizeof(struct clk *), GFP_KERNEL);
		if (!clk->parents)
			return -ENOMEM;
	}

	/*
	 * find index of new parent clock using cached parent ptrs,
	 * or if not yet cached, use string name comparison and cache
	 * them now to avoid future calls to clk_core_lookup.
	 */

/* IAMROOT-12:
 * -------------
 * 부모 클럭을 이름으로 검색하여 연결하고 인덱스 값을 반환한다. (0~)
 * 못 찾은 경우 -ENOMEM을 반환한다.
 */
	for (i = 0; i < clk->num_parents; i++) {
		if (clk->parents[i] == parent)
			return i;

		if (clk->parents[i])
			continue;

		if (!strcmp(clk->parent_names[i], parent->name)) {
			clk->parents[i] = clk_core_lookup(parent->name);
			return i;
		}
	}

	return -EINVAL;
}

static void clk_reparent(struct clk_core *clk, struct clk_core *new_parent)
{

/* IAMROOT-12:
 * -------------
 * 기존 관리되고 있는 리스트에서 제거하는데 다음과 같이 2가지 케이스가 가능하다.
 *     1) 전역 고아 클럭 리스트에서 제거
 *     2) 기존 모 노드의 children에서 제거
 */
	hlist_del(&clk->child_node);


/* IAMROOT-12:
 * -------------
 * 부모가 지정되었을 때 이미 그 부모의 새 자식이 이 클럭인 경우 null을 대입하고
 * 부모의 children 리스트에 추가한다. 
 *
 * 부모가 없는 경우 고아 클럭 리스트에 추가한다.
 */
	if (new_parent) {
		/* avoid duplicate POST_RATE_CHANGE notifications */
		if (new_parent->new_child == clk)
			new_parent->new_child = NULL;

		hlist_add_head(&clk->child_node, &new_parent->children);
	} else {
		hlist_add_head(&clk->child_node, &clk_orphan_list);
	}

/* IAMROOT-12:
 * -------------
 * 새 부모를 선택한다.
 */
	clk->parent = new_parent;
}

static struct clk_core *__clk_set_parent_before(struct clk_core *clk,
					   struct clk_core *parent)
{
	unsigned long flags;
	struct clk_core *old_parent = clk->parent;

	/*
	 * Migrate prepare state between parents and prevent race with
	 * clk_enable().
	 *
	 * If the clock is not prepared, then a race with
	 * clk_enable/disable() is impossible since we already have the
	 * prepare lock (future calls to clk_enable() need to be preceded by
	 * a clk_prepare()).
	 *
	 * If the clock is prepared, migrate the prepared state to the new
	 * parent and also protect against a race with clk_enable() by
	 * forcing the clock and the new parent on.  This ensures that all
	 * future calls to clk_enable() are practically NOPs with respect to
	 * hardware and software states.
	 *
	 * See also: Comment for clk_set_parent() below.
	 */

/* IAMROOT-12:
 * -------------
 * 새 부모의 클럭이 준비된 경우 enable 시킨다.
 */
	if (clk->prepare_count) {
		clk_core_prepare(parent);
		flags = clk_enable_lock();
		clk_core_enable(parent);
		clk_core_enable(clk);
		clk_enable_unlock(flags);
	}

	/* update the clk tree topology */

/* IAMROOT-12:
 * -------------
 * new_child, new_parent가 현재 클럭의 토플로지에 반영된다.
 */
	flags = clk_enable_lock();
	clk_reparent(clk, parent);
	clk_enable_unlock(flags);

	return old_parent;
}

static void __clk_set_parent_after(struct clk_core *core,
				   struct clk_core *parent,
				   struct clk_core *old_parent)
{
	unsigned long flags;

	/*
	 * Finish the migration of prepare state and undo the changes done
	 * for preventing a race with clk_enable().
	 */

/* IAMROOT-12:
 * -------------
 * 사용하지 않는 클럭은 disable -> unprepare 처리한다.
 */
	if (core->prepare_count) {
		flags = clk_enable_lock();
		clk_core_disable(core);
		clk_core_disable(old_parent);
		clk_enable_unlock(flags);
		clk_core_unprepare(old_parent);
	}
}

static int __clk_set_parent(struct clk_core *clk, struct clk_core *parent,
			    u8 p_index)
{
	unsigned long flags;
	int ret = 0;
	struct clk_core *old_parent;

	old_parent = __clk_set_parent_before(clk, parent);

	/* change clock input source */
	if (parent && clk->ops->set_parent)
		ret = clk->ops->set_parent(clk->hw, p_index);

	if (ret) {
		flags = clk_enable_lock();
		clk_reparent(clk, old_parent);
		clk_enable_unlock(flags);

		if (clk->prepare_count) {
			flags = clk_enable_lock();
			clk_core_disable(clk);
			clk_core_disable(parent);
			clk_enable_unlock(flags);
			clk_core_unprepare(parent);
		}
		return ret;
	}

	__clk_set_parent_after(clk, parent, old_parent);

	return 0;
}

/**
 * __clk_speculate_rates
 * @clk: first clk in the subtree
 * @parent_rate: the "future" rate of clk's parent
 *
 * Walks the subtree of clks starting with clk, speculating rates as it
 * goes and firing off PRE_RATE_CHANGE notifications as necessary.
 *
 * Unlike clk_recalc_rates, clk_speculate_rates exists only for sending
 * pre-rate change notifications and returns early if no clks in the
 * subtree have subscribed to the notifications.  Note that if a clk does not
 * implement the .recalc_rate callback then it is assumed that the clock will
 * take on the rate of its parent.
 *
 * Caller must hold prepare_lock.
 */
static int __clk_speculate_rates(struct clk_core *clk,
				 unsigned long parent_rate)
{
	struct clk_core *child;
	unsigned long new_rate;
	int ret = NOTIFY_DONE;

	new_rate = clk_recalc(clk, parent_rate);

	/* abort rate change if a driver returns NOTIFY_BAD or NOTIFY_STOP */
	if (clk->notifier_count)
		ret = __clk_notify(clk, PRE_RATE_CHANGE, clk->rate, new_rate);

	if (ret & NOTIFY_STOP_MASK) {
		pr_debug("%s: clk notifier callback for clock %s aborted with error %d\n",
				__func__, clk->name, ret);
		goto out;
	}

	hlist_for_each_entry(child, &clk->children, child_node) {
		ret = __clk_speculate_rates(child, new_rate);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}

out:
	return ret;
}

static void clk_calc_subtree(struct clk_core *clk, unsigned long new_rate,
			     struct clk_core *new_parent, u8 p_index)
{
	struct clk_core *child;

	clk->new_rate = new_rate;
	clk->new_parent = new_parent;
	clk->new_parent_index = p_index;
	/* include clk in new parent's PRE_RATE_CHANGE notifications */
	clk->new_child = NULL;

/* IAMROOT-12:
 * -------------
 * 부모 클럭이 바뀐 경우 새 부모 클럭의 new_child에 현재 클럭이 등록될거라고 
 * 준비한다. (propogate 과정에서 commit될 때 변경된다.)
 */
	if (new_parent && new_parent != clk->parent)
		new_parent->new_child = clk;

/* IAMROOT-12:
 * -------------
 * 현재 클럭의 자식 클럭들의 rate를 재평가하여 new_rate에 잠시 기록해둔다.
 * 역시 propogate 과정에서 commit되는 경우 rate <- new_rate 반영된다
 * 자식 밑으로도 자식이 있는 경우 재귀호출하여 
 * 평가(new_parent, new_child, new_rate) 한다.
 */
	hlist_for_each_entry(child, &clk->children, child_node) {
		child->new_rate = clk_recalc(child, new_rate);
		clk_calc_subtree(child, child->new_rate, NULL, 0);
	}
}

/*
 * calculate the new rates returning the topmost clock that has to be
 * changed.
 */
static struct clk_core *clk_calc_new_rates(struct clk_core *clk,
					   unsigned long rate)
{
	struct clk_core *top = clk;
	struct clk_core *old_parent, *parent;
	struct clk_hw *parent_hw;
	unsigned long best_parent_rate = 0;
	unsigned long new_rate;
	unsigned long min_rate;
	unsigned long max_rate;
	int p_index = 0;

	/* sanity */
	if (IS_ERR_OR_NULL(clk))
		return NULL;

	/* save parent rate, if it exists */
	parent = old_parent = clk->parent;
	if (parent)
		best_parent_rate = parent->rate;

/* IAMROOT-12:
 * -------------
 * 현재 클럭이 설정할 수 있는 범위(min ~ max)를 알아온다.
 */
	clk_core_get_boundaries(clk, &min_rate, &max_rate);

	/* find the closest rate and parent clk/rate */

/* IAMROOT-12:
 * -------------
 * mux 기능이 구현된 클럭인 경우 해당 드라이버가 부모 클럭을 선택하여
 * 결정한 rate 값을 가져온다.
 */
	if (clk->ops->determine_rate) {
		parent_hw = parent ? parent->hw : NULL;
		new_rate = clk->ops->determine_rate(clk->hw, rate,
						    min_rate,
						    max_rate,
						    &best_parent_rate,
						    &parent_hw);
		parent = parent_hw ? parent_hw->core : NULL;

/* IAMROOT-12:
 * -------------
 * divider류의 클럭인 경우 해당 드라이버가 설정할 수 있는 best rate를 알아온다.
 */
	} else if (clk->ops->round_rate) {
		new_rate = clk->ops->round_rate(clk->hw, rate,
						&best_parent_rate);
		if (new_rate < min_rate || new_rate > max_rate)
			return NULL;

/* IAMROOT-12:
 * -------------
 * 루트 클럭이거나 부모 클럭을 설정하지 못하게 한 경우 현재 클럭 선에서 
 * 결정하게 한다.
 */
	} else if (!parent || !(clk->flags & CLK_SET_RATE_PARENT)) {
		/* pass-through clock without adjustable parent */
		clk->new_rate = clk->rate;
		return NULL;

/* IAMROOT-12:
 * -------------
 * gate류의 클럭인 경우 부모 클럭 rate를 상속받는다.
 */
	} else {
		/* pass-through clock with adjustable parent */
		top = clk_calc_new_rates(parent, rate);
		new_rate = parent->new_rate;
		goto out;
	}

	/* some clocks must be gated to change parent */

/* IAMROOT-12:
 * -------------
 * mux 클럭 드라이버로 인해 부모 클럭이 변경되었는데 CLK_SET_PARENT_GATE 옵션
 * 으로 인해 gate되지 않은 클럭에 대해 rate 변경을 요청하게 한 경우 
 * 디버그 로그를 출력하고 더 이상 처리없이 함수를 빠져나간다.
 */
	if (parent != old_parent &&
	    (clk->flags & CLK_SET_PARENT_GATE) && clk->prepare_count) {
		pr_debug("%s: %s not gated but wants to reparent\n",
			 __func__, clk->name);
		return NULL;
	}

	/* try finding the new parent index */

/* IAMROOT-12:
 * -------------
 * mux 타입 클럭에서 부모 클럭 인덱스 값을 알아온다.
 */
	if (parent && clk->num_parents > 1) {
		p_index = clk_fetch_parent_index(clk, parent);
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
				 __func__, parent->name, clk->name);
			return NULL;
		}
	}

/* IAMROOT-12:
 * -------------
 * rate 설정을 위해 부모 클럭도 진행하라고 요청받은 경우 부모 클럭으로
 * 이 함수를 상위 방향으로 재귀호출한다.
 */
	if ((clk->flags & CLK_SET_RATE_PARENT) && parent &&
	    best_parent_rate != parent->rate)
		top = clk_calc_new_rates(parent, best_parent_rate);

out:

/* IAMROOT-12:
 * -------------
 * 변경될 부모관계(new_parent, new_child) 및 rate(new_rate) 값들을 설정한다.
 * 내부에서는 하위 방향으로 재귀호출한다.
 */
	clk_calc_subtree(clk, new_rate, parent, p_index);

	return top;
}

/*
 * Notify about rate changes in a subtree. Always walk down the whole tree
 * so that in case of an error we can walk down the whole tree again and
 * abort the change.
 */
static struct clk_core *clk_propagate_rate_change(struct clk_core *clk,
						  unsigned long event)
{
	struct clk_core *child, *tmp_clk, *fail_clk = NULL;
	int ret = NOTIFY_DONE;

/* IAMROOT-12:
 * -------------
 * rate 변경이 발생한 경우가 아니면 성공(null)을 반환한다.
 */
	if (clk->rate == clk->new_rate)
		return NULL;

/* IAMROOT-12:
 * -------------
 * 이 클럭에 대해 notify 요청을 한 경우 결과를 확인한다. 0인 경우 성공
 * 실패한 경우 fail_clk이 설정된다.
 */
	if (clk->notifier_count) {
		ret = __clk_notify(clk, event, clk->rate, clk->new_rate);
		if (ret & NOTIFY_STOP_MASK)
			fail_clk = clk;
	}

/* IAMROOT-12:
 * -------------
 * 자식들이 있는 경우 모든 자식 클럭 노드 밑으로 통지확인 한다. (재귀호출)
 * (단 부모가 바뀐 child 노드 밑으로는 통지하지 않는다)
 */
	hlist_for_each_entry(child, &clk->children, child_node) {
		/* Skip children who will be reparented to another clock */
		if (child->new_parent && child->new_parent != clk)
			continue;
		tmp_clk = clk_propagate_rate_change(child, event);
		if (tmp_clk)
			fail_clk = tmp_clk;
	}

/* IAMROOT-12:
 * -------------
 * 새롭게 부모가 바뀐 child 노드들 밑으로도 통지한다. (재귀호출)
 */
	/* handle the new child who might not be in clk->children yet */
	if (clk->new_child) {
		tmp_clk = clk_propagate_rate_change(clk->new_child, event);
		if (tmp_clk)
			fail_clk = tmp_clk;
	}

	return fail_clk;
}

/*
 * walk down a subtree and set the new rates notifying the rate
 * change on the way
 */
static void clk_change_rate(struct clk_core *clk)
{
	struct clk_core *child;
	struct hlist_node *tmp;
	unsigned long old_rate;
	unsigned long best_parent_rate = 0;
	bool skip_set_rate = false;
	struct clk_core *old_parent;

	old_rate = clk->rate;

/* IAMROOT-12:
 * -------------
 * 현재 mux 클럭의 부모가 바뀐 경우 새 부모 클럭의 rate를 best_parent_rate로
 * 바꾸고 그렇지 않은 경우 원래 부모의 rate 값을 사용한다.
 */
	if (clk->new_parent)
		best_parent_rate = clk->new_parent->rate;
	else if (clk->parent)
		best_parent_rate = clk->parent->rate;

/* IAMROOT-12:
 * -------------
 * 현재 mux 클럭의 부모가 바뀐 경우 new_child, new_parent를 반영한다.
 * (이 과정에서 prepare 및 enable을 수행한다.)
 */
	if (clk->new_parent && clk->new_parent != clk->parent) {
		old_parent = __clk_set_parent_before(clk, clk->new_parent);

/* IAMROOT-12:
 * -------------
 * 부모와 rate 설정을 동시에 변경가능한 ops를 제공하는 경우 해당 함수를 
 * 호출한다.
 */
		if (clk->ops->set_rate_and_parent) {
			skip_set_rate = true;
			clk->ops->set_rate_and_parent(clk->hw, clk->new_rate,
					best_parent_rate,
					clk->new_parent_index);

/* IAMROOT-12:
 * -------------
 * 현재 클럭 드라이버에서 부모 클럭을 바꾸도록 설정 요청한다.
 */
		} else if (clk->ops->set_parent) {
			clk->ops->set_parent(clk->hw, clk->new_parent_index);
		}

/* IAMROOT-12:
 * -------------
 * 사용하지 않는 부모 클럭은 disable -> unprepare 처리한다.
 */
		__clk_set_parent_after(clk, clk->new_parent, old_parent);
	}

/* IAMROOT-12:
 * -------------
 * rate 변경 ops를 제공하는 경우 해당 드라이버의 함수를 호출한다.
 */
	if (!skip_set_rate && clk->ops->set_rate)
		clk->ops->set_rate(clk->hw, clk->new_rate, best_parent_rate);

/* IAMROOT-12:
 * -------------
 * 현재 클럭 드라이버의 rate 재계산 ops를 수행하여 rate를 산출한다.
 */
	clk->rate = clk_recalc(clk, best_parent_rate);

/* IAMROOT-12:
 * -------------
 * 통지 요청한 클럭이면서 rate가 변경된 경우 통지를 한다.
 * (결과 여부는 무시한다.)
 */
	if (clk->notifier_count && old_rate != clk->rate)
		__clk_notify(clk, POST_RATE_CHANGE, old_rate, clk->rate);

	/*
	 * Use safe iteration, as change_rate can actually swap parents
	 * for certain clock types.
	 */

/* IAMROOT-12:
 * -------------
 * 하위 자식 노드들에 대해 rate 변경을 한다. (재귀 호출)
 * 단 부모가 다른 부모로 바뀐 child 에 대해서는 통지할 필요 없다.
 */
	hlist_for_each_entry_safe(child, tmp, &clk->children, child_node) {
		/* Skip children who will be reparented to another clock */
		if (child->new_parent && child->new_parent != clk)
			continue;
		clk_change_rate(child);
	}

	/* handle the new child who might not be in clk->children yet */

/* IAMROOT-12:
 * -------------
 * 새로 등록된 child 이하의 노드들에 대해 rate 변경을 수행한다.
 */
	if (clk->new_child)
		clk_change_rate(clk->new_child);
}

static int clk_core_set_rate_nolock(struct clk_core *clk,
				    unsigned long req_rate)
{
	struct clk_core *top, *fail_clk;
	unsigned long rate = req_rate;
	int ret = 0;

	if (!clk)
		return 0;

	/* bail early if nothing to do */

/* IAMROOT-12:
 * -------------
 * 요청한 rate와 동일한 상태면 빠져나간다.
 */
	if (rate == clk_core_get_rate_nolock(clk))
		return 0;

/* IAMROOT-12:
 * -------------
 * 클럭이 동작중(ungate)일 때에 rate 변경이 불가능
 */
	if ((clk->flags & CLK_SET_RATE_GATE) && clk->prepare_count)
		return -EBUSY;

	/* calculate new rates and get the topmost changed clock */

/* IAMROOT-12:
 * -------------
 * 현재 클럭을 대상으로 영향을 끼치는 토플로지내에서 
 * new_child, new_parent, new_rate를 산출한다.
 */
	top = clk_calc_new_rates(clk, rate);
	if (!top)
		return -EINVAL;

	/* notify that we are about to change rates */

/* IAMROOT-12:
 * -------------
 * PRE 단계 통지하여 문제가 되는 클럭이 있는지 확인한다.
 * 문제가 발생한 경우 abort 통지한다.
 */
	fail_clk = clk_propagate_rate_change(top, PRE_RATE_CHANGE);
	if (fail_clk) {
		pr_debug("%s: failed to set %s rate\n", __func__,
				fail_clk->name);
		clk_propagate_rate_change(top, ABORT_RATE_CHANGE);
		return -EBUSY;
	}

	/* change the rates */

/* IAMROOT-12:
 * -------------
 * 문제가 없으면 rate 변경을 시도한다. 이 과정에 post 통지한다.
 */
	clk_change_rate(top);

	clk->req_rate = req_rate;

	return ret;
}

/**
 * clk_set_rate - specify a new rate for clk
 * @clk: the clk whose rate is being changed
 * @rate: the new rate for clk
 *
 * In the simplest case clk_set_rate will only adjust the rate of clk.
 *
 * Setting the CLK_SET_RATE_PARENT flag allows the rate change operation to
 * propagate up to clk's parent; whether or not this happens depends on the
 * outcome of clk's .round_rate implementation.  If *parent_rate is unchanged
 * after calling .round_rate then upstream parent propagation is ignored.  If
 * *parent_rate comes back with a new rate for clk's parent then we propagate
 * up to clk's parent and set its rate.  Upward propagation will continue
 * until either a clk does not support the CLK_SET_RATE_PARENT flag or
 * .round_rate stops requesting changes to clk's parent_rate.
 *
 * Rate changes are accomplished via tree traversal that also recalculates the
 * rates for the clocks and fires off POST_RATE_CHANGE notifiers.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;

	if (!clk)
		return 0;

	/* prevent racing with updates to the clock topology */

/* IAMROOT-12:
 * -------------
 * 이 클럭의 rate를 설정한다. 부모 관계 및 플래그 설정에 따라 주변 클럭들의
 * rate 설정에 영향을 끼친다.
 */
	clk_prepare_lock();

	ret = clk_core_set_rate_nolock(clk->core, rate);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate);

/**
 * clk_set_rate_range - set a rate range for a clock source
 * @clk: clock source
 * @min: desired minimum clock rate in Hz, inclusive
 * @max: desired maximum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_rate_range(struct clk *clk, unsigned long min, unsigned long max)
{
	int ret = 0;

	if (!clk)
		return 0;

	if (min > max) {
		pr_err("%s: clk %s dev %s con %s: invalid range [%lu, %lu]\n",
		       __func__, clk->core->name, clk->dev_id, clk->con_id,
		       min, max);
		return -EINVAL;
	}

	clk_prepare_lock();

	if (min != clk->min_rate || max != clk->max_rate) {
		clk->min_rate = min;
		clk->max_rate = max;
		ret = clk_core_set_rate_nolock(clk->core, clk->core->req_rate);
	}

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_rate_range);

/**
 * clk_set_min_rate - set a minimum clock rate for a clock source
 * @clk: clock source
 * @rate: desired minimum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_min_rate(struct clk *clk, unsigned long rate)
{
	if (!clk)
		return 0;

	return clk_set_rate_range(clk, rate, clk->max_rate);
}
EXPORT_SYMBOL_GPL(clk_set_min_rate);

/**
 * clk_set_max_rate - set a maximum clock rate for a clock source
 * @clk: clock source
 * @rate: desired maximum clock rate in Hz, inclusive
 *
 * Returns success (0) or negative errno.
 */
int clk_set_max_rate(struct clk *clk, unsigned long rate)
{
	if (!clk)
		return 0;

	return clk_set_rate_range(clk, clk->min_rate, rate);
}
EXPORT_SYMBOL_GPL(clk_set_max_rate);

/**
 * clk_get_parent - return the parent of a clk
 * @clk: the clk whose parent gets returned
 *
 * Simply returns clk->parent.  Returns NULL if clk is NULL.
 */
struct clk *clk_get_parent(struct clk *clk)
{
	struct clk *parent;

	clk_prepare_lock();
	parent = __clk_get_parent(clk);
	clk_prepare_unlock();

	return parent;
}
EXPORT_SYMBOL_GPL(clk_get_parent);

/*
 * .get_parent is mandatory for clocks with multiple possible parents.  It is
 * optional for single-parent clocks.  Always call .get_parent if it is
 * available and WARN if it is missing for multi-parent clocks.
 *
 * For single-parent clocks without .get_parent, first check to see if the
 * .parents array exists, and if so use it to avoid an expensive tree
 * traversal.  If .parents does not exist then walk the tree.
 */
static struct clk_core *__clk_init_parent(struct clk_core *clk)
{
	struct clk_core *ret = NULL;
	u8 index;

	/* handle the trivial cases */

/* IAMROOT-12:
 * -------------
 * 부모 클럭이 없는 경우 종료
 */
	if (!clk->num_parents)
		goto out;

/* IAMROOT-12:
 * -------------
 * 부모 클럭이 하나인 경우 부모 클럭을 찾아 연결한다.
 */
	if (clk->num_parents == 1) {
		if (IS_ERR_OR_NULL(clk->parent))
			clk->parent = clk_core_lookup(clk->parent_names[0]);
		ret = clk->parent;
		goto out;
	}

/* IAMROOT-12:
 * -------------
 * mux 타입 클럭에서 (*get_parent) 함수가 구현되지 않은 드라이버는 에러
 */
	if (!clk->ops->get_parent) {
		WARN(!clk->ops->get_parent,
			"%s: multi-parent clocks must implement .get_parent\n",
			__func__);
		goto out;
	};

	/*
	 * Do our best to cache parent clocks in clk->parents.  This prevents
	 * unnecessary and expensive lookups.  We don't set clk->parent here;
	 * that is done by the calling function.
	 */

/* IAMROOT-12:
 * -------------
 * 현재 시점에서 가장(best) 적절한 부모 클럭 인덱스를 구한다.
 */
	index = clk->ops->get_parent(clk->hw);

/* IAMROOT-12:
 * -------------
 * 부모 클럭을 연결하기 위해 부모 수 만큼 포인터 배열을 할당한다.
 */
	if (!clk->parents)
		clk->parents =
			kcalloc(clk->num_parents, sizeof(struct clk *),
					GFP_KERNEL);

/* IAMROOT-12:
 * -------------
 * 인덱스에 따른 부모를 찾아 연결하고 반환한다.
 */
	ret = clk_core_get_parent_by_index(clk, index);

out:
	return ret;
}

static void clk_core_reparent(struct clk_core *clk,
				  struct clk_core *new_parent)
{
/* IAMROOT-12:
 * -------------
 * 새 부모로 바뀌면서 현재 클럭 이하 모든 자식 클럭들의 정확도와 rate를 
 * 재설정한다. 다만 rate를 바꾸는 과정에서 클럭 notify chain에 등록한 
 * 함수의 처리 결과를 반영하지는 않는다.
 */
	clk_reparent(clk, new_parent);
	__clk_recalc_accuracies(clk);
	__clk_recalc_rates(clk, POST_RATE_CHANGE);
}

/**
 * clk_has_parent - check if a clock is a possible parent for another
 * @clk: clock source
 * @parent: parent clock source
 *
 * This function can be used in drivers that need to check that a clock can be
 * the parent of another without actually changing the parent.
 *
 * Returns true if @parent is a possible parent for @clk, false otherwise.
 */
bool clk_has_parent(struct clk *clk, struct clk *parent)
{
	struct clk_core *core, *parent_core;
	unsigned int i;

	/* NULL clocks should be nops, so return success if either is NULL. */
	if (!clk || !parent)
		return true;

	core = clk->core;
	parent_core = parent->core;

	/* Optimize for the case where the parent is already the parent. */
	if (core->parent == parent_core)
		return true;

	for (i = 0; i < core->num_parents; i++)
		if (strcmp(core->parent_names[i], parent_core->name) == 0)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(clk_has_parent);

static int clk_core_set_parent(struct clk_core *clk, struct clk_core *parent)
{
	int ret = 0;
	int p_index = 0;
	unsigned long p_rate = 0;

	if (!clk)
		return 0;

	/* verify ops for for multi-parent clks */
	if ((clk->num_parents > 1) && (!clk->ops->set_parent))
		return -ENOSYS;

	/* prevent racing with updates to the clock topology */
	clk_prepare_lock();

	if (clk->parent == parent)
		goto out;

	/* check that we are allowed to re-parent if the clock is in use */
	if ((clk->flags & CLK_SET_PARENT_GATE) && clk->prepare_count) {
		ret = -EBUSY;
		goto out;
	}

	/* try finding the new parent index */
	if (parent) {
		p_index = clk_fetch_parent_index(clk, parent);
		p_rate = parent->rate;
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
					__func__, parent->name, clk->name);
			ret = p_index;
			goto out;
		}
	}

	/* propagate PRE_RATE_CHANGE notifications */
	ret = __clk_speculate_rates(clk, p_rate);

	/* abort if a driver objects */
	if (ret & NOTIFY_STOP_MASK)
		goto out;

	/* do the re-parent */
	ret = __clk_set_parent(clk, parent, p_index);

	/* propagate rate an accuracy recalculation accordingly */
	if (ret) {
		__clk_recalc_rates(clk, ABORT_RATE_CHANGE);
	} else {
		__clk_recalc_rates(clk, POST_RATE_CHANGE);
		__clk_recalc_accuracies(clk);
	}

out:
	clk_prepare_unlock();

	return ret;
}

/**
 * clk_set_parent - switch the parent of a mux clk
 * @clk: the mux clk whose input we are switching
 * @parent: the new input to clk
 *
 * Re-parent clk to use parent as its new input source.  If clk is in
 * prepared state, the clk will get enabled for the duration of this call. If
 * that's not acceptable for a specific clk (Eg: the consumer can't handle
 * that, the reparenting is glitchy in hardware, etc), use the
 * CLK_SET_PARENT_GATE flag to allow reparenting only when clk is unprepared.
 *
 * After successfully changing clk's parent clk_set_parent will update the
 * clk topology, sysfs topology and propagate rate recalculation via
 * __clk_recalc_rates.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_set_parent(struct clk *clk, struct clk *parent)
{
	if (!clk)
		return 0;

	return clk_core_set_parent(clk->core, parent ? parent->core : NULL);
}
EXPORT_SYMBOL_GPL(clk_set_parent);

/**
 * clk_set_phase - adjust the phase shift of a clock signal
 * @clk: clock signal source
 * @degrees: number of degrees the signal is shifted
 *
 * Shifts the phase of a clock signal by the specified
 * degrees. Returns 0 on success, -EERROR otherwise.
 *
 * This function makes no distinction about the input or reference
 * signal that we adjust the clock signal phase against. For example
 * phase locked-loop clock signal generators we may shift phase with
 * respect to feedback clock signal input, but for other cases the
 * clock phase may be shifted with respect to some other, unspecified
 * signal.
 *
 * Additionally the concept of phase shift does not propagate through
 * the clock tree hierarchy, which sets it apart from clock rates and
 * clock accuracy. A parent clock phase attribute does not have an
 * impact on the phase attribute of a child clock.
 */
int clk_set_phase(struct clk *clk, int degrees)
{
	int ret = 0;

	if (!clk)
		goto out;

	/* sanity check degrees */
	degrees %= 360;
	if (degrees < 0)
		degrees += 360;

	clk_prepare_lock();

	if (!clk->core->ops->set_phase)
		goto out_unlock;

	ret = clk->core->ops->set_phase(clk->core->hw, degrees);

	if (!ret)
		clk->core->phase = degrees;

out_unlock:
	clk_prepare_unlock();

out:
	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_phase);

static int clk_core_get_phase(struct clk_core *clk)
{
	int ret = 0;

	if (!clk)
		goto out;

	clk_prepare_lock();
	ret = clk->phase;
	clk_prepare_unlock();

out:
	return ret;
}
EXPORT_SYMBOL_GPL(clk_get_phase);

/**
 * clk_get_phase - return the phase shift of a clock signal
 * @clk: clock signal source
 *
 * Returns the phase shift of a clock node in degrees, otherwise returns
 * -EERROR.
 */
int clk_get_phase(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_get_phase(clk->core);
}

/**
 * clk_is_match - check if two clk's point to the same hardware clock
 * @p: clk compared against q
 * @q: clk compared against p
 *
 * Returns true if the two struct clk pointers both point to the same hardware
 * clock node. Put differently, returns true if struct clk *p and struct clk *q
 * share the same struct clk_core object.
 *
 * Returns false otherwise. Note that two NULL clks are treated as matching.
 */
bool clk_is_match(const struct clk *p, const struct clk *q)
{
	/* trivial case: identical struct clk's or both NULL */
	if (p == q)
		return true;

	/* true if clk->core pointers match. Avoid derefing garbage */
	if (!IS_ERR_OR_NULL(p) && !IS_ERR_OR_NULL(q))
		if (p->core == q->core)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(clk_is_match);

/**
 * __clk_init - initialize the data structures in a struct clk
 * @dev:	device initializing this clk, placeholder for now
 * @clk:	clk being initialized
 *
 * Initializes the lists in struct clk_core, queries the hardware for the
 * parent and rate and sets them both.
 */
static int __clk_init(struct device *dev, struct clk *clk_user)
{
	int i, ret = 0;
	struct clk_core *orphan;
	struct hlist_node *tmp2;
	struct clk_core *clk;
	unsigned long rate;

	if (!clk_user)
		return -EINVAL;

	clk = clk_user->core;

	clk_prepare_lock();

	/* check to see if a clock with this name is already registered */

/* IAMROOT-12:
 * -------------
 * 모든 클럭 트리에서 clk_core->name으로 검색하여 이미 클럭명이 지정된 경우 
 * 초기화가 된것으로 간주하여 함수를 빠져나간다.
 * (루트 클럭 리스트 & 코아 클럭 리스트에서 검색)
 */
	if (clk_core_lookup(clk->name)) {
		pr_debug("%s: clk %s already initialized\n",
				__func__, clk->name);
		ret = -EEXIST;
		goto out;
	}

	/* check that clk_ops are sane.  See Documentation/clk.txt */

/* IAMROOT-12:
 * -------------
 * (*set_rate)를 제공한 클럭은 (*round_rate) 또는 (*determine_rate) 함수가
 * (*recalc_rate) 함수와 같이 제공되어야 한다.
 *
 * (*set_rate)를 제공하는 클럭은 다음 3가지 기본 타입이 있다.
 *   fixed-factor, divider, fractional-divider, (composite)
 */
	if (clk->ops->set_rate &&
	    !((clk->ops->round_rate || clk->ops->determine_rate) &&
	      clk->ops->recalc_rate)) {
		pr_warning("%s: %s must implement .round_rate or .determine_rate in addition to .recalc_rate\n",
				__func__, clk->name);
		ret = -EINVAL;
		goto out;
	}

/* IAMROOT-12:
 * -------------
 * (*set_parent)를 제공한 클럭은 (*get_parent)도 제공해야 한다.
 *
 * (*set_parent)를 제공하는 클럭은 mux 타입 클럭이다. (composite 포함)
 */
	if (clk->ops->set_parent && !clk->ops->get_parent) {
		pr_warning("%s: %s must implement .get_parent & .set_parent\n",
				__func__, clk->name);
		ret = -EINVAL;
		goto out;
	}

/* IAMROOT-12:
 * -------------
 * (*set_rate_and_parent) 함수가 제공된 경우 (*set_parent)와 (*set_rate) 함수도
 * 제공해야 한다.
 *
 * (*set_rate_and_parent)를 제공하는 클럭은 composite 타입 클럭이다.
 */
	if (clk->ops->set_rate_and_parent &&
			!(clk->ops->set_parent && clk->ops->set_rate)) {
		pr_warn("%s: %s must implement .set_parent & .set_rate\n",
				__func__, clk->name);
		ret = -EINVAL;
		goto out;
	}

	/* throw a WARN if any entries in parent_names are NULL */

/* IAMROOT-12:
 * -------------
 * 부모 클럭명들 중 하나라도 null인 클럭은 경고 메시지를 출력한다.
 *
 * (디바이스 트리 구성 시 스펠링 오류 또는 구성 오류 시 발생할 수 있다.)
 */
	for (i = 0; i < clk->num_parents; i++)
		WARN(!clk->parent_names[i],
				"%s: invalid NULL in %s's .parent_names\n",
				__func__, clk->name);

	/*
	 * Allocate an array of struct clk *'s to avoid unnecessary string
	 * look-ups of clk's possible parents.  This can fail for clocks passed
	 * in to clk_init during early boot; thus any access to clk->parents[]
	 * must always check for a NULL pointer and try to populate it if
	 * necessary.
	 *
	 * If clk->parents is not NULL we skip this entire block.  This allows
	 * for clock drivers to statically initialize clk->parents.
	 */

/* IAMROOT-12:
 * -------------
 * mux(멀티플렉서) 타입 클럭 장치에 부모 클럭 배열이 구성되지 않은 경우 할당한다.
 *
 * 부모가 하나일 경우 clk_core->parents는 부모 클럭을 가리키지만 
 * 부모가 둘 이상일 경우 배열로 사용한다.
 */
	if (clk->num_parents > 1 && !clk->parents) {
		clk->parents = kcalloc(clk->num_parents, sizeof(struct clk *),
					GFP_KERNEL);
		/*
		 * clk_core_lookup returns NULL for parents that have not been
		 * clk_init'd; thus any access to clk->parents[] must check
		 * for a NULL pointer.  We can always perform lazy lookups for
		 * missing parents later on.
		 */

/* IAMROOT-12:
 * -------------
 * 부모 클럭명으로 부모클럭을 찾아 연결한다.
 */
		if (clk->parents)
			for (i = 0; i < clk->num_parents; i++)
				clk->parents[i] =
					clk_core_lookup(clk->parent_names[i]);
	}


/* IAMROOT-12:
 * -------------
 * 부모 클럭을 찾아서 연결한다.
 */
	clk->parent = __clk_init_parent(clk);

	/*
	 * Populate clk->parent if parent has already been __clk_init'd.  If
	 * parent has not yet been __clk_init'd then place clk in the orphan
	 * list.  If clk has set the CLK_IS_ROOT flag then place it in the root
	 * clk list.
	 *
	 * Every time a new clk is clk_init'd then we walk the list of orphan
	 * clocks and re-parent any that are children of the clock currently
	 * being clk_init'd.
	 */

/* IAMROOT-12:
 * -------------
 * 부모가 선택된 경우 이 클럭을 부모클럭의 자식으로 등록한다.
 * 부모가 없이 루트 노드인 경우 루트 노드만 있는 리스트에 등록하고,
 * 그렇지 않은 경우 고아 노드만 있는 리스트에 등록한다.
 */
	if (clk->parent)
		hlist_add_head(&clk->child_node,
				&clk->parent->children);
	else if (clk->flags & CLK_IS_ROOT)
		hlist_add_head(&clk->child_node, &clk_root_list);
	else
		hlist_add_head(&clk->child_node, &clk_orphan_list);

	/*
	 * Set clk's accuracy.  The preferred method is to use
	 * .recalc_accuracy. For simple clocks and lazy developers the default
	 * fallback is to use the parent's accuracy.  If a clock doesn't have a
	 * parent (or is orphaned) then accuracy is set to zero (perfect
	 * clock).
	 */

/* IAMROOT-12:
 * -------------
 * (*recalc_accuracy)가 등록된 경우 해당 함수를 호출할 때 부모의 정확도를 
 * 사용하여 함수를 호출한다.
 *
 * 등록된 함수가 없는 경우 부모의 정확도를 사용하고 그렇지 않으면 0을 대입한다.
 * (0->perfect 클럭: 원하는 rate와 동일하게 설정된 경우로 추정됨)
 *
 * (*recalc_accuracy)의 구현은 fixed-rate 타입의 함수에서 구현되어 있다.
 */
	if (clk->ops->recalc_accuracy)
		clk->accuracy = clk->ops->recalc_accuracy(clk->hw,
					__clk_get_accuracy(clk->parent));
	else if (clk->parent)
		clk->accuracy = clk->parent->accuracy;
	else
		clk->accuracy = 0;

	/*
	 * Set clk's phase.
	 * Since a phase is by definition relative to its parent, just
	 * query the current clock phase, or just assume it's in phase.
	 */

/* IAMROOT-12:
 * -------------
 * (*get_phase)가 등록된 경우 위상 값을 알아온다. 
 * 함수가 제공되지 않는 경우 위상 값은 0도 이다. (0=match)
 *
 * 예) sunxi/clk-mod0.c & rockchip/clk-mmc-phase.c
 */
	if (clk->ops->get_phase)
		clk->phase = clk->ops->get_phase(clk->hw);
	else
		clk->phase = 0;

	/*
	 * Set clk's rate.  The preferred method is to use .recalc_rate.  For
	 * simple clocks and lazy developers the default fallback is to use the
	 * parent's rate.  If a clock doesn't have a parent (or is orphaned)
	 * then rate is set to zero.
	 */

/* IAMROOT-12:
 * -------------
 * (*recalc_rate)가 등록된 경우 부모 클럭 주파수 값으로 재계산한다.
 * 만일 함수가 제공되지 않는 경우 부모 클럭 주파수를 사용하고,
 * 부모가 없는 경우 0으로 한다.
 *
 * (*recalc_rate)가 없는 타입: gate, mux,  타입 
 *
 * fixed-factor 타입: clk_factor_recalc_rate()
 */
	if (clk->ops->recalc_rate)
		rate = clk->ops->recalc_rate(clk->hw,
				clk_core_get_rate_nolock(clk->parent));
	else if (clk->parent)
		rate = clk->parent->rate;
	else
		rate = 0;
	clk->rate = clk->req_rate = rate;

	/*
	 * walk the list of orphan clocks and reparent any that are children of
	 * this clock
	 */

/* IAMROOT-12:
 * -------------
 * 예) 고아 클럭의 부모가 이제야 등록되는 상황에서 고아 클럭 리스트들을 
 *     순서대로 처리하는데 
 *
 * case A) (*get_parent)가 제공되는 mux의 경우 
 *
 *     (*get_parent)하여 부모 인덱스 값을 한 번에 얻어오고 그 부모의 이름과
 *     등록할 클럭의 이름이 동일한 경우 고아 클럭의 부모로 지금 등록한 클럭을 
 *     지정한다.
 */
	hlist_for_each_entry_safe(orphan, tmp2, &clk_orphan_list, child_node) {
		if (orphan->num_parents && orphan->ops->get_parent) {
			i = orphan->ops->get_parent(orphan->hw);
			if (!strcmp(clk->name, orphan->parent_names[i]))
				clk_core_reparent(orphan, clk);
			continue;
		}


/* IAMROOT-12:
 * -------------
 * case B) (*get_parent)가 제공되지 않는 경우 
 *
 *      부모 수만큼 루프를 돌며 그 부모의 이름과 등록할 클럭의 이름이 동일한 
 *      경우 고아 클럭의 부모로 지금 등록한 클럭을 지정한다.
 */
		for (i = 0; i < orphan->num_parents; i++)
			if (!strcmp(clk->name, orphan->parent_names[i])) {
				clk_core_reparent(orphan, clk);
				break;
			}
	 }

	/*
	 * optional platform-specific magic
	 *
	 * The .init callback is not used by any of the basic clock types, but
	 * exists for weird hardware that must perform initialization magic.
	 * Please consider other ways of solving initialization problems before
	 * using this callback, as its use is discouraged.
	 */

/* IAMROOT-12:
 * -------------
 * 플랫폼 종속 코드가 잇는 경우 해당 초기화 함수를 수행한다.
 */
	if (clk->ops->init)
		clk->ops->init(clk->hw);

	kref_init(&clk->ref);
out:
	clk_prepare_unlock();

	if (!ret)
		clk_debug_register(clk);

	return ret;
}

struct clk *__clk_create_clk(struct clk_hw *hw, const char *dev_id,
			     const char *con_id)
{
	struct clk *clk;

	/* This is to allow this function to be chained to others */
	if (!hw || IS_ERR(hw))
		return (struct clk *) hw;

/* IAMROOT-12:
 * -------------
 * clk 구조체를 할당받는다.
 */
	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->core = hw->core;
	clk->dev_id = dev_id;
	clk->con_id = con_id;
	clk->max_rate = ULONG_MAX;

	clk_prepare_lock();

/* IAMROOT-12:
 * -------------
 * clk_core->clks에 할당받은 clk 구조체를 등록한다.
 */
	hlist_add_head(&clk->child_node, &hw->core->clks);
	clk_prepare_unlock();

	return clk;
}

void __clk_free_clk(struct clk *clk)
{
	clk_prepare_lock();
	hlist_del(&clk->child_node);
	clk_prepare_unlock();

	kfree(clk);
}

/**
 * clk_register - allocate a new clock, register it and return an opaque cookie
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * clk_register is the primary interface for populating the clock tree with new
 * clock nodes.  It returns a pointer to the newly allocated struct clk which
 * cannot be dereferenced by driver code but may be used in conjuction with the
 * rest of the clock API.  In the event of an error clk_register will return an
 * error code; drivers must test for an error code after calling clk_register.
 */
struct clk *clk_register(struct device *dev, struct clk_hw *hw)
{
	int i, ret;
	struct clk_core *clk;

/* IAMROOT-12:
 * -------------
 * clk_core 구조체를 할당받는다.
 */
	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk) {
		pr_err("%s: could not allocate clk\n", __func__);
		ret = -ENOMEM;
		goto fail_out;
	}

	clk->name = kstrdup_const(hw->init->name, GFP_KERNEL);
	if (!clk->name) {
		pr_err("%s: could not allocate clk->name\n", __func__);
		ret = -ENOMEM;
		goto fail_name;
	}
	clk->ops = hw->init->ops;
	if (dev && dev->driver)
		clk->owner = dev->driver->owner;
	clk->hw = hw;
	clk->flags = hw->init->flags;
	clk->num_parents = hw->init->num_parents;
	hw->core = clk;

	/* allocate local copy in case parent_names is __initdata */

/* IAMROOT-12:
 * -------------
 * num_parents 갯수만큼 문자열(부모클럭명)을 가리키는 포인터 배열을 할당한다.
 * (num_parents = 4, 4개의 포인터(4/8 바이트)만큼 배열이 만들어진다.
 *
 * fixed-rate 타입 클럭은 부모가 없으므로 null이다.
 * fixed-factor 타입 클럭은 num_parents=1이다.
 * mux 타입 클럭은 num_parents > 1 이다.
 */
	clk->parent_names = kcalloc(clk->num_parents, sizeof(char *),
					GFP_KERNEL);

	if (!clk->parent_names) {
		pr_err("%s: could not allocate clk->parent_names\n", __func__);
		ret = -ENOMEM;
		goto fail_parent_names;
	}


	/* copy each string name in case parent_names is __initdata */

/* IAMROOT-12:
 * -------------
 * 부모클럭에 대한 문자열을 복사하여 연결한다.
 */
	for (i = 0; i < clk->num_parents; i++) {
		clk->parent_names[i] = kstrdup_const(hw->init->parent_names[i],
						GFP_KERNEL);
		if (!clk->parent_names[i]) {
			pr_err("%s: could not copy parent_names\n", __func__);
			ret = -ENOMEM;
			goto fail_parent_names_copy;
		}
	}

/* IAMROOT-12:
 * -------------
 * clk_core->clks 리스트를 초기화하고 clk 구조체 하나를 할당받아 등록한다.
 */
	INIT_HLIST_HEAD(&clk->clks);

	hw->clk = __clk_create_clk(hw, NULL, NULL);
	if (IS_ERR(hw->clk)) {
		pr_err("%s: could not allocate per-user clk\n", __func__);
		ret = PTR_ERR(hw->clk);
		goto fail_parent_names_copy;
	}

	ret = __clk_init(dev, hw->clk);
	if (!ret)
		return hw->clk;

	__clk_free_clk(hw->clk);
	hw->clk = NULL;

fail_parent_names_copy:
	while (--i >= 0)
		kfree_const(clk->parent_names[i]);
	kfree(clk->parent_names);
fail_parent_names:
	kfree_const(clk->name);
fail_name:
	kfree(clk);
fail_out:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(clk_register);

/*
 * Free memory allocated for a clock.
 * Caller must hold prepare_lock.
 */
static void __clk_release(struct kref *ref)
{
	struct clk_core *clk = container_of(ref, struct clk_core, ref);
	int i = clk->num_parents;

	kfree(clk->parents);
	while (--i >= 0)
		kfree_const(clk->parent_names[i]);

	kfree(clk->parent_names);
	kfree_const(clk->name);
	kfree(clk);
}

/*
 * Empty clk_ops for unregistered clocks. These are used temporarily
 * after clk_unregister() was called on a clock and until last clock
 * consumer calls clk_put() and the struct clk object is freed.
 */
static int clk_nodrv_prepare_enable(struct clk_hw *hw)
{
	return -ENXIO;
}

static void clk_nodrv_disable_unprepare(struct clk_hw *hw)
{
	WARN_ON_ONCE(1);
}

static int clk_nodrv_set_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	return -ENXIO;
}

static int clk_nodrv_set_parent(struct clk_hw *hw, u8 index)
{
	return -ENXIO;
}

static const struct clk_ops clk_nodrv_ops = {
	.enable		= clk_nodrv_prepare_enable,
	.disable	= clk_nodrv_disable_unprepare,
	.prepare	= clk_nodrv_prepare_enable,
	.unprepare	= clk_nodrv_disable_unprepare,
	.set_rate	= clk_nodrv_set_rate,
	.set_parent	= clk_nodrv_set_parent,
};

/**
 * clk_unregister - unregister a currently registered clock
 * @clk: clock to unregister
 */
void clk_unregister(struct clk *clk)
{
	unsigned long flags;

	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_debug_unregister(clk->core);

	clk_prepare_lock();

	if (clk->core->ops == &clk_nodrv_ops) {
		pr_err("%s: unregistered clock: %s\n", __func__,
		       clk->core->name);
		return;
	}
	/*
	 * Assign empty clock ops for consumers that might still hold
	 * a reference to this clock.
	 */
	flags = clk_enable_lock();
	clk->core->ops = &clk_nodrv_ops;
	clk_enable_unlock(flags);

	if (!hlist_empty(&clk->core->children)) {
		struct clk_core *child;
		struct hlist_node *t;

		/* Reparent all children to the orphan list. */
		hlist_for_each_entry_safe(child, t, &clk->core->children,
					  child_node)
			clk_core_set_parent(child, NULL);
	}

	hlist_del_init(&clk->core->child_node);

	if (clk->core->prepare_count)
		pr_warn("%s: unregistering prepared clock: %s\n",
					__func__, clk->core->name);
	kref_put(&clk->core->ref, __clk_release);

	clk_prepare_unlock();
}
EXPORT_SYMBOL_GPL(clk_unregister);

static void devm_clk_release(struct device *dev, void *res)
{
	clk_unregister(*(struct clk **)res);
}

/**
 * devm_clk_register - resource managed clk_register()
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * Managed clk_register(). Clocks returned from this function are
 * automatically clk_unregister()ed on driver detach. See clk_register() for
 * more information.
 */
struct clk *devm_clk_register(struct device *dev, struct clk_hw *hw)
{
	struct clk *clk;
	struct clk **clkp;

	clkp = devres_alloc(devm_clk_release, sizeof(*clkp), GFP_KERNEL);
	if (!clkp)
		return ERR_PTR(-ENOMEM);

	clk = clk_register(dev, hw);
	if (!IS_ERR(clk)) {
		*clkp = clk;
		devres_add(dev, clkp);
	} else {
		devres_free(clkp);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(devm_clk_register);

static int devm_clk_match(struct device *dev, void *res, void *data)
{
	struct clk *c = res;
	if (WARN_ON(!c))
		return 0;
	return c == data;
}

/**
 * devm_clk_unregister - resource managed clk_unregister()
 * @clk: clock to unregister
 *
 * Deallocate a clock allocated with devm_clk_register(). Normally
 * this function will not need to be called and the resource management
 * code will ensure that the resource is freed.
 */
void devm_clk_unregister(struct device *dev, struct clk *clk)
{
	WARN_ON(devres_release(dev, devm_clk_release, devm_clk_match, clk));
}
EXPORT_SYMBOL_GPL(devm_clk_unregister);

/*
 * clkdev helpers
 */
int __clk_get(struct clk *clk)
{
	struct clk_core *core = !clk ? NULL : clk->core;

	if (core) {
		if (!try_module_get(core->owner))
			return 0;

		kref_get(&core->ref);
	}
	return 1;
}

void __clk_put(struct clk *clk)
{
	struct module *owner;

	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_prepare_lock();

	hlist_del(&clk->child_node);
	if (clk->min_rate > clk->core->req_rate ||
	    clk->max_rate < clk->core->req_rate)
		clk_core_set_rate_nolock(clk->core, clk->core->req_rate);

	owner = clk->core->owner;
	kref_put(&clk->core->ref, __clk_release);

	clk_prepare_unlock();

	module_put(owner);

	kfree(clk);
}

/***        clk rate change notifiers        ***/

/**
 * clk_notifier_register - add a clk rate change notifier
 * @clk: struct clk * to watch
 * @nb: struct notifier_block * with callback info
 *
 * Request notification when clk's rate changes.  This uses an SRCU
 * notifier because we want it to block and notifier unregistrations are
 * uncommon.  The callbacks associated with the notifier must not
 * re-enter into the clk framework by calling any top-level clk APIs;
 * this will cause a nested prepare_lock mutex.
 *
 * In all notification cases cases (pre, post and abort rate change) the
 * original clock rate is passed to the callback via struct
 * clk_notifier_data.old_rate and the new frequency is passed via struct
 * clk_notifier_data.new_rate.
 *
 * clk_notifier_register() must be called from non-atomic context.
 * Returns -EINVAL if called with null arguments, -ENOMEM upon
 * allocation failure; otherwise, passes along the return value of
 * srcu_notifier_chain_register().
 */
int clk_notifier_register(struct clk *clk, struct notifier_block *nb)
{
	struct clk_notifier *cn;
	int ret = -ENOMEM;

/* IAMROOT-12:
 * -------------
 * rate 변경 시 통지를 받아 확인을 해야 하는 경우 사용한다.
 */
	if (!clk || !nb)
		return -EINVAL;

	clk_prepare_lock();

	/* search the list of notifiers for this clk */
	list_for_each_entry(cn, &clk_notifier_list, node)
		if (cn->clk == clk)
			break;

	/* if clk wasn't in the notifier list, allocate new clk_notifier */
	if (cn->clk != clk) {
		cn = kzalloc(sizeof(struct clk_notifier), GFP_KERNEL);
		if (!cn)
			goto out;

		cn->clk = clk;
		srcu_init_notifier_head(&cn->notifier_head);

		list_add(&cn->node, &clk_notifier_list);
	}

	ret = srcu_notifier_chain_register(&cn->notifier_head, nb);

	clk->core->notifier_count++;

out:
	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_notifier_register);

/**
 * clk_notifier_unregister - remove a clk rate change notifier
 * @clk: struct clk *
 * @nb: struct notifier_block * with callback info
 *
 * Request no further notification for changes to 'clk' and frees memory
 * allocated in clk_notifier_register.
 *
 * Returns -EINVAL if called with null arguments; otherwise, passes
 * along the return value of srcu_notifier_chain_unregister().
 */
int clk_notifier_unregister(struct clk *clk, struct notifier_block *nb)
{
	struct clk_notifier *cn = NULL;
	int ret = -EINVAL;

	if (!clk || !nb)
		return -EINVAL;

	clk_prepare_lock();

	list_for_each_entry(cn, &clk_notifier_list, node)
		if (cn->clk == clk)
			break;

	if (cn->clk == clk) {
		ret = srcu_notifier_chain_unregister(&cn->notifier_head, nb);

		clk->core->notifier_count--;

		/* XXX the notifier code should handle this better */
		if (!cn->notifier_head.head) {
			srcu_cleanup_notifier_head(&cn->notifier_head);
			list_del(&cn->node);
			kfree(cn);
		}

	} else {
		ret = -ENOENT;
	}

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_notifier_unregister);

#ifdef CONFIG_OF
/**
 * struct of_clk_provider - Clock provider registration structure
 * @link: Entry in global list of clock providers
 * @node: Pointer to device tree node of clock provider
 * @get: Get clock callback.  Returns NULL or a struct clk for the
 *       given clock specifier
 * @data: context pointer to be passed into @get callback
 */

/* IAMROOT-12:
 * -------------
 * 클럭 프로바이더에 등록되는 구조체로 phandle 다음의 인수를 어떻게 parsing
 * 해야 하는지 알아낼 수 있는 핸들러 함수가 지정되어 있다. (*get)
 */
struct of_clk_provider {
	struct list_head link;

	struct device_node *node;
	struct clk *(*get)(struct of_phandle_args *clkspec, void *data);
	void *data;
};

static const struct of_device_id __clk_of_table_sentinel
	__used __section(__clk_of_table_end);

static LIST_HEAD(of_clk_providers);
static DEFINE_MUTEX(of_clk_mutex);

/* of_clk_provider list locking helpers */
void of_clk_lock(void)
{
	mutex_lock(&of_clk_mutex);
}

void of_clk_unlock(void)
{
	mutex_unlock(&of_clk_mutex);
}

/* IAMROOT-12:
 * -------------
 * 클럭 프로바이더에서 사용하는 인수 해석용 함수 중 하나 (*get)
 */
struct clk *of_clk_src_simple_get(struct of_phandle_args *clkspec,
				     void *data)
{
	return data;
}
EXPORT_SYMBOL_GPL(of_clk_src_simple_get);

/* IAMROOT-12:
 * -------------
 * phandle 값 다음의 정수를 클럭 배열의 인수로 사용하는 인수 해석용 함수 
 * (멀티 클럭에서 사용된다. 예) gate 클럭 등)
 *
 * clocks = <&abc_clock 3>;     <- clk_data->clks[3]을 의미 
 */
struct clk *of_clk_src_onecell_get(struct of_phandle_args *clkspec, void *data)
{
	struct clk_onecell_data *clk_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= clk_data->clk_num) {
		pr_err("%s: invalid clock index %d\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return clk_data->clks[idx];
}
EXPORT_SYMBOL_GPL(of_clk_src_onecell_get);

/**
 * of_clk_add_provider() - Register a clock provider for a node
 * @np: Device node pointer associated with clock provider
 * @clk_src_get: callback for decoding clock
 * @data: context pointer for @clk_src_get callback.
 */
int of_clk_add_provider(struct device_node *np,
			struct clk *(*clk_src_get)(struct of_phandle_args *clkspec,
						   void *data),
			void *data)
{
	struct of_clk_provider *cp;
	int ret;

/* IAMROOT-12:
 * -------------
 * of_clk_proivder 구조체를 할당 받고 인수들을 구조체에 설정하고 
 * of_clk_providers 리스트에 등록한다.
 */
	cp = kzalloc(sizeof(struct of_clk_provider), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->node = of_node_get(np);
	cp->data = data;
	cp->get = clk_src_get;

	mutex_lock(&of_clk_mutex);
	list_add(&cp->link, &of_clk_providers);
	mutex_unlock(&of_clk_mutex);
	pr_debug("Added clock from %s\n", np->full_name);

/* IAMROOT-12:
 * -------------
 * rate, accuracy 등을 기본 설정으로 구성한다.
 */
	ret = of_clk_set_defaults(np, true);
	if (ret < 0)
		of_clk_del_provider(np);

	return ret;
}
EXPORT_SYMBOL_GPL(of_clk_add_provider);

/**
 * of_clk_del_provider() - Remove a previously registered clock provider
 * @np: Device node pointer associated with clock provider
 */
void of_clk_del_provider(struct device_node *np)
{
	struct of_clk_provider *cp;

	mutex_lock(&of_clk_mutex);
	list_for_each_entry(cp, &of_clk_providers, link) {
		if (cp->node == np) {
			list_del(&cp->link);
			of_node_put(cp->node);
			kfree(cp);
			break;
		}
	}
	mutex_unlock(&of_clk_mutex);
}
EXPORT_SYMBOL_GPL(of_clk_del_provider);

struct clk *__of_clk_get_from_provider(struct of_phandle_args *clkspec,
				       const char *dev_id, const char *con_id)
{
	struct of_clk_provider *provider;
	struct clk *clk = ERR_PTR(-EPROBE_DEFER);

	/* Check if we have such a provider in our array */
	list_for_each_entry(provider, &of_clk_providers, link) {
		if (provider->node == clkspec->np)
			clk = provider->get(clkspec, provider->data);
		if (!IS_ERR(clk)) {
			clk = __clk_create_clk(__clk_get_hw(clk), dev_id,
					       con_id);

			if (!IS_ERR(clk) && !__clk_get(clk)) {
				__clk_free_clk(clk);
				clk = ERR_PTR(-ENOENT);
			}

			break;
		}
	}

	return clk;
}

struct clk *of_clk_get_from_provider(struct of_phandle_args *clkspec)
{
	struct clk *clk;

	mutex_lock(&of_clk_mutex);
	clk = __of_clk_get_from_provider(clkspec, NULL, __func__);
	mutex_unlock(&of_clk_mutex);

	return clk;
}

int of_clk_get_parent_count(struct device_node *np)
{
	return of_count_phandle_with_args(np, "clocks", "#clock-cells");
}
EXPORT_SYMBOL_GPL(of_clk_get_parent_count);

const char *of_clk_get_parent_name(struct device_node *np, int index)
{
	struct of_phandle_args clkspec;
	struct property *prop;
	const char *clk_name;
	const __be32 *vp;
	u32 pv;
	int rc;
	int count;

	if (index < 0)
		return NULL;

/* IAMROOT-12:
 * -------------
 * "clocks = <&c0 40>, <&c1 50>, ....;" <- phandle 값에서 index 번호의 클럭 
 * 노드를 찾아 clkspec을 산출해온다.
 *
 * 예) index=0
 *     clkspec = {np=&c0, args_count=1, args[]={40}}
 */
	rc = of_parse_phandle_with_args(np, "clocks", "#clock-cells", index,
					&clkspec);
	if (rc)
		return NULL;


/* IAMROOT-12:
 * -------------
 * 부모 클럭이 멀티 클럭인 경우 처음 클럭을 index 값으로 한다.
 *
 * #clock-cells = <1> 인 경우 멀티 클럭으로 클럭이 2개 이상이다.
 *                따라서 clock-output-names = "aaa", "bbb", ... 와 같이 
 *                2개 이상의 클럭명을 지정할 수 있다.
 *
 * "clock-indices = <0>, <1>, <3>, <5>, <8>, <12>, <13>, ...;"
 *
 * 부모 클럭 노드 {
 *                   clock-indices = <10>, <20>, <30>, <40>;
 *                   clock-output-names = "c1-10", "c1-20", "c1-30", "c1-40";
 * }
 *
 * 이 경우 index = 10이 된다.
 */
	index = clkspec.args_count ? clkspec.args[0] : 0;
	count = 0;

	/* if there is an indices property, use it to transfer the index
	 * specified into an array offset for the clock-output-names property.
	 */

/* IAMROOT-12:
 * -------------
 * 부모 클럭의 "clock-indices"에 있는 인덱스 값들에서 index와 동일한 경우의 
 * count 값을 찾는다. 
 *
 * 예) 위의 예에서 index=10인 경우 count=0이고 이 값을 다시 index에 대입 
 *     index <- 0
 */
	of_property_for_each_u32(clkspec.np, "clock-indices", prop, vp, pv) {
		if (index == pv) {
			index = count;
			break;
		}
		count++;
	}

/* IAMROOT-12:
 * -------------
 * 부모 클럭의 "clock-output-names" 속성 값에서 index 번째의 클럭명을 반환
 */
	if (of_property_read_string_index(clkspec.np, "clock-output-names",
					  index,
					  &clk_name) < 0)
		clk_name = clkspec.np->name;

	of_node_put(clkspec.np);
	return clk_name;
}
EXPORT_SYMBOL_GPL(of_clk_get_parent_name);

struct clock_provider {
	of_clk_init_cb_t clk_init_cb;
	struct device_node *np;
	struct list_head node;
};

static LIST_HEAD(clk_provider_list);

/*
 * This function looks for a parent clock. If there is one, then it
 * checks that the provider for this parent clock was initialized, in
 * this case the parent clock will be ready.
 */
static int parent_ready(struct device_node *np)
{
	int i = 0;

	while (true) {

/* IAMROOT-12:
 * -------------
 * i 인덱스의 부모 클럭을 찾는다.
 */
		struct clk *clk = of_clk_get(np, i);

		/* this parent is ready we can check the next one */
		if (!IS_ERR(clk)) {
			clk_put(clk);
			i++;
			continue;
		}

		/* at least one parent is not ready, we exit now */

/* IAMROOT-12:
 * -------------
 * 한 부모라도 초기화되지 않은 경우 0을 반환한다.
 */
		if (PTR_ERR(clk) == -EPROBE_DEFER)
			return 0;

		/*
		 * Here we make assumption that the device tree is
		 * written correctly. So an error means that there is
		 * no more parent. As we didn't exit yet, then the
		 * previous parent are ready. If there is no clock
		 * parent, no need to wait for them, then we can
		 * consider their absence as being ready
		 */

/* IAMROOT-12:
 * -------------
 * 모든 부모가 준비된 경우
 */
		return 1;
	}
}

/**
 * of_clk_init() - Scan and init clock providers from the DT
 * @matches: array of compatible values and init functions for providers.
 *
 * This function scans the device tree for matching clock providers
 * and calls their initialization functions. It also does it by trying
 * to follow the dependencies.
 */
void __init of_clk_init(const struct of_device_id *matches)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct clock_provider *clk_provider, *next;
	bool is_init_done;
	bool force = false;

	if (!matches)
		matches = &__clk_of_table;

	/* First prepare the list of the clocks providers */

/* IAMROOT-12:
 * -------------
 * 클럭 드라이버들(of_device_id 구조체가 __clk_of_table 섹션)의 compatible명과
 * 디바이스 트리의 compatible 명이 동일한 노드들에 대한 iteration
 *
 * clk_provider_list에 clock_provider를 구성한다.
 * (clock_provider는 device_node *와 초기화 함수로 구성된다)
 */
	for_each_matching_node_and_match(np, matches, &match) {
		struct clock_provider *parent =
			kzalloc(sizeof(struct clock_provider),	GFP_KERNEL);

		parent->clk_init_cb = match->data;
		parent->np = np;
		list_add_tail(&parent->node, &clk_provider_list);
	}

/* IAMROOT-12:
 * -------------
 * 가장 상위 부모부터 초기화를 수행한다. (dependency)
 * (루프 중 한 번도 초기화되지 않은 경우가 발생하면 최종적으로 초기화한다)
 */
	while (!list_empty(&clk_provider_list)) {
		is_init_done = false;
		list_for_each_entry_safe(clk_provider, next,
					&clk_provider_list, node) {
			if (force || parent_ready(clk_provider->np)) {

/* IAMROOT-12:
 * -------------
 * drivers/clk 디렉토리에 등록된 클럭 디바이스의 초기화 함수를 호출한다.
 * rpi2: "fixed-rate" 및 "fixed-factor" 드라이버를 사용한다.
 *       - drivers/clk/clk-fixed-rate.c - of_fixed_clk_setup() 
 *       - drivers/clk/clk-fixed-factor.c - of_fixed_factor_clk_setup() 
 */
				clk_provider->clk_init_cb(clk_provider->np);
				of_clk_set_defaults(clk_provider->np, true);

				list_del(&clk_provider->node);
				kfree(clk_provider);
				is_init_done = true;
			}
		}

		/*
		 * We didn't manage to initialize any of the
		 * remaining providers during the last loop, so now we
		 * initialize all the remaining ones unconditionally
		 * in case the clock parent was not mandatory
		 */
		if (!is_init_done)
			force = true;
	}
}
#endif
