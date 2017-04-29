/*
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Interrupt architecture for the GIC:
 *
 * o There is one Interrupt Distributor, which receives interrupts
 *   from system devices and sends them to the Interrupt Controllers.
 *
 * o There is one CPU Interface per CPU, which sends interrupts sent
 *   by the Distributor, and interrupts generated locally, to the
 *   associated CPU. The base address of the CPU interface is usually
 *   aliased so that the same address points to different chips depending
 *   on the CPU it is accessed from.
 *
 * Note that IRQs 0-31 are special - they are local to each CPU.
 * As such, the enable set/clear, pending set/clear and active bit
 * registers are banked per-cpu for these sources.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip/arm-gic.h>

#include <asm/cputype.h>
#include <asm/irq.h>
#include <asm/exception.h>
#include <asm/smp_plat.h>

#include "irq-gic-common.h"
#include "irqchip.h"

union gic_base {

/* IAMROOT-12:
 * -------------
 * *common_base: banked 레지스터들을 사용하는 경우 
 *               non banked 레지스터들을 사용하는 경우 cpu 0번의 
 *               base 주소를 담는다.
 * *percpu_base: non banked 레지스터들을 사용하는 경우
 */
	void __iomem *common_base;
	void __percpu * __iomem *percpu_base;
};

struct gic_chip_data {
	union gic_base dist_base;
	union gic_base cpu_base;
#ifdef CONFIG_CPU_PM
	u32 saved_spi_enable[DIV_ROUND_UP(1020, 32)];
	u32 saved_spi_conf[DIV_ROUND_UP(1020, 16)];
	u32 saved_spi_target[DIV_ROUND_UP(1020, 4)];
	u32 __percpu *saved_ppi_enable;
	u32 __percpu *saved_ppi_conf;
#endif
	struct irq_domain *domain;
	unsigned int gic_irqs;
#ifdef CONFIG_GIC_NON_BANKED

/* IAMROOT-12:
 * -------------
 * 뱅크되지 않은 레지스터들을 사용해야 하는 경우 (*get_base)를 
 * 사용하여 base 주소를 알아올 수 있다.
 */
	void __iomem *(*get_base)(union gic_base *);
#endif
};

static DEFINE_RAW_SPINLOCK(irq_controller_lock);

/*
 * The GIC mapping of CPU interfaces does not necessarily match
 * the logical CPU numbering.  Let's use a mapping as returned
 * by the GIC itself.
 */
#define NR_GIC_CPU_IF 8

/* IAMROOT-12:
 * -------------
 * gic_cpu_map[0] = 0b00000001 <- gic로 읽은 값
 * gic_cpu_map[1] = 0b00000010
 * gic_cpu_map[2] = 0b00000100
 * gic_cpu_map[3] = 0b00001000
 */

static u8 gic_cpu_map[NR_GIC_CPU_IF] __read_mostly;

/*
 * Supported arch specific GIC irq extension.
 * Default make them NULL.
 */
struct irq_chip gic_arch_extn = {
	.irq_eoi	= NULL,
	.irq_mask	= NULL,
	.irq_unmask	= NULL,
	.irq_retrigger	= NULL,
	.irq_set_type	= NULL,
	.irq_set_wake	= NULL,
};

#ifndef MAX_GIC_NR
#define MAX_GIC_NR	1
#endif


/* IAMROOT-12:
 * -------------
 * 최대 gic 갯수만큼 배열을 사용한다.
 */
static struct gic_chip_data gic_data[MAX_GIC_NR] __read_mostly;

#ifdef CONFIG_GIC_NON_BANKED
static void __iomem *gic_get_percpu_base(union gic_base *base)
{
	return raw_cpu_read(*base->percpu_base);
}

static void __iomem *gic_get_common_base(union gic_base *base)
{
	return base->common_base;
}

static inline void __iomem *gic_data_dist_base(struct gic_chip_data *data)
{
	return data->get_base(&data->dist_base);
}

static inline void __iomem *gic_data_cpu_base(struct gic_chip_data *data)
{
	return data->get_base(&data->cpu_base);
}

static inline void gic_set_base_accessor(struct gic_chip_data *data,
					 void __iomem *(*f)(union gic_base *))
{
	data->get_base = f;
}
#else
#define gic_data_dist_base(d)	((d)->dist_base.common_base)
#define gic_data_cpu_base(d)	((d)->cpu_base.common_base)
#define gic_set_base_accessor(d, f)
#endif

static inline void __iomem *gic_dist_base(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data_dist_base(gic_data);
}

static inline void __iomem *gic_cpu_base(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data_cpu_base(gic_data);
}

static inline unsigned int gic_irq(struct irq_data *d)
{
	return d->hwirq;
}

/*
 * Routines to acknowledge, disable and enable interrupts
 */
static void gic_mask_irq(struct irq_data *d)
{

/* IAMROOT-12:
 * -------------
 * GIC_DIST_ENABLE_CLEAR 레지스터들 중 해당 인터럽트에 해당하는 비트를 설정하여 
 * mask 처리한다.
 */
	u32 mask = 1 << (gic_irq(d) % 32);
	unsigned long flags;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	writel_relaxed(mask, gic_dist_base(d) + GIC_DIST_ENABLE_CLEAR + (gic_irq(d) / 32) * 4);
	if (gic_arch_extn.irq_mask)
		gic_arch_extn.irq_mask(d);
	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
}

static void gic_unmask_irq(struct irq_data *d)
{
/* IAMROOT-12:
 * -------------
 * GIC_DIST_ENABLE_SET 레지스터들 중 해당 인터럽트에 해당하는 비트를 설정하여 
 * unmask 처리한다.
 */
	u32 mask = 1 << (gic_irq(d) % 32);
	unsigned long flags;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	if (gic_arch_extn.irq_unmask)
		gic_arch_extn.irq_unmask(d);
	writel_relaxed(mask, gic_dist_base(d) + GIC_DIST_ENABLE_SET + (gic_irq(d) / 32) * 4);
	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
}

