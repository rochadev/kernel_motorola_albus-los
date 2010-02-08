/*
 * wm8350-irq.c  --  IRQ support for Wolfson WM8350
 *
 * Copyright 2007, 2008, 2009 Wolfson Microelectronics PLC.
 *
 * Author: Liam Girdwood, Mark Brown
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bug.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>

#include <linux/mfd/wm8350/core.h>
#include <linux/mfd/wm8350/audio.h>
#include <linux/mfd/wm8350/comparator.h>
#include <linux/mfd/wm8350/gpio.h>
#include <linux/mfd/wm8350/pmic.h>
#include <linux/mfd/wm8350/rtc.h>
#include <linux/mfd/wm8350/supply.h>
#include <linux/mfd/wm8350/wdt.h>

#define WM8350_NUM_IRQ_REGS 7

#define WM8350_INT_OFFSET_1                     0
#define WM8350_INT_OFFSET_2                     1
#define WM8350_POWER_UP_INT_OFFSET              2
#define WM8350_UNDER_VOLTAGE_INT_OFFSET         3
#define WM8350_OVER_CURRENT_INT_OFFSET          4
#define WM8350_GPIO_INT_OFFSET                  5
#define WM8350_COMPARATOR_INT_OFFSET            6

struct wm8350_irq_data {
	int primary;
	int reg;
	int mask;
	int primary_only;
};

static struct wm8350_irq_data wm8350_irqs[] = {
	[WM8350_IRQ_OC_LS] = {
		.primary = WM8350_OC_INT,
		.reg = WM8350_OVER_CURRENT_INT_OFFSET,
		.mask = WM8350_OC_LS_EINT,
		.primary_only = 1,
	},
	[WM8350_IRQ_UV_DC1] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC1_EINT,
	},
	[WM8350_IRQ_UV_DC2] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC2_EINT,
	},
	[WM8350_IRQ_UV_DC3] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC3_EINT,
	},
	[WM8350_IRQ_UV_DC4] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC4_EINT,
	},
	[WM8350_IRQ_UV_DC5] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC5_EINT,
	},
	[WM8350_IRQ_UV_DC6] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_DC6_EINT,
	},
	[WM8350_IRQ_UV_LDO1] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_LDO1_EINT,
	},
	[WM8350_IRQ_UV_LDO2] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_LDO2_EINT,
	},
	[WM8350_IRQ_UV_LDO3] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_LDO3_EINT,
	},
	[WM8350_IRQ_UV_LDO4] = {
		.primary = WM8350_UV_INT,
		.reg = WM8350_UNDER_VOLTAGE_INT_OFFSET,
		.mask = WM8350_UV_LDO4_EINT,
	},
	[WM8350_IRQ_CHG_BAT_HOT] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_BAT_HOT_EINT,
	},
	[WM8350_IRQ_CHG_BAT_COLD] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_BAT_COLD_EINT,
	},
	[WM8350_IRQ_CHG_BAT_FAIL] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_BAT_FAIL_EINT,
	},
	[WM8350_IRQ_CHG_TO] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_TO_EINT,
	},
	[WM8350_IRQ_CHG_END] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_END_EINT,
	},
	[WM8350_IRQ_CHG_START] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_START_EINT,
	},
	[WM8350_IRQ_CHG_FAST_RDY] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_FAST_RDY_EINT,
	},
	[WM8350_IRQ_CHG_VBATT_LT_3P9] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_VBATT_LT_3P9_EINT,
	},
	[WM8350_IRQ_CHG_VBATT_LT_3P1] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_VBATT_LT_3P1_EINT,
	},
	[WM8350_IRQ_CHG_VBATT_LT_2P85] = {
		.primary = WM8350_CHG_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_CHG_VBATT_LT_2P85_EINT,
	},
	[WM8350_IRQ_RTC_ALM] = {
		.primary = WM8350_RTC_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_RTC_ALM_EINT,
	},
	[WM8350_IRQ_RTC_SEC] = {
		.primary = WM8350_RTC_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_RTC_SEC_EINT,
	},
	[WM8350_IRQ_RTC_PER] = {
		.primary = WM8350_RTC_INT,
		.reg = WM8350_INT_OFFSET_1,
		.mask = WM8350_RTC_PER_EINT,
	},
	[WM8350_IRQ_CS1] = {
		.primary = WM8350_CS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_CS1_EINT,
	},
	[WM8350_IRQ_CS2] = {
		.primary = WM8350_CS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_CS2_EINT,
	},
	[WM8350_IRQ_SYS_HYST_COMP_FAIL] = {
		.primary = WM8350_SYS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_SYS_HYST_COMP_FAIL_EINT,
	},
	[WM8350_IRQ_SYS_CHIP_GT115] = {
		.primary = WM8350_SYS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_SYS_CHIP_GT115_EINT,
	},
	[WM8350_IRQ_SYS_CHIP_GT140] = {
		.primary = WM8350_SYS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_SYS_CHIP_GT140_EINT,
	},
	[WM8350_IRQ_SYS_WDOG_TO] = {
		.primary = WM8350_SYS_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_SYS_WDOG_TO_EINT,
	},
	[WM8350_IRQ_AUXADC_DATARDY] = {
		.primary = WM8350_AUXADC_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_AUXADC_DATARDY_EINT,
	},
	[WM8350_IRQ_AUXADC_DCOMP4] = {
		.primary = WM8350_AUXADC_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_AUXADC_DCOMP4_EINT,
	},
	[WM8350_IRQ_AUXADC_DCOMP3] = {
		.primary = WM8350_AUXADC_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_AUXADC_DCOMP3_EINT,
	},
	[WM8350_IRQ_AUXADC_DCOMP2] = {
		.primary = WM8350_AUXADC_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_AUXADC_DCOMP2_EINT,
	},
	[WM8350_IRQ_AUXADC_DCOMP1] = {
		.primary = WM8350_AUXADC_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_AUXADC_DCOMP1_EINT,
	},
	[WM8350_IRQ_USB_LIMIT] = {
		.primary = WM8350_USB_INT,
		.reg = WM8350_INT_OFFSET_2,
		.mask = WM8350_USB_LIMIT_EINT,
		.primary_only = 1,
	},
	[WM8350_IRQ_WKUP_OFF_STATE] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_OFF_STATE_EINT,
	},
	[WM8350_IRQ_WKUP_HIB_STATE] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_HIB_STATE_EINT,
	},
	[WM8350_IRQ_WKUP_CONV_FAULT] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_CONV_FAULT_EINT,
	},
	[WM8350_IRQ_WKUP_WDOG_RST] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_WDOG_RST_EINT,
	},
	[WM8350_IRQ_WKUP_GP_PWR_ON] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_GP_PWR_ON_EINT,
	},
	[WM8350_IRQ_WKUP_ONKEY] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_ONKEY_EINT,
	},
	[WM8350_IRQ_WKUP_GP_WAKEUP] = {
		.primary = WM8350_WKUP_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_WKUP_GP_WAKEUP_EINT,
	},
	[WM8350_IRQ_CODEC_JCK_DET_L] = {
		.primary = WM8350_CODEC_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_CODEC_JCK_DET_L_EINT,
	},
	[WM8350_IRQ_CODEC_JCK_DET_R] = {
		.primary = WM8350_CODEC_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_CODEC_JCK_DET_R_EINT,
	},
	[WM8350_IRQ_CODEC_MICSCD] = {
		.primary = WM8350_CODEC_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_CODEC_MICSCD_EINT,
	},
	[WM8350_IRQ_CODEC_MICD] = {
		.primary = WM8350_CODEC_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_CODEC_MICD_EINT,
	},
	[WM8350_IRQ_EXT_USB_FB] = {
		.primary = WM8350_EXT_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_EXT_USB_FB_EINT,
	},
	[WM8350_IRQ_EXT_WALL_FB] = {
		.primary = WM8350_EXT_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_EXT_WALL_FB_EINT,
	},
	[WM8350_IRQ_EXT_BAT_FB] = {
		.primary = WM8350_EXT_INT,
		.reg = WM8350_COMPARATOR_INT_OFFSET,
		.mask = WM8350_EXT_BAT_FB_EINT,
	},
	[WM8350_IRQ_GPIO(0)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP0_EINT,
	},
	[WM8350_IRQ_GPIO(1)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP1_EINT,
	},
	[WM8350_IRQ_GPIO(2)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP2_EINT,
	},
	[WM8350_IRQ_GPIO(3)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP3_EINT,
	},
	[WM8350_IRQ_GPIO(4)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP4_EINT,
	},
	[WM8350_IRQ_GPIO(5)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP5_EINT,
	},
	[WM8350_IRQ_GPIO(6)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP6_EINT,
	},
	[WM8350_IRQ_GPIO(7)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP7_EINT,
	},
	[WM8350_IRQ_GPIO(8)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP8_EINT,
	},
	[WM8350_IRQ_GPIO(9)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP9_EINT,
	},
	[WM8350_IRQ_GPIO(10)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP10_EINT,
	},
	[WM8350_IRQ_GPIO(11)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP11_EINT,
	},
	[WM8350_IRQ_GPIO(12)] = {
		.primary = WM8350_GP_INT,
		.reg = WM8350_GPIO_INT_OFFSET,
		.mask = WM8350_GP12_EINT,
	},
};

static void wm8350_irq_call_handler(struct wm8350 *wm8350, int irq)
{
	mutex_lock(&wm8350->irq_mutex);

	if (wm8350->irq[irq].handler)
		wm8350->irq[irq].handler(irq, wm8350->irq[irq].data);
	else {
		dev_err(wm8350->dev, "irq %d nobody cared. now masked.\n",
			irq);
		wm8350_mask_irq(wm8350, irq);
	}

	mutex_unlock(&wm8350->irq_mutex);
}

/*
 * This is a threaded IRQ handler so can access I2C/SPI.  Since all
 * interrupts are clear on read the IRQ line will be reasserted and
 * the physical IRQ will be handled again if another interrupt is
 * asserted while we run - in the normal course of events this is a
 * rare occurrence so we save I2C/SPI reads.
 */
