/*
 * Broadcom specific AMBA
 * ChipCommon Power Management Unit driver
 *
 * Copyright 2009, Michael Buesch <m@bues.ch>
 * Copyright 2007, 2011, Broadcom Corporation
 * Copyright 2011, 2012, Hauke Mehrtens <hauke@hauke-m.de>
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */

#include "bcma_private.h"
#include <linux/export.h>
#include <linux/bcma/bcma.h>

static u32 bcma_chipco_pll_read(struct bcma_drv_cc *cc, u32 offset)
{
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR, offset);
	bcma_cc_read32(cc, BCMA_CC_PLLCTL_ADDR);
	return bcma_cc_read32(cc, BCMA_CC_PLLCTL_DATA);
}

void bcma_chipco_pll_write(struct bcma_drv_cc *cc, u32 offset, u32 value)
{
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR, offset);
	bcma_cc_read32(cc, BCMA_CC_PLLCTL_ADDR);
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_DATA, value);
}
EXPORT_SYMBOL_GPL(bcma_chipco_pll_write);

void bcma_chipco_pll_maskset(struct bcma_drv_cc *cc, u32 offset, u32 mask,
			     u32 set)
{
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR, offset);
	bcma_cc_read32(cc, BCMA_CC_PLLCTL_ADDR);
	bcma_cc_maskset32(cc, BCMA_CC_PLLCTL_DATA, mask, set);
}
EXPORT_SYMBOL_GPL(bcma_chipco_pll_maskset);

void bcma_chipco_chipctl_maskset(struct bcma_drv_cc *cc,
				 u32 offset, u32 mask, u32 set)
{
	bcma_cc_write32(cc, BCMA_CC_CHIPCTL_ADDR, offset);
	bcma_cc_read32(cc, BCMA_CC_CHIPCTL_ADDR);
	bcma_cc_maskset32(cc, BCMA_CC_CHIPCTL_DATA, mask, set);
}
EXPORT_SYMBOL_GPL(bcma_chipco_chipctl_maskset);

void bcma_chipco_regctl_maskset(struct bcma_drv_cc *cc, u32 offset, u32 mask,
				u32 set)
{
	bcma_cc_write32(cc, BCMA_CC_REGCTL_ADDR, offset);
	bcma_cc_read32(cc, BCMA_CC_REGCTL_ADDR);
	bcma_cc_maskset32(cc, BCMA_CC_REGCTL_DATA, mask, set);
}
EXPORT_SYMBOL_GPL(bcma_chipco_regctl_maskset);

static void bcma_pmu_resources_init(struct bcma_drv_cc *cc)
{
	struct bcma_bus *bus = cc->core->bus;
	u32 min_msk = 0, max_msk = 0;

	switch (bus->chipinfo.id) {
	case BCMA_CHIP_ID_BCM4313:
		min_msk = 0x200D;
		max_msk = 0xFFFF;
		break;
	default:
		bcma_debug(bus, "PMU resource config unknown or not needed for device 0x%04X\n",
			   bus->chipinfo.id);
	}

	/* Set the resource masks. */
	if (min_msk)
		bcma_cc_write32(cc, BCMA_CC_PMU_MINRES_MSK, min_msk);
	if (max_msk)
		bcma_cc_write32(cc, BCMA_CC_PMU_MAXRES_MSK, max_msk);

	/* Add some delay; allow resources to come up and settle. */
	mdelay(2);
}

/* Disable to allow reading SPROM. Don't know the adventages of enabling it. */
void bcma_chipco_bcm4331_ext_pa_lines_ctl(struct bcma_drv_cc *cc, bool enable)
{
	struct bcma_bus *bus = cc->core->bus;
	u32 val;

	val = bcma_cc_read32(cc, BCMA_CC_CHIPCTL);
	if (enable) {
		val |= BCMA_CHIPCTL_4331_EXTPA_EN;
		if (bus->chipinfo.pkg == 9 || bus->chipinfo.pkg == 11)
			val |= BCMA_CHIPCTL_4331_EXTPA_ON_GPIO2_5;
		else if (bus->chipinfo.rev > 0)
			val |= BCMA_CHIPCTL_4331_EXTPA_EN2;
	} else {
		val &= ~BCMA_CHIPCTL_4331_EXTPA_EN;
		val &= ~BCMA_CHIPCTL_4331_EXTPA_EN2;
		val &= ~BCMA_CHIPCTL_4331_EXTPA_ON_GPIO2_5;
	}
	bcma_cc_write32(cc, BCMA_CC_CHIPCTL, val);
}