static void gic_eoi_irq(struct irq_data *d)
{

/* IAMROOT-12:
 * -------------
 * 해당 인터럽트의 처리가 완료되었음을 gic에게 알린다.
 */
	if (gic_arch_extn.irq_eoi) {
		raw_spin_lock(&irq_controller_lock);
		gic_arch_extn.irq_eoi(d);
		raw_spin_unlock(&irq_controller_lock);
	}

	writel_relaxed(gic_irq(d), gic_cpu_base(d) + GIC_CPU_EOI);
}

static int gic_set_type(struct irq_data *d, unsigned int type)
{
	void __iomem *base = gic_dist_base(d);
	unsigned int gicirq = gic_irq(d);
	unsigned long flags;
	int ret;

	/* Interrupt configuration for SGIs can't be changed */

/* IAMROOT-12:
 * -------------
 * 트리거 타입은 SGIs를 제외한 16번 부터 설정가능하다.
 */
	if (gicirq < 16)
		return -EINVAL;

	/* SPIs have restrictions on the supported types */
	if (gicirq >= 32 && type != IRQ_TYPE_LEVEL_HIGH &&
			    type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);

	if (gic_arch_extn.irq_set_type)
		gic_arch_extn.irq_set_type(d, type);

	ret = gic_configure_irq(gicirq, type, base, NULL);

	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);

	return ret;
}

static int gic_retrigger(struct irq_data *d)
{
	if (gic_arch_extn.irq_retrigger)
		return gic_arch_extn.irq_retrigger(d);

	/* the genirq layer expects 0 if we can't retrigger in hardware */
	return 0;
}

#ifdef CONFIG_SMP
static int gic_set_affinity(struct irq_data *d, const struct cpumask *mask_val,
			    bool force)
{
	void __iomem *reg = gic_dist_base(d) + GIC_DIST_TARGET + (gic_irq(d) & ~3);
	unsigned int cpu, shift = (gic_irq(d) % 4) * 8;
	u32 val, mask, bit;
	unsigned long flags;

/* IAMROOT-12:
 * -------------
 * 해당 인터럽트를 지정한 cpu들에서 사용할 수 있도록 설정한다.
 *
 * force=0: online cpu & mask_val 들 중 랜덤으로 하나를 가져온다. (0= cpu#0)
 * force=1: mask_val에서 가장 첫 cpu 
 */
	if (!force)
		cpu = cpumask_any_and(mask_val, cpu_online_mask);
	else
		cpu = cpumask_first(mask_val);

	if (cpu >= NR_GIC_CPU_IF || cpu >= nr_cpu_ids)
		return -EINVAL;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	mask = 0xff << shift;
	bit = gic_cpu_map[cpu] << shift;

/* IAMROOT-12:
 * -------------
 * 레지스터를 읽은 후 해당 인터럽트에 대한 cpu 비트들을 모두 클리어하고 
 * 다시 bit를 더해서 기록한다. (선택된 cpu의 gic_cpu_map[] cpumask로 설정)
 */
	val = readl_relaxed(reg) & ~mask;
	writel_relaxed(val | bit, reg);
	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);

	return IRQ_SET_MASK_OK;
}
#endif

#ifdef CONFIG_PM
static int gic_set_wake(struct irq_data *d, unsigned int on)
{
	int ret = -ENXIO;

	if (gic_arch_extn.irq_set_wake)
		ret = gic_arch_extn.irq_set_wake(d, on);

	return ret;
}

#else
#define gic_set_wake	NULL
#endif

static void __exception_irq_entry gic_handle_irq(struct pt_regs *regs)
{
	u32 irqstat, irqnr;
	struct gic_chip_data *gic = &gic_data[0];
	void __iomem *cpu_base = gic_data_cpu_base(gic);

	do {
		irqstat = readl_relaxed(cpu_base + GIC_CPU_INTACK);
		irqnr = irqstat & GICC_IAR_INT_ID_MASK;

		if (likely(irqnr > 15 && irqnr < 1021)) {

/* IAMROOT-12:
 * -------------
 * (*handle_arch_irq) -> gic_handle_irq() -> handle_domain_irq()
 */
			handle_domain_irq(gic->domain, irqnr, regs);
			continue;
		}
		if (irqnr < 16) {
			writel_relaxed(irqstat, cpu_base + GIC_CPU_EOI);
#ifdef CONFIG_SMP
			handle_IPI(irqnr, regs);
#endif
			continue;
		}
		break;
	} while (1);
}

static void gic_handle_cascade_irq(unsigned int irq, struct irq_desc *desc)
{
	struct gic_chip_data *chip_data = irq_get_handler_data(irq);
	struct irq_chip *chip = irq_get_chip(irq);
	unsigned int cascade_irq, gic_irq;
	unsigned long status;

	chained_irq_enter(chip, desc);

	raw_spin_lock(&irq_controller_lock);
	status = readl_relaxed(gic_data_cpu_base(chip_data) + GIC_CPU_INTACK);
	raw_spin_unlock(&irq_controller_lock);

	gic_irq = (status & GICC_IAR_INT_ID_MASK);
	if (gic_irq == GICC_INT_SPURIOUS)
		goto out;

	cascade_irq = irq_find_mapping(chip_data->domain, gic_irq);
	if (unlikely(gic_irq < 32 || gic_irq > 1020))
		handle_bad_irq(cascade_irq, desc);
	else
		generic_handle_irq(cascade_irq);

 out:
	chained_irq_exit(chip, desc);
}