static irqreturn_t wm8350_irq(int irq, void *irq_data)
{
	struct wm8350 *wm8350 = irq_data;
	u16 level_one;
	u16 sub_reg[WM8350_NUM_IRQ_REGS];
	int read_done[WM8350_NUM_IRQ_REGS];
	struct wm8350_irq_data *data;
	int i;

	/* TODO: Use block reads to improve performance? */
	level_one = wm8350_reg_read(wm8350, WM8350_SYSTEM_INTERRUPTS)
		& ~wm8350_reg_read(wm8350, WM8350_SYSTEM_INTERRUPTS_MASK);

	if (!level_one)
		return IRQ_NONE;

	memset(&read_done, 0, sizeof(read_done));

	for (i = 0; i < ARRAY_SIZE(wm8350_irqs); i++) {
		data = &wm8350_irqs[i];

		if (!(level_one & data->primary))
			continue;

		if (!read_done[data->reg]) {
			sub_reg[data->reg] =
				wm8350_reg_read(wm8350, WM8350_INT_STATUS_1 +
						data->reg);
			sub_reg[data->reg] &=
				~wm8350_reg_read(wm8350,
						 WM8350_INT_STATUS_1_MASK +
						 data->reg);
			read_done[data->reg] = 1;
		}

		if (sub_reg[data->reg] & data->mask)
			wm8350_irq_call_handler(wm8350, i);
	}

	return IRQ_HANDLED;
}