void bcma_pmu_workarounds(struct bcma_drv_cc *cc)
{
	struct bcma_bus *bus = cc->core->bus;

	switch (bus->chipinfo.id) {
	case BCMA_CHIP_ID_BCM4313:
		/* enable 12 mA drive strenth for 4313 and set chipControl
		   register bit 1 */
		bcma_chipco_chipctl_maskset(cc, 0,
					    BCMA_CCTRL_4313_12MA_LED_DRIVE,
					    BCMA_CCTRL_4313_12MA_LED_DRIVE);
		break;
	case BCMA_CHIP_ID_BCM4331:
	case BCMA_CHIP_ID_BCM43431:
		/* Ext PA lines must be enabled for tx on BCM4331 */
		bcma_chipco_bcm4331_ext_pa_lines_ctl(cc, true);
		break;
	case BCMA_CHIP_ID_BCM43224:
	case BCMA_CHIP_ID_BCM43421:
		/* enable 12 mA drive strenth for 43224 and set chipControl
		   register bit 15 */
		if (bus->chipinfo.rev == 0) {
			bcma_cc_maskset32(cc, BCMA_CC_CHIPCTL,
					  BCMA_CCTRL_43224_GPIO_TOGGLE,
					  BCMA_CCTRL_43224_GPIO_TOGGLE);
			bcma_chipco_chipctl_maskset(cc, 0,
						    BCMA_CCTRL_43224A0_12MA_LED_DRIVE,
						    BCMA_CCTRL_43224A0_12MA_LED_DRIVE);
		} else {
			bcma_chipco_chipctl_maskset(cc, 0,
						    BCMA_CCTRL_43224B0_12MA_LED_DRIVE,
						    BCMA_CCTRL_43224B0_12MA_LED_DRIVE);
		}
		break;
	default:
		bcma_debug(bus, "Workarounds unknown or not needed for device 0x%04X\n",
			   bus->chipinfo.id);
	}
}

void bcma_pmu_init(struct bcma_drv_cc *cc)
{
	u32 pmucap;

	pmucap = bcma_cc_read32(cc, BCMA_CC_PMU_CAP);
	cc->pmu.rev = (pmucap & BCMA_CC_PMU_CAP_REVISION);

	bcma_debug(cc->core->bus, "Found rev %u PMU (capabilities 0x%08X)\n",
		   cc->pmu.rev, pmucap);

	if (cc->pmu.rev == 1)
		bcma_cc_mask32(cc, BCMA_CC_PMU_CTL,
			      ~BCMA_CC_PMU_CTL_NOILPONW);
	else
		bcma_cc_set32(cc, BCMA_CC_PMU_CTL,
			     BCMA_CC_PMU_CTL_NOILPONW);

	bcma_pmu_resources_init(cc);
	bcma_pmu_workarounds(cc);
}

u32 bcma_pmu_alp_clock(struct bcma_drv_cc *cc)
{
	struct bcma_bus *bus = cc->core->bus;

	switch (bus->chipinfo.id) {
	case BCMA_CHIP_ID_BCM4716:
	case BCMA_CHIP_ID_BCM4748:
	case BCMA_CHIP_ID_BCM47162:
	case BCMA_CHIP_ID_BCM4313:
	case BCMA_CHIP_ID_BCM5357:
	case BCMA_CHIP_ID_BCM4749:
	case BCMA_CHIP_ID_BCM53572:
		/* always 20Mhz */
		return 20000 * 1000;
	case BCMA_CHIP_ID_BCM5356:
	case BCMA_CHIP_ID_BCM4706:
		/* always 25Mhz */
		return 25000 * 1000;
	default:
		bcma_warn(bus, "No ALP clock specified for %04X device, pmu rev. %d, using default %d Hz\n",
			  bus->chipinfo.id, cc->pmu.rev, BCMA_CC_PMU_ALP_CLOCK);
	}
	return BCMA_CC_PMU_ALP_CLOCK;
}

/* Find the output of the "m" pll divider given pll controls that start with
 * pllreg "pll0" i.e. 12 for main 6 for phy, 0 for misc.
 */