/* IAMROOT-12:
 * -------------
 * gic 컨트롤러에는 irq_startup(), irq_enable() 등이 구현되어 있지 않고
 * 그냥 irq_unmask()만 가지고도 인터럽트가 개통된다.
 */
static struct irq_chip gic_chip = {
	.name			= "GIC",
	.irq_mask		= gic_mask_irq,
	.irq_unmask		= gic_unmask_irq,
	.irq_eoi		= gic_eoi_irq,
	.irq_set_type		= gic_set_type,
	.irq_retrigger		= gic_retrigger,
#ifdef CONFIG_SMP
	.irq_set_affinity	= gic_set_affinity,
#endif
	.irq_set_wake		= gic_set_wake,
};

void __init gic_cascade_irq(unsigned int gic_nr, unsigned int irq)
{
	if (gic_nr >= MAX_GIC_NR)
		BUG();
	if (irq_set_handler_data(irq, &gic_data[gic_nr]) != 0)
		BUG();
	irq_set_chained_handler(irq, gic_handle_cascade_irq);
}

static u8 gic_get_cpumask(struct gic_chip_data *gic)
{
	void __iomem *base = gic_data_dist_base(gic);
	u32 mask, i;


/* IAMROOT-12:
 * -------------
 * irq#0~irq#31에서 사용하는 cpu 마스크가 존재하는 경우 하위 8비트에 이동
 * 시켜 반환한다.
 */
	for (i = mask = 0; i < 32; i += 4) {
		mask = readl_relaxed(base + GIC_DIST_TARGET + i);
		mask |= mask >> 16;
		mask |= mask >> 8;
		if (mask)
			break;
	}

	if (!mask)
		pr_crit("GIC CPU mask not found - kernel will fail to boot.\n");

/* IAMROOT-12:
 * -------------
 * 8bit만 반환
 */
	return mask;
}

static void gic_cpu_if_up(void)
{
	void __iomem *cpu_base = gic_data_cpu_base(&gic_data[0]);
	u32 bypass = 0;

	/*
	* Preserve bypass disable bits to be written back later
	*/
	bypass = readl(cpu_base + GIC_CPU_CTRL);
	bypass &= GICC_DIS_BYPASS_MASK;

/* IAMROOT-12:
 * -------------
 * gic-v1에는 bypass 없음
 */
	writel_relaxed(bypass | GICC_ENABLE, cpu_base + GIC_CPU_CTRL);
}


static void __init gic_dist_init(struct gic_chip_data *gic)
{
	unsigned int i;
	u32 cpumask;
	unsigned int gic_irqs = gic->gic_irqs;
	void __iomem *base = gic_data_dist_base(gic);

/* IAMROOT-12:
 * -------------
 * GIC distributor의 controler 레지스터를 disable 처리한다.
 */
	writel_relaxed(GICD_DISABLE, base + GIC_DIST_CTRL);

	/*
	 * Set all global interrupts to this CPU only.
	 */

/* IAMROOT-12:
 * -------------
 * cpumask를 읽어와서 SPI에 해당하는 모든 인터럽트에 대해서 cpumask를 설정한다.
 * (Distributor의 Target 레지스터 사용)
 */
	cpumask = gic_get_cpumask(gic);
	cpumask |= cpumask << 8;
	cpumask |= cpumask << 16;
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(cpumask, base + GIC_DIST_TARGET + i * 4 / 4);

	gic_dist_config(base, gic_irqs, NULL);


/* IAMROOT-12:
 * -------------
 * 다시 GIC distributor의 controler 레지스터를 enable 처리한다.
 */
	writel_relaxed(GICD_ENABLE, base + GIC_DIST_CTRL);
}

static void gic_cpu_init(struct gic_chip_data *gic)
{
	void __iomem *dist_base = gic_data_dist_base(gic);
	void __iomem *base = gic_data_cpu_base(gic);
	unsigned int cpu_mask, cpu = smp_processor_id();
	int i;

	/*
	 * Get what the GIC says our CPU mask is.
	 */

/* IAMROOT-12:
 * -------------
 * gic의 cpu 마스크를 읽어와서 설정한다. (PPI에 해당하는 레지스터는 RO)
 * (8bit로 bit0=cpu0의 forward/discard를 결정하고, bit1=cpu1, ...
 */
	BUG_ON(cpu >= NR_GIC_CPU_IF);
	cpu_mask = gic_get_cpumask(gic);
	gic_cpu_map[cpu] = cpu_mask;

	/*
	 * Clear our mask from the other map entries in case they're
	 * still undefined.
	 */

/* IAMROOT-12:
 * -------------
 * 그 외의 cpu에서는 위에서 알아온 mask의 반대만 mask하여 사용한다.
 * 즉, 위에서 사용한 cpu 마스크는 제외시킨다.
 */
	for (i = 0; i < NR_GIC_CPU_IF; i++)
		if (i != cpu)
			gic_cpu_map[i] &= ~cpu_mask;

	gic_cpu_config(dist_base, NULL);

/* IAMROOT-12:
 * -------------
 * 우선순위 스레졸드를 0xf0으로 설정한다. 
 * priority 값이 낮을 수록 우선 순위가 높다. (0=highest, 255=lowest)
 */
	writel_relaxed(GICC_INT_PRI_THRESHOLD, base + GIC_CPU_PRIMASK);

/* IAMROOT-12:
 * -------------
 * cpu_if 컨틀롤러를 enable 한다.
 */
	gic_cpu_if_up();
}

void gic_cpu_if_down(void)
{
	void __iomem *cpu_base = gic_data_cpu_base(&gic_data[0]);
	u32 val = 0;

	val = readl(cpu_base + GIC_CPU_CTRL);
	val &= ~GICC_ENABLE;
	writel_relaxed(val, cpu_base + GIC_CPU_CTRL);
}