int wm8350_register_irq(struct wm8350 *wm8350, int irq,
			irq_handler_t handler, unsigned long flags,
			const char *name, void *data)
{
	if (irq < 0 || irq >= WM8350_NUM_IRQ || !handler)
		return -EINVAL;

	if (wm8350->irq[irq].handler)
		return -EBUSY;

	mutex_lock(&wm8350->irq_mutex);
	wm8350->irq[irq].handler = handler;
	wm8350->irq[irq].data = data;
	mutex_unlock(&wm8350->irq_mutex);

	wm8350_unmask_irq(wm8350, irq);

	return 0;
}
EXPORT_SYMBOL_GPL(wm8350_register_irq);

int wm8350_free_irq(struct wm8350 *wm8350, int irq)
{
	if (irq < 0 || irq >= WM8350_NUM_IRQ)
		return -EINVAL;

	wm8350_mask_irq(wm8350, irq);

	mutex_lock(&wm8350->irq_mutex);
	wm8350->irq[irq].handler = NULL;
	mutex_unlock(&wm8350->irq_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(wm8350_free_irq);

int wm8350_mask_irq(struct wm8350 *wm8350, int irq)
{
	return wm8350_set_bits(wm8350, WM8350_INT_STATUS_1_MASK +
			       wm8350_irqs[irq].reg,
			       wm8350_irqs[irq].mask);
}
EXPORT_SYMBOL_GPL(wm8350_mask_irq);

int wm8350_unmask_irq(struct wm8350 *wm8350, int irq)
{
	return wm8350_clear_bits(wm8350, WM8350_INT_STATUS_1_MASK +
				 wm8350_irqs[irq].reg,
				 wm8350_irqs[irq].mask);
}
EXPORT_SYMBOL_GPL(wm8350_unmask_irq);

int wm8350_irq_init(struct wm8350 *wm8350, int irq,
		    struct wm8350_platform_data *pdata)
{
	int ret;
	int flags = IRQF_ONESHOT;

	if (!irq) {
		dev_err(wm8350->dev, "No IRQ configured\n");
		return -EINVAL;
	}

	wm8350_reg_write(wm8350, WM8350_SYSTEM_INTERRUPTS_MASK, 0xFFFF);
	wm8350_reg_write(wm8350, WM8350_INT_STATUS_1_MASK, 0xFFFF);
	wm8350_reg_write(wm8350, WM8350_INT_STATUS_2_MASK, 0xFFFF);
	wm8350_reg_write(wm8350, WM8350_UNDER_VOLTAGE_INT_STATUS_MASK, 0xFFFF);
	wm8350_reg_write(wm8350, WM8350_GPIO_INT_STATUS_MASK, 0xFFFF);
	wm8350_reg_write(wm8350, WM8350_COMPARATOR_INT_STATUS_MASK, 0xFFFF);

	mutex_init(&wm8350->irq_mutex);
	wm8350->chip_irq = irq;

	if (pdata && pdata->irq_high) {
		flags |= IRQF_TRIGGER_HIGH;

		wm8350_set_bits(wm8350, WM8350_SYSTEM_CONTROL_1,
				WM8350_IRQ_POL);
	} else {
		flags |= IRQF_TRIGGER_LOW;

		wm8350_clear_bits(wm8350, WM8350_SYSTEM_CONTROL_1,
				  WM8350_IRQ_POL);
	}

	ret = request_threaded_irq(irq, NULL, wm8350_irq, flags,
				   "wm8350", wm8350);
	if (ret != 0)
		dev_err(wm8350->dev, "Failed to request IRQ: %d\n", ret);

	return ret;
}

int wm8350_irq_exit(struct wm8350 *wm8350)
{
	free_irq(wm8350->chip_irq, wm8350);
	return 0;
}
