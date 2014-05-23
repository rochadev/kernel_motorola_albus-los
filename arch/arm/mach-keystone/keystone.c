/*
 * Keystone2 based boards and SOC related code.
 *
 * Copyright 2013 Texas Instruments, Inc.
 *	Cyril Chemparathy <cyril@ti.com>
 *	Santosh Shilimkar <santosh.shillimkar@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include <linux/io.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>

#include <asm/setup.h>
#include <asm/mach/map.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/smp_plat.h>
#include <asm/memory.h>

#include "memory.h"

#include "keystone.h"

#define PLL_RESET_WRITE_KEY_MASK		0xffff0000
#define PLL_RESET_WRITE_KEY			0x5a69
#define PLL_RESET				BIT(16)

static void __iomem *keystone_rstctrl;
static struct notifier_block platform_nb;
static unsigned long keystone_dma_pfn_offset __read_mostly;

static int keystone_platform_notifier(struct notifier_block *nb,
				      unsigned long event, void *data)
{
	struct device *dev = data;

	if (event != BUS_NOTIFY_ADD_DEVICE)
		return NOTIFY_DONE;

	if (!dev)
		return NOTIFY_BAD;

	if (!dev->of_node) {
		dev->dma_pfn_offset = keystone_dma_pfn_offset;
		dev_err(dev, "set dma_pfn_offset%08lx\n",
			dev->dma_pfn_offset);
	}
	return NOTIFY_OK;
}

static void __init keystone_init(void)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "ti,keystone-reset");
	if (WARN_ON(!node))
		pr_warn("ti,keystone-reset node undefined\n");

	keystone_rstctrl = of_iomap(node, 0);
	if (WARN_ON(!keystone_rstctrl))
		pr_warn("ti,keystone-reset iomap error\n");

	keystone_pm_runtime_init();
	if (platform_nb.notifier_call)
		bus_register_notifier(&platform_bus_type, &platform_nb);
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
}

static phys_addr_t keystone_virt_to_idmap(unsigned long x)
{
	return (phys_addr_t)(x) - CONFIG_PAGE_OFFSET + KEYSTONE_LOW_PHYS_START;
}

static void __init keystone_init_meminfo(void)
{
	bool lpae = IS_ENABLED(CONFIG_ARM_LPAE);
	bool pvpatch = IS_ENABLED(CONFIG_ARM_PATCH_PHYS_VIRT);
	phys_addr_t offset = PHYS_OFFSET - KEYSTONE_LOW_PHYS_START;
	phys_addr_t mem_start, mem_end;

	BUG_ON(meminfo.nr_banks < 1);
	mem_start = meminfo.bank[0].start;
	mem_end = mem_start + meminfo.bank[0].size - 1;

	/* nothing to do if we are running out of the <32-bit space */
	if (mem_start >= KEYSTONE_LOW_PHYS_START &&
	    mem_end   <= KEYSTONE_LOW_PHYS_END)
		return;

	if (!lpae || !pvpatch) {
		pr_crit("Enable %s%s%s to run outside 32-bit space\n",
		      !lpae ? __stringify(CONFIG_ARM_LPAE) : "",
		      (!lpae && !pvpatch) ? " and " : "",
		      !pvpatch ? __stringify(CONFIG_ARM_PATCH_PHYS_VIRT) : "");
	}

	if (mem_start < KEYSTONE_HIGH_PHYS_START ||
	    mem_end   > KEYSTONE_HIGH_PHYS_END) {
		pr_crit("Invalid address space for memory (%08llx-%08llx)\n",
		      (u64)mem_start, (u64)mem_end);
	}

	offset += KEYSTONE_HIGH_PHYS_START;
	__pv_phys_pfn_offset = PFN_DOWN(offset);
	__pv_offset = (offset - PAGE_OFFSET);

	/* Populate the arch idmap hook */
	arch_virt_to_idmap = keystone_virt_to_idmap;
	platform_nb.notifier_call = keystone_platform_notifier;
	keystone_dma_pfn_offset = PFN_DOWN(KEYSTONE_HIGH_PHYS_START -
						KEYSTONE_LOW_PHYS_START);

	pr_info("Switching to high address space at 0x%llx\n", (u64)offset);
}

static const char *keystone_match[] __initconst = {
	"ti,keystone",
	NULL,
};

void keystone_restart(enum reboot_mode mode, const char *cmd)
{
	u32 val;

	BUG_ON(!keystone_rstctrl);

	/* Enable write access to RSTCTRL */
	val = readl(keystone_rstctrl);
	val &= PLL_RESET_WRITE_KEY_MASK;
	val |= PLL_RESET_WRITE_KEY;
	writel(val, keystone_rstctrl);

	/* Reset the SOC */
	val = readl(keystone_rstctrl);
	val &= ~PLL_RESET;
	writel(val, keystone_rstctrl);
}

DT_MACHINE_START(KEYSTONE, "Keystone")
#if defined(CONFIG_ZONE_DMA) && defined(CONFIG_ARM_LPAE)
	.dma_zone_size	= SZ_2G,
#endif
	.smp		= smp_ops(keystone_smp_ops),
	.init_machine	= keystone_init,
	.dt_compat	= keystone_match,
	.restart	= keystone_restart,
	.init_meminfo   = keystone_init_meminfo,
MACHINE_END