#ifdef CONFIG_CPU_PM
/*
 * Saves the GIC distributor registers during suspend or idle.  Must be called
 * with interrupts disabled but before powering down the GIC.  After calling
 * this function, no interrupts will be delivered by the GIC, and another
 * platform-specific wakeup source must be enabled.
 */
static void gic_dist_save(unsigned int gic_nr)
{
	unsigned int gic_irqs;
	void __iomem *dist_base;
	int i;

	if (gic_nr >= MAX_GIC_NR)
		BUG();

	gic_irqs = gic_data[gic_nr].gic_irqs;
	dist_base = gic_data_dist_base(&gic_data[gic_nr]);

	if (!dist_base)
		return;

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 16); i++)
		gic_data[gic_nr].saved_spi_conf[i] =
			readl_relaxed(dist_base + GIC_DIST_CONFIG + i * 4);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 4); i++)
		gic_data[gic_nr].saved_spi_target[i] =
			readl_relaxed(dist_base + GIC_DIST_TARGET + i * 4);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 32); i++)
		gic_data[gic_nr].saved_spi_enable[i] =
			readl_relaxed(dist_base + GIC_DIST_ENABLE_SET + i * 4);
}

/*
 * Restores the GIC distributor registers during resume or when coming out of
 * idle.  Must be called before enabling interrupts.  If a level interrupt
 * that occured while the GIC was suspended is still present, it will be
 * handled normally, but any edge interrupts that occured will not be seen by
 * the GIC and need to be handled by the platform-specific wakeup source.
 */
static void gic_dist_restore(unsigned int gic_nr)
{
	unsigned int gic_irqs;
	unsigned int i;
	void __iomem *dist_base;

	if (gic_nr >= MAX_GIC_NR)
		BUG();

	gic_irqs = gic_data[gic_nr].gic_irqs;
	dist_base = gic_data_dist_base(&gic_data[gic_nr]);

	if (!dist_base)
		return;

	writel_relaxed(GICD_DISABLE, dist_base + GIC_DIST_CTRL);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 16); i++)
		writel_relaxed(gic_data[gic_nr].saved_spi_conf[i],
			dist_base + GIC_DIST_CONFIG + i * 4);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 4); i++)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
			dist_base + GIC_DIST_PRI + i * 4);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 4); i++)
		writel_relaxed(gic_data[gic_nr].saved_spi_target[i],
			dist_base + GIC_DIST_TARGET + i * 4);

	for (i = 0; i < DIV_ROUND_UP(gic_irqs, 32); i++)
		writel_relaxed(gic_data[gic_nr].saved_spi_enable[i],
			dist_base + GIC_DIST_ENABLE_SET + i * 4);

	writel_relaxed(GICD_ENABLE, dist_base + GIC_DIST_CTRL);
}

static void gic_cpu_save(unsigned int gic_nr)
{
	int i;
	u32 *ptr;
	void __iomem *dist_base;
	void __iomem *cpu_base;

	if (gic_nr >= MAX_GIC_NR)
		BUG();

	dist_base = gic_data_dist_base(&gic_data[gic_nr]);
	cpu_base = gic_data_cpu_base(&gic_data[gic_nr]);

	if (!dist_base || !cpu_base)
		return;

	ptr = raw_cpu_ptr(gic_data[gic_nr].saved_ppi_enable);
	for (i = 0; i < DIV_ROUND_UP(32, 32); i++)
		ptr[i] = readl_relaxed(dist_base + GIC_DIST_ENABLE_SET + i * 4);

	ptr = raw_cpu_ptr(gic_data[gic_nr].saved_ppi_conf);
	for (i = 0; i < DIV_ROUND_UP(32, 16); i++)
		ptr[i] = readl_relaxed(dist_base + GIC_DIST_CONFIG + i * 4);

}

static void gic_cpu_restore(unsigned int gic_nr)
{
	int i;
	u32 *ptr;
	void __iomem *dist_base;
	void __iomem *cpu_base;

	if (gic_nr >= MAX_GIC_NR)
		BUG();

	dist_base = gic_data_dist_base(&gic_data[gic_nr]);
	cpu_base = gic_data_cpu_base(&gic_data[gic_nr]);

	if (!dist_base || !cpu_base)
		return;

	ptr = raw_cpu_ptr(gic_data[gic_nr].saved_ppi_enable);
	for (i = 0; i < DIV_ROUND_UP(32, 32); i++)
		writel_relaxed(ptr[i], dist_base + GIC_DIST_ENABLE_SET + i * 4);

	ptr = raw_cpu_ptr(gic_data[gic_nr].saved_ppi_conf);
	for (i = 0; i < DIV_ROUND_UP(32, 16); i++)
		writel_relaxed(ptr[i], dist_base + GIC_DIST_CONFIG + i * 4);

	for (i = 0; i < DIV_ROUND_UP(32, 4); i++)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
					dist_base + GIC_DIST_PRI + i * 4);

	writel_relaxed(GICC_INT_PRI_THRESHOLD, cpu_base + GIC_CPU_PRIMASK);
	gic_cpu_if_up();
}