static u32 bcma_pmu_clock(struct bcma_drv_cc *cc, u32 pll0, u32 m)
{
	u32 tmp, div, ndiv, p1, p2, fc;
	struct bcma_bus *bus = cc->core->bus;

	BUG_ON((pll0 & 3) || (pll0 > BCMA_CC_PMU4716_MAINPLL_PLL0));

	BUG_ON(!m || m > 4);

	if (bus->chipinfo.id == BCMA_CHIP_ID_BCM5357 ||
	    bus->chipinfo.id == BCMA_CHIP_ID_BCM4749) {
		/* Detect failure in clock setting */
		tmp = bcma_cc_read32(cc, BCMA_CC_CHIPSTAT);
		if (tmp & 0x40000)
			return 133 * 1000000;
	}

	tmp = bcma_chipco_pll_read(cc, pll0 + BCMA_CC_PPL_P1P2_OFF);
	p1 = (tmp & BCMA_CC_PPL_P1_MASK) >> BCMA_CC_PPL_P1_SHIFT;
	p2 = (tmp & BCMA_CC_PPL_P2_MASK) >> BCMA_CC_PPL_P2_SHIFT;

	tmp = bcma_chipco_pll_read(cc, pll0 + BCMA_CC_PPL_M14_OFF);
	div = (tmp >> ((m - 1) * BCMA_CC_PPL_MDIV_WIDTH)) &
		BCMA_CC_PPL_MDIV_MASK;

	tmp = bcma_chipco_pll_read(cc, pll0 + BCMA_CC_PPL_NM5_OFF);
	ndiv = (tmp & BCMA_CC_PPL_NDIV_MASK) >> BCMA_CC_PPL_NDIV_SHIFT;

	/* Do calculation in Mhz */
	fc = bcma_pmu_alp_clock(cc) / 1000000;
	fc = (p1 * ndiv * fc) / p2;

	/* Return clock in Hertz */
	return (fc / div) * 1000000;
}

/* query bus clock frequency for PMU-enabled chipcommon */
u32 bcma_pmu_get_clockcontrol(struct bcma_drv_cc *cc)
{
	struct bcma_bus *bus = cc->core->bus;

	switch (bus->chipinfo.id) {
	case BCMA_CHIP_ID_BCM4716:
	case BCMA_CHIP_ID_BCM4748:
	case BCMA_CHIP_ID_BCM47162:
		return bcma_pmu_clock(cc, BCMA_CC_PMU4716_MAINPLL_PLL0,
				      BCMA_CC_PMU5_MAINPLL_SSB);
	case BCMA_CHIP_ID_BCM5356:
		return bcma_pmu_clock(cc, BCMA_CC_PMU5356_MAINPLL_PLL0,
				      BCMA_CC_PMU5_MAINPLL_SSB);
	case BCMA_CHIP_ID_BCM5357:
	case BCMA_CHIP_ID_BCM4749:
		return bcma_pmu_clock(cc, BCMA_CC_PMU5357_MAINPLL_PLL0,
				      BCMA_CC_PMU5_MAINPLL_SSB);
	case BCMA_CHIP_ID_BCM4706:
		return bcma_pmu_clock(cc, BCMA_CC_PMU4706_MAINPLL_PLL0,
				      BCMA_CC_PMU5_MAINPLL_SSB);
	case BCMA_CHIP_ID_BCM53572:
		return 75000000;
	default:
		bcma_warn(bus, "No backplane clock specified for %04X device, pmu rev. %d, using default %d Hz\n",
			  bus->chipinfo.id, cc->pmu.rev, BCMA_CC_PMU_HT_CLOCK);
	}
	return BCMA_CC_PMU_HT_CLOCK;
}

/* query cpu clock frequency for PMU-enabled chipcommon */
u32 bcma_pmu_get_clockcpu(struct bcma_drv_cc *cc)
{
	struct bcma_bus *bus = cc->core->bus;

	if (bus->chipinfo.id == BCMA_CHIP_ID_BCM53572)
		return 300000000;

	if (cc->pmu.rev >= 5) {
		u32 pll;
		switch (bus->chipinfo.id) {
		case BCMA_CHIP_ID_BCM5356:
			pll = BCMA_CC_PMU5356_MAINPLL_PLL0;
			break;
		case BCMA_CHIP_ID_BCM5357:
		case BCMA_CHIP_ID_BCM4749:
			pll = BCMA_CC_PMU5357_MAINPLL_PLL0;
			break;
		default:
			pll = BCMA_CC_PMU4716_MAINPLL_PLL0;
			break;
		}

		/* TODO: if (bus->chipinfo.id == BCMA_CHIP_ID_BCM4706)
		  return si_4706_pmu_clock(sih, osh, cc, PMU4706_MAINPLL_PLL0, PMU5_MAINPLL_CPU); */
		return bcma_pmu_clock(cc, pll, BCMA_CC_PMU5_MAINPLL_CPU);
	}

	return bcma_pmu_get_clockcontrol(cc);
}

static void bcma_pmu_spuravoid_pll_write(struct bcma_drv_cc *cc, u32 offset,
					 u32 value)
{
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR, offset);
	bcma_cc_write32(cc, BCMA_CC_PLLCTL_DATA, value);
}

