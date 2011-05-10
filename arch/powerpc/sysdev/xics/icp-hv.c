/*
 * Copyright 2011 IBM Corporation.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/of.h>

#include <asm/smp.h>
#include <asm/irq.h>
#include <asm/errno.h>
#include <asm/xics.h>
#include <asm/io.h>
#include <asm/hvcall.h>

static inline unsigned int icp_hv_get_xirr(unsigned char cppr)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	long rc;

	rc = plpar_hcall(H_XIRR, retbuf, cppr);
	if (rc != H_SUCCESS)
		panic(" bad return code xirr - rc = %lx\n", rc);
	return (unsigned int)retbuf[0];
}

static inline void icp_hv_set_xirr(unsigned int value)
{
	long rc = plpar_hcall_norets(H_EOI, value);
	if (rc != H_SUCCESS)
		panic("bad return code EOI - rc = %ld, value=%x\n", rc, value);
}

static inline void icp_hv_set_cppr(u8 value)
{
	long rc = plpar_hcall_norets(H_CPPR, value);
	if (rc != H_SUCCESS)
		panic("bad return code cppr - rc = %lx\n", rc);
}

static inline void icp_hv_set_qirr(int n_cpu , u8 value)
{
	long rc = plpar_hcall_norets(H_IPI, get_hard_smp_processor_id(n_cpu),
				     value);
	if (rc != H_SUCCESS)
		panic("bad return code qirr - rc = %lx\n", rc);
}

static void icp_hv_eoi(struct irq_data *d)
{
	unsigned int hw_irq = (unsigned int)irqd_to_hwirq(d);

	iosync();
	icp_hv_set_xirr((xics_pop_cppr() << 24) | hw_irq);
}

static void icp_hv_teardown_cpu(void)
{
	int cpu = smp_processor_id();

	/* Clear any pending IPI */
	icp_hv_set_qirr(cpu, 0xff);
}

static void icp_hv_flush_ipi(void)
{
	/* We take the ipi irq but and never return so we
	 * need to EOI the IPI, but want to leave our priority 0
	 *
	 * should we check all the other interrupts too?
	 * should we be flagging idle loop instead?
	 * or creating some task to be scheduled?
	 */

	icp_hv_set_xirr((0x00 << 24) | XICS_IPI);
}

static unsigned int icp_hv_get_irq(void)
{
	unsigned int xirr = icp_hv_get_xirr(xics_cppr_top());
	unsigned int vec = xirr & 0x00ffffff;
	unsigned int irq;

	if (vec == XICS_IRQ_SPURIOUS)
		return NO_IRQ;

	irq = irq_radix_revmap_lookup(xics_host, vec);
	if (likely(irq != NO_IRQ)) {
		xics_push_cppr(vec);
		return irq;
	}

	/* We don't have a linux mapping, so have rtas mask it. */
	xics_mask_unknown_vec(vec);

	/* We might learn about it later, so EOI it */
	icp_hv_set_xirr(xirr);

	return NO_IRQ;
}

static void icp_hv_set_cpu_priority(unsigned char cppr)
{
	xics_set_base_cppr(cppr);
	icp_hv_set_cppr(cppr);
	iosync();
}

#ifdef CONFIG_SMP

static void icp_hv_message_pass(int cpu, int msg)
{
	unsigned long *tgt = &per_cpu(xics_ipi_message, cpu);

	set_bit(msg, tgt);
	mb();
	icp_hv_set_qirr(cpu, IPI_PRIORITY);
}

static irqreturn_t icp_hv_ipi_action(int irq, void *dev_id)
{
	int cpu = smp_processor_id();

	icp_hv_set_qirr(cpu, 0xff);

	return xics_ipi_dispatch(cpu);
}

#endif /* CONFIG_SMP */

static const struct icp_ops icp_hv_ops = {
	.get_irq	= icp_hv_get_irq,
	.eoi		= icp_hv_eoi,
	.set_priority	= icp_hv_set_cpu_priority,
	.teardown_cpu	= icp_hv_teardown_cpu,
	.flush_ipi	= icp_hv_flush_ipi,
#ifdef CONFIG_SMP
	.ipi_action	= icp_hv_ipi_action,
	.message_pass	= icp_hv_message_pass,
#endif
};

int icp_hv_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "ibm,ppc-xicp");
	if (!np)
		np = of_find_node_by_type(NULL,
				    "PowerPC-External-Interrupt-Presentation");
	if (!np)
		return -ENODEV;

	icp_ops = &icp_hv_ops;

	return 0;
}