static int gic_notifier(struct notifier_block *self, unsigned long cmd,	void *v)
{
	int i;

	for (i = 0; i < MAX_GIC_NR; i++) {
#ifdef CONFIG_GIC_NON_BANKED
		/* Skip over unused GICs */
		if (!gic_data[i].get_base)
			continue;
#endif
		switch (cmd) {
		case CPU_PM_ENTER:
			gic_cpu_save(i);
			break;
		case CPU_PM_ENTER_FAILED:
		case CPU_PM_EXIT:
			gic_cpu_restore(i);
			break;
		case CPU_CLUSTER_PM_ENTER:
			gic_dist_save(i);
			break;
		case CPU_CLUSTER_PM_ENTER_FAILED:
		case CPU_CLUSTER_PM_EXIT:
			gic_dist_restore(i);
			break;
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block gic_notifier_block = {
	.notifier_call = gic_notifier,
};

static void __init gic_pm_init(struct gic_chip_data *gic)
{

/* IAMROOT-12:
 * -------------
 * 4 byte를 per-cpu로 할당한다.
 */
	gic->saved_ppi_enable = __alloc_percpu(DIV_ROUND_UP(32, 32) * 4,
		sizeof(u32));
	BUG_ON(!gic->saved_ppi_enable);


/* IAMROOT-12:
 * -------------
 * 8 byte를 할당한다. (4 바이트 정렬)
 */
	gic->saved_ppi_conf = __alloc_percpu(DIV_ROUND_UP(32, 16) * 4,
		sizeof(u32));
	BUG_ON(!gic->saved_ppi_conf);


/* IAMROOT-12:
 * -------------
 * 현재 컨트롤러가 gic의 첫번째 컨트롤러인 경우 cpu_pm notify 등록한다.
 * (power monitor)
 */
	if (gic == &gic_data[0])
		cpu_pm_register_notifier(&gic_notifier_block);
}
#else
static void __init gic_pm_init(struct gic_chip_data *gic)
{
}
#endif

#ifdef CONFIG_SMP
static void gic_raise_softirq(const struct cpumask *mask, unsigned int irq)
{
	int cpu;
	unsigned long flags, map = 0;

	raw_spin_lock_irqsave(&irq_controller_lock, flags);

	/* Convert our logical CPU mask into a physical one. */
	for_each_cpu(cpu, mask)
		map |= gic_cpu_map[cpu];

	/*
	 * Ensure that stores to Normal memory are visible to the
	 * other CPUs before they observe us issuing the IPI.
	 */
	dmb(ishst);

	/* this always happens on GIC0 */
	writel_relaxed(map << 16 | irq, gic_data_dist_base(&gic_data[0]) + GIC_DIST_SOFTINT);

	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
}
#endif

#ifdef CONFIG_BL_SWITCHER
/*
 * gic_send_sgi - send a SGI directly to given CPU interface number
 *
 * cpu_id: the ID for the destination CPU interface
 * irq: the IPI number to send a SGI for
 */
void gic_send_sgi(unsigned int cpu_id, unsigned int irq)
{
	BUG_ON(cpu_id >= NR_GIC_CPU_IF);
	cpu_id = 1 << cpu_id;
	/* this always happens on GIC0 */
	writel_relaxed((cpu_id << 16) | irq, gic_data_dist_base(&gic_data[0]) + GIC_DIST_SOFTINT);
}

/*
 * gic_get_cpu_id - get the CPU interface ID for the specified CPU
 *
 * @cpu: the logical CPU number to get the GIC ID for.
 *
 * Return the CPU interface ID for the given logical CPU number,
 * or -1 if the CPU number is too large or the interface ID is
 * unknown (more than one bit set).
 */
int gic_get_cpu_id(unsigned int cpu)
{
	unsigned int cpu_bit;

	if (cpu >= NR_GIC_CPU_IF)
		return -1;
	cpu_bit = gic_cpu_map[cpu];
	if (cpu_bit & (cpu_bit - 1))
		return -1;
	return __ffs(cpu_bit);
}

/*
 * gic_migrate_target - migrate IRQs to another CPU interface
 *
 * @new_cpu_id: the CPU target ID to migrate IRQs to
 *
 * Migrate all peripheral interrupts with a target matching the current CPU
 * to the interface corresponding to @new_cpu_id.  The CPU interface mapping
 * is also updated.  Targets to other CPU interfaces are unchanged.
 * This must be called with IRQs locally disabled.
 */
void gic_migrate_target(unsigned int new_cpu_id)
{
	unsigned int cur_cpu_id, gic_irqs, gic_nr = 0;
	void __iomem *dist_base;
	int i, ror_val, cpu = smp_processor_id();
	u32 val, cur_target_mask, active_mask;

	if (gic_nr >= MAX_GIC_NR)
		BUG();

	dist_base = gic_data_dist_base(&gic_data[gic_nr]);
	if (!dist_base)
		return;
	gic_irqs = gic_data[gic_nr].gic_irqs;

	cur_cpu_id = __ffs(gic_cpu_map[cpu]);
	cur_target_mask = 0x01010101 << cur_cpu_id;
	ror_val = (cur_cpu_id - new_cpu_id) & 31;

	raw_spin_lock(&irq_controller_lock);

	/* Update the target interface for this logical CPU */
	gic_cpu_map[cpu] = 1 << new_cpu_id;

	/*
	 * Find all the peripheral interrupts targetting the current
	 * CPU interface and migrate them to the new CPU interface.
	 * We skip DIST_TARGET 0 to 7 as they are read-only.
	 */
	for (i = 8; i < DIV_ROUND_UP(gic_irqs, 4); i++) {
		val = readl_relaxed(dist_base + GIC_DIST_TARGET + i * 4);
		active_mask = val & cur_target_mask;
		if (active_mask) {
			val &= ~active_mask;
			val |= ror32(active_mask, ror_val);
			writel_relaxed(val, dist_base + GIC_DIST_TARGET + i*4);
		}
	}

	raw_spin_unlock(&irq_controller_lock);

	/*
	 * Now let's migrate and clear any potential SGIs that might be
	 * pending for us (cur_cpu_id).  Since GIC_DIST_SGI_PENDING_SET
	 * is a banked register, we can only forward the SGI using
	 * GIC_DIST_SOFTINT.  The original SGI source is lost but Linux
	 * doesn't use that information anyway.
	 *
	 * For the same reason we do not adjust SGI source information
	 * for previously sent SGIs by us to other CPUs either.
	 */
	for (i = 0; i < 16; i += 4) {
		int j;
		val = readl_relaxed(dist_base + GIC_DIST_SGI_PENDING_SET + i);
		if (!val)
			continue;
		writel_relaxed(val, dist_base + GIC_DIST_SGI_PENDING_CLEAR + i);
		for (j = i; j < i + 4; j++) {
			if (val & 0xff)
				writel_relaxed((1 << (new_cpu_id + 16)) | j,
						dist_base + GIC_DIST_SOFTINT);
			val >>= 8;
		}
	}
}

/*
 * gic_get_sgir_physaddr - get the physical address for the SGI register
 *
 * REturn the physical address of the SGI register to be used
 * by some early assembly code when the kernel is not yet available.
 */
static unsigned long gic_dist_physaddr;

unsigned long gic_get_sgir_physaddr(void)
{
	if (!gic_dist_physaddr)
		return 0;
	return gic_dist_physaddr + GIC_DIST_SOFTINT;
}

void __init gic_init_physaddr(struct device_node *node)
{
	struct resource res;
	if (of_address_to_resource(node, 0, &res) == 0) {
		gic_dist_physaddr = res.start;
		pr_info("GIC physical location is %#lx\n", gic_dist_physaddr);
	}
}

#else
#define gic_init_physaddr(node)  do { } while (0)
#endif

static int gic_irq_domain_map(struct irq_domain *d, unsigned int irq,
				irq_hw_number_t hw)
{

/* IAMROOT-12:
 * -------------
 * PPIs: 32 미만의 hwirq에 대해서 percpu 디바이스로 플래그를 설정한다.
 */
	if (hw < 32) {
		irq_set_percpu_devid(irq);

/* IAMROOT-12:
 * -------------
 * 내장 인터럽트 핸들러로 percpu_devid를 다루는 핸들러로 설정한다.
 */
		irq_domain_set_info(d, irq, hw, &gic_chip, d->host_data,
				    handle_percpu_devid_irq, NULL, NULL);
		set_irq_flags(irq, IRQF_VALID | IRQF_NOAUTOEN);
	} else {

/* IAMROOT-12:
 * -------------
 * SPIs: 32개를 제외한 나머지 hwirq에 대해서 내장 인터럽트 핸들러로 
 * fasteoi 핸들러로 설정한다.
 */
		irq_domain_set_info(d, irq, hw, &gic_chip, d->host_data,
				    handle_fasteoi_irq, NULL, NULL);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);

/* IAMROOT-12:
 * -------------
 * gic_routable_irq_domain_map() 함수를 호출한다. (gic는 0만 반환)
 */
		gic_routable_irq_domain_ops->map(d, irq, hw);
	}
	return 0;
}

static void gic_irq_domain_unmap(struct irq_domain *d, unsigned int irq)
{
	gic_routable_irq_domain_ops->unmap(d, irq);
}


/* IAMROOT-12:
 * -------------
 * 성공시 0을 반환한다.
 */
static int gic_irq_domain_xlate(struct irq_domain *d,
				struct device_node *controller,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq, unsigned int *out_type)
{
	unsigned long ret = 0;

/* IAMROOT-12:
 * -------------
 * 현재 디바이스 노드가 요청한 디바이스인 경우가 아니면 -EINVAL 반환
 */
	if (d->of_node != controller)
		return -EINVAL;

/* IAMROOT-12:
 * -------------
 * 인수가 3개보다 적으면 -EINVAL 반환 
 *    예) arch/arm/boot/dts/omap4.dtsi - gic를 사용하는 uart1 디바이스
 *        interrupts = <GIC_SPI 72 IRQ_TYPE_LEVEL_HIGH>;
 */
	if (intsize < 3)
		return -EINVAL;

	/* Get the interrupt number and add 16 to skip over SGIs */

/* IAMROOT-12:
 * -------------
 * hwirq <- 두번째 인수 값 + 16  (위의 uart1의 경우 72 + 16)
 */
	*out_hwirq = intspec[1] + 16;

	/* For SPIs, we need to add 16 more to get the GIC irq ID number */

/* IAMROOT-12:
 * -------------
 * 첫 번째 인수가 0인 경우 out_hwirq에 16을 더해서 산출한다.
 */
	if (!intspec[0]) {
		ret = gic_routable_irq_domain_ops->xlate(d, controller,
							 intspec,
							 intsize,
							 out_hwirq,
							 out_type);

		if (IS_ERR_VALUE(ret))
			return ret;
	}

/* IAMROOT-12:
 * -------------
 * 세번째 인수는 irq 타입을 지정한다.
 */
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return ret;
}

#ifdef CONFIG_SMP
static int gic_secondary_init(struct notifier_block *nfb, unsigned long action,
			      void *hcpu)
{
	if (action == CPU_STARTING || action == CPU_STARTING_FROZEN)
		gic_cpu_init(&gic_data[0]);
	return NOTIFY_OK;
}

/*
 * Notifier for enabling the GIC CPU interface. Set an arbitrarily high
 * priority because the GIC needs to be up before the ARM generic timers.
 */
static struct notifier_block gic_cpu_notifier = {
	.notifier_call = gic_secondary_init,
	.priority = 100,
};
#endif

static int gic_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct of_phandle_args *irq_data = arg;

/* IAMROOT-12:
 * -------------
 * args를 분석하여 hwirq, type 정보를 알아온다. 변환이 실패하는 경우 
 * 에러를 반환한다.
 */
	ret = gic_irq_domain_xlate(domain, irq_data->np, irq_data->args,
				   irq_data->args_count, &hwirq, &type);
	if (ret)
		return ret;

/* IAMROOT-12:
 * -------------
 * 요청한 인터럽트 수만큼 irq 디스크립터에 chip 핸들러와 내장 인터럽트 핸들러
 * 및 플래그 등을 설정한다.
 */
	for (i = 0; i < nr_irqs; i++)
		gic_irq_domain_map(domain, virq + i, hwirq + i);

	return 0;
}


/* IAMROOT-12:
 * -------------
 * 디바이스 트리 사용 시 gic의 기본 irq_domain의 ops이다.
 * (irq_domain 하이라키를 지원하기 때문에 alloc과 free 후크가 준비되었다.
 */
static const struct irq_domain_ops gic_irq_domain_hierarchy_ops = {
	.xlate = gic_irq_domain_xlate,
	.alloc = gic_irq_domain_alloc,
	.free = irq_domain_free_irqs_top,
};

static const struct irq_domain_ops gic_irq_domain_ops = {
	.map = gic_irq_domain_map,
	.unmap = gic_irq_domain_unmap,
	.xlate = gic_irq_domain_xlate,
};

/* Default functions for routable irq domain */
static int gic_routable_irq_domain_map(struct irq_domain *d, unsigned int irq,
			      irq_hw_number_t hw)
{
	return 0;
}

static void gic_routable_irq_domain_unmap(struct irq_domain *d,
					  unsigned int irq)
{
}

static int gic_routable_irq_domain_xlate(struct irq_domain *d,
				struct device_node *controller,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq,
				unsigned int *out_type)
{
	*out_hwirq += 16;
	return 0;
}

static const struct irq_domain_ops gic_default_routable_irq_domain_ops = {
	.map = gic_routable_irq_domain_map,
	.unmap = gic_routable_irq_domain_unmap,
	.xlate = gic_routable_irq_domain_xlate,
};

const struct irq_domain_ops *gic_routable_irq_domain_ops =
					&gic_default_routable_irq_domain_ops;

void __init gic_init_bases(unsigned int gic_nr, int irq_start,
			   void __iomem *dist_base, void __iomem *cpu_base,
			   u32 percpu_offset, struct device_node *node)
{
	irq_hw_number_t hwirq_base;
	struct gic_chip_data *gic;
	int gic_irqs, irq_base, i;
	int nr_routable_irqs;

	BUG_ON(gic_nr >= MAX_GIC_NR);

	gic = &gic_data[gic_nr];
#ifdef CONFIG_GIC_NON_BANKED

/* IAMROOT-12:
 * -------------
 * percpu_offset가 주어진 경우는 non banked로 gic 레지스터들을 설정하여 
 * 사용하라는 의미이다.
 *
 * 에) - percpu_offset: 0x4000, dist_base=0x2000_0000
 *       gic_data.dist_base.percpu_base <- per-cpu 할당 후 percpu_offset 만큼 
 *            간격으로 dist_base를 cpu 수 만큼 배치한다. 
 *            (0x2000_0000, 0x2000_4000, 0x2000_8000, 0x2000_c000)
 *     - percpu_offset: 0x0 
 *       gic_data.dist_base.common_base <- dist_base를 그대로 사용한다.
 *            (0x2000_000)
 */
	if (percpu_offset) { /* Frankein-GIC without banked registers... */
		unsigned int cpu;

		gic->dist_base.percpu_base = alloc_percpu(void __iomem *);
		gic->cpu_base.percpu_base = alloc_percpu(void __iomem *);
		if (WARN_ON(!gic->dist_base.percpu_base ||
			    !gic->cpu_base.percpu_base)) {
			free_percpu(gic->dist_base.percpu_base);
			free_percpu(gic->cpu_base.percpu_base);
			return;
		}

		for_each_possible_cpu(cpu) {
			u32 mpidr = cpu_logical_map(cpu);
			u32 core_id = MPIDR_AFFINITY_LEVEL(mpidr, 0);
			unsigned long offset = percpu_offset * core_id;
			*per_cpu_ptr(gic->dist_base.percpu_base, cpu) = dist_base + offset;
			*per_cpu_ptr(gic->cpu_base.percpu_base, cpu) = cpu_base + offset;
		}

/* IAMROOT-12:
 * -------------
 * get_base = gic_get_percpu_base() 
 *            -> raw_cpu_read(*base->percpu_base);
 */
		gic_set_base_accessor(gic, gic_get_percpu_base);
	} else
#endif
	{			/* Normal, sane GIC... */
		WARN(percpu_offset,
		     "GIC_NON_BANKED not enabled, ignoring %08x offset!",
		     percpu_offset);
		gic->dist_base.common_base = dist_base;
		gic->cpu_base.common_base = cpu_base;

/* IAMROOT-12:
 * -------------
 * get_base = gic_get_common_base() 
 *            -> base->common_base;
 */
		gic_set_base_accessor(gic, gic_get_common_base);
	}

	/*
	 * Initialize the CPU interface map to all CPUs.
	 * It will be refined as each CPU probes its ID.
	 */
	for (i = 0; i < NR_GIC_CPU_IF; i++)
		gic_cpu_map[i] = 0xff;

	/*
	 * Find out how many interrupts are supported.
	 * The GIC only supports up to 1020 interrupt sources.
	 */

/* IAMROOT-12:
 * -------------
 * Control Type Register의 5bit를 읽어온다. (32개 단위의 SPIs)
 * *(5 bit값 + 1) * 32 = gic_irqs
 */
	gic_irqs = readl_relaxed(gic_data_dist_base(gic) + GIC_DIST_CTR) & 0x1f;
	gic_irqs = (gic_irqs + 1) * 32;
	if (gic_irqs > 1020)
		gic_irqs = 1020;
	gic->gic_irqs = gic_irqs;

	if (node) {		/* DT case */
		const struct irq_domain_ops *ops = &gic_irq_domain_hierarchy_ops;

/* IAMROOT-12:
 * -------------
 * dra7.dtsi 에서만 사용하는 속성이며 이 속성이 읽히는 경우 
 * gic 인터럽트 수를 지정한다.
 *	arm,routable-irqs = <192>;  <-- 192개가 라우터블 irq로 지정되는데 
 *					추후 확인 필요
 */
		if (!of_property_read_u32(node, "arm,routable-irqs",
					  &nr_routable_irqs)) {
			ops = &gic_irq_domain_ops;
			gic_irqs = nr_routable_irqs;
		}

/* IAMROOT-12:
 * -------------
 * gic가 지원하는 irq 수만큼 리니어 테이블을 만들고 도메인에 추가한다.
 */
		gic->domain = irq_domain_add_linear(node, gic_irqs, ops, gic);
	} else {		/* Non-DT case */
		/*
		 * For primary GICs, skip over SGIs.
		 * For secondary GICs, skip over PPIs, too.
		 */
		if (gic_nr == 0 && (irq_start & 31) > 0) {
			hwirq_base = 16;
			if (irq_start != -1)
				irq_start = (irq_start & ~31) + 16;
		} else {
			hwirq_base = 32;
		}

		gic_irqs -= hwirq_base; /* calculate # of irqs to allocate */

		irq_base = irq_alloc_descs(irq_start, 16, gic_irqs,
					   numa_node_id());
		if (IS_ERR_VALUE(irq_base)) {
			WARN(1, "Cannot allocate irq_descs @ IRQ%d, assuming pre-allocated\n",
			     irq_start);
			irq_base = irq_start;
		}

		gic->domain = irq_domain_add_legacy(node, gic_irqs, irq_base,
					hwirq_base, &gic_irq_domain_ops, gic);
	}

	if (WARN_ON(!gic->domain))
		return;

	if (gic_nr == 0) {
#ifdef CONFIG_SMP
		set_smp_cross_call(gic_raise_softirq);
		register_cpu_notifier(&gic_cpu_notifier);
#endif
		set_handle_irq(gic_handle_irq);
	}

	gic_chip.flags |= gic_arch_extn.flags;

/* IAMROOT-12:
 * -------------
 * distributor & cpu 인터럽트 컨트롤러를 초기화한다.
 */
	gic_dist_init(gic);
	gic_cpu_init(gic);

/* IAMROOT-12:
 * -------------
 * Power Monitor 관련 초기화를 수행한다.
 */
	gic_pm_init(gic);
}

#ifdef CONFIG_OF
static int gic_cnt __initdata;

static int __init
gic_of_init(struct device_node *node, struct device_node *parent)
{
	void __iomem *cpu_base;
	void __iomem *dist_base;
	u32 percpu_offset;
	int irq;

	if (WARN_ON(!node))
		return -ENODEV;

/* IAMROOT-12:
 * -------------
 * 인터럽트 컨틀롤러 주소를 알아와서 IO 매핑한다.
 * (디바이스 노드의 reg 속성에서 읽은 주소를 상위 버스의 ranges 
 *  속성을 사용하여 변환한 주소, 
 *  버스가 하이라키로 구성된 경우 최상위 까지 변환 필요)
 *
 * 예) arch/arm/boot/dts/omap4.dtsi
 *
 * gic: interrupt-controller@48241000 {
 *         compatible = "arm,cortex-a9-gic"
 *         reg = <0x48241000 0x1000>,     <- distributor 
 *               <0x48240100 0x0100>;     <- cpu i/f
 */
	dist_base = of_iomap(node, 0);
	WARN(!dist_base, "unable to map gic dist registers\n");

	cpu_base = of_iomap(node, 1);
	WARN(!cpu_base, "unable to map gic cpu registers\n");

/* IAMROOT-12:
 * -------------
 * "cpu-offset" 속성이 없으면 0으로 한다.
 */
	if (of_property_read_u32(node, "cpu-offset", &percpu_offset))
		percpu_offset = 0;

/* IAMROOT-12:
 * -------------
 * gic_cnt는 처음에 0부터 시작하여 초기화될 때 마다 1씩 증가된다.
 */
	gic_init_bases(gic_cnt, -1, dist_base, cpu_base, percpu_offset, node);
	if (!gic_cnt)
		gic_init_physaddr(node);

/* IAMROOT-12:
 * -------------
 * 부모 인터럽트 컨틀롤러가 있는 경우 irq를 상위 인터럽트 컨트롤러에 
 * cascade하기 위해 매핑한다. (2개 이상의 gic 사용)
 */
	if (parent) {

/* IAMROOT-12:
 * -------------
 * 디바이스 노드에서 표현된 인수를 도메인에서 hwirq로 변환하고 
 * irq 디스크립터를 할당한 후에 이 값에 해당하는 virq를 매핑하고 
 * 그 값을 가져온다.
 */
		irq = irq_of_parse_and_map(node, 0);
		gic_cascade_irq(gic_cnt, irq);
	}

	if (IS_ENABLED(CONFIG_ARM_GIC_V2M))
		gicv2m_of_init(node, gic_data[gic_cnt].domain);

	gic_cnt++;
	return 0;
}
IRQCHIP_DECLARE(gic_400, "arm,gic-400", gic_of_init);
IRQCHIP_DECLARE(arm11mp_gic, "arm,arm11mp-gic", gic_of_init);
IRQCHIP_DECLARE(arm1176jzf_dc_gic, "arm,arm1176jzf-devchip-gic", gic_of_init);
IRQCHIP_DECLARE(cortex_a15_gic, "arm,cortex-a15-gic", gic_of_init);
IRQCHIP_DECLARE(cortex_a9_gic, "arm,cortex-a9-gic", gic_of_init);
IRQCHIP_DECLARE(cortex_a7_gic, "arm,cortex-a7-gic", gic_of_init);
IRQCHIP_DECLARE(msm_8660_qgic, "qcom,msm-8660-qgic", gic_of_init);
IRQCHIP_DECLARE(msm_qgic2, "qcom,msm-qgic2", gic_of_init);

#endif