void bcma_pmu_spuravoid_pllupdate(struct bcma_drv_cc *cc, int spuravoid)
{
	u32 tmp = 0;
	u8 phypll_offset = 0;
	u8 bcm5357_bcm43236_p1div[] = {0x1, 0x5, 0x5};
	u8 bcm5357_bcm43236_ndiv[] = {0x30, 0xf6, 0xfc};
	struct bcma_bus *bus = cc->core->bus;

	switch (bus->chipinfo.id) {
	case BCMA_CHIP_ID_BCM5357:
	case BCMA_CHIP_ID_BCM4749:
	case BCMA_CHIP_ID_BCM53572:
		/* 5357[ab]0, 43236[ab]0, and 6362b0 */

		/* BCM5357 needs to touch PLL1_PLLCTL[02],
		   so offset PLL0_PLLCTL[02] by 6 */
		phypll_offset = (bus->chipinfo.id == BCMA_CHIP_ID_BCM5357 ||
		       bus->chipinfo.id == BCMA_CHIP_ID_BCM4749 ||
		       bus->chipinfo.id == BCMA_CHIP_ID_BCM53572) ? 6 : 0;

		/* RMW only the P1 divider */
		bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR,
				BCMA_CC_PMU_PLL_CTL0 + phypll_offset);
		tmp = bcma_cc_read32(cc, BCMA_CC_PLLCTL_DATA);
		tmp &= (~(BCMA_CC_PMU1_PLL0_PC0_P1DIV_MASK));
		tmp |= (bcm5357_bcm43236_p1div[spuravoid] << BCMA_CC_PMU1_PLL0_PC0_P1DIV_SHIFT);
		bcma_cc_write32(cc, BCMA_CC_PLLCTL_DATA, tmp);

		/* RMW only the int feedback divider */
		bcma_cc_write32(cc, BCMA_CC_PLLCTL_ADDR,
				BCMA_CC_PMU_PLL_CTL2 + phypll_offset);
		tmp = bcma_cc_read32(cc, BCMA_CC_PLLCTL_DATA);
		tmp &= ~(BCMA_CC_PMU1_PLL0_PC2_NDIV_INT_MASK);
		tmp |= (bcm5357_bcm43236_ndiv[spuravoid]) << BCMA_CC_PMU1_PLL0_PC2_NDIV_INT_SHIFT;
		bcma_cc_write32(cc, BCMA_CC_PLLCTL_DATA, tmp);

		tmp = 1 << 10;
		break;

	case BCMA_CHIP_ID_BCM4331:
	case BCMA_CHIP_ID_BCM43431:
		if (spuravoid == 2) {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11500014);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x0FC00a08);
		} else if (spuravoid == 1) {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11500014);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x0F600a08);
		} else {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11100014);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x03000a08);
		}
		tmp = 1 << 10;
		break;

	case BCMA_CHIP_ID_BCM43224:
	case BCMA_CHIP_ID_BCM43225:
	case BCMA_CHIP_ID_BCM43421:
		if (spuravoid == 1) {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11500010);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x000C0C06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x0F600a08);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x2001E920);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		} else {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11100010);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x000c0c06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x03000a08);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x200005c0);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		}
		tmp = 1 << 10;
		break;

	case BCMA_CHIP_ID_BCM4716:
	case BCMA_CHIP_ID_BCM4748:
	case BCMA_CHIP_ID_BCM47162:
		if (spuravoid == 1) {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11500060);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x080C0C06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x0F600000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x2001E924);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		} else {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11100060);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x080c0c06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x03000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x200005c0);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		}

		tmp = 3 << 9;
		break;

	case BCMA_CHIP_ID_BCM43227:
	case BCMA_CHIP_ID_BCM43228:
	case BCMA_CHIP_ID_BCM43428:
		/* LCNXN */
		/* PLL Settings for spur avoidance on/off mode,
		   no on2 support for 43228A0 */
		if (spuravoid == 1) {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x01100014);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x040C0C06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x03140A08);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00333333);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x202C2820);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		} else {
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL0,
						     0x11100014);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL1,
						     0x040c0c06);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL2,
						     0x03000a08);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL3,
						     0x00000000);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL4,
						     0x200005c0);
			bcma_pmu_spuravoid_pll_write(cc, BCMA_CC_PMU_PLL_CTL5,
						     0x88888815);
		}
		tmp = 1 << 10;
		break;
	default:
		bcma_err(bus, "Unknown spuravoidance settings for chip 0x%04X, not changing PLL\n",
			 bus->chipinfo.id);
		break;
	}

	tmp |= bcma_cc_read32(cc, BCMA_CC_PMU_CTL);
	bcma_cc_write32(cc, BCMA_CC_PMU_CTL, tmp);
}
EXPORT_SYMBOL_GPL(bcma_pmu_spuravoid_pllupdate);
