/*
 * max77693-private.h - Voltage regulator driver for the Maxim 77693
 *
 *  Copyright (C) 2012 Samsung Electrnoics
 *  SangYoung Son <hello.son@samsung.com>
 *
 * This program is not provided / owned by Maxim Integrated Products.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __LINUX_MFD_MAX77693_PRIV_H
#define __LINUX_MFD_MAX77693_PRIV_H

#include <linux/i2c.h>

#define MAX77693_NUM_IRQ_MUIC_REGS	3
#define MAX77693_REG_INVALID		(0xff)

/* Slave addr = 0xCC: PMIC, Charger, Flash LED */
enum max77693_pmic_reg {
	MAX77693_LED_REG_IFLASH1			= 0x00,
	MAX77693_LED_REG_IFLASH2			= 0x01,
	MAX77693_LED_REG_ITORCH				= 0x02,
	MAX77693_LED_REG_ITORCHTIMER			= 0x03,
	MAX77693_LED_REG_FLASH_TIMER			= 0x04,
	MAX77693_LED_REG_FLASH_EN			= 0x05,
	MAX77693_LED_REG_MAX_FLASH1			= 0x06,
	MAX77693_LED_REG_MAX_FLASH2			= 0x07,
	MAX77693_LED_REG_MAX_FLASH3			= 0x08,
	MAX77693_LED_REG_MAX_FLASH4			= 0x09,
	MAX77693_LED_REG_VOUT_CNTL			= 0x0A,
	MAX77693_LED_REG_VOUT_FLASH1			= 0x0B,
	MAX77693_LED_REG_VOUT_FLASH2			= 0x0C,
	MAX77693_LED_REG_FLASH_INT			= 0x0E,
	MAX77693_LED_REG_FLASH_INT_MASK			= 0x0F,
	MAX77693_LED_REG_FLASH_INT_STATUS		= 0x10,

	MAX77693_PMIC_REG_PMIC_ID1			= 0x20,
	MAX77693_PMIC_REG_PMIC_ID2			= 0x21,
	MAX77693_PMIC_REG_INTSRC			= 0x22,
	MAX77693_PMIC_REG_INTSRC_MASK			= 0x23,
	MAX77693_PMIC_REG_TOPSYS_INT			= 0x24,
	MAX77693_PMIC_REG_TOPSYS_INT_MASK		= 0x26,
	MAX77693_PMIC_REG_TOPSYS_STAT			= 0x28,
	MAX77693_PMIC_REG_MAINCTRL1			= 0x2A,
	MAX77693_PMIC_REG_LSCNFG			= 0x2B,

	MAX77693_CHG_REG_CHG_INT			= 0xB0,
	MAX77693_CHG_REG_CHG_INT_MASK			= 0xB1,
	MAX77693_CHG_REG_CHG_INT_OK			= 0xB2,
	MAX77693_CHG_REG_CHG_DETAILS_00			= 0xB3,
	MAX77693_CHG_REG_CHG_DETAILS_01			= 0xB4,
	MAX77693_CHG_REG_CHG_DETAILS_02			= 0xB5,
	MAX77693_CHG_REG_CHG_DETAILS_03			= 0xB6,
	MAX77693_CHG_REG_CHG_CNFG_00			= 0xB7,
	MAX77693_CHG_REG_CHG_CNFG_01			= 0xB8,
	MAX77693_CHG_REG_CHG_CNFG_02			= 0xB9,
	MAX77693_CHG_REG_CHG_CNFG_03			= 0xBA,
	MAX77693_CHG_REG_CHG_CNFG_04			= 0xBB,
	MAX77693_CHG_REG_CHG_CNFG_05			= 0xBC,
	MAX77693_CHG_REG_CHG_CNFG_06			= 0xBD,
	MAX77693_CHG_REG_CHG_CNFG_07			= 0xBE,
	MAX77693_CHG_REG_CHG_CNFG_08			= 0xBF,
	MAX77693_CHG_REG_CHG_CNFG_09			= 0xC0,
	MAX77693_CHG_REG_CHG_CNFG_10			= 0xC1,
	MAX77693_CHG_REG_CHG_CNFG_11			= 0xC2,
	MAX77693_CHG_REG_CHG_CNFG_12			= 0xC3,
	MAX77693_CHG_REG_CHG_CNFG_13			= 0xC4,
	MAX77693_CHG_REG_CHG_CNFG_14			= 0xC5,
	MAX77693_CHG_REG_SAFEOUT_CTRL			= 0xC6,

	MAX77693_PMIC_REG_END,
};

/* Slave addr = 0x4A: MUIC */
enum max77693_muic_reg {
	MAX77693_MUIC_REG_ID		= 0x00,
	MAX77693_MUIC_REG_INT1		= 0x01,
	MAX77693_MUIC_REG_INT2		= 0x02,
	MAX77693_MUIC_REG_INT3		= 0x03,
	MAX77693_MUIC_REG_STATUS1	= 0x04,
	MAX77693_MUIC_REG_STATUS2	= 0x05,
	MAX77693_MUIC_REG_STATUS3	= 0x06,
	MAX77693_MUIC_REG_INTMASK1	= 0x07,
	MAX77693_MUIC_REG_INTMASK2	= 0x08,
	MAX77693_MUIC_REG_INTMASK3	= 0x09,
	MAX77693_MUIC_REG_CDETCTRL1	= 0x0A,
	MAX77693_MUIC_REG_CDETCTRL2	= 0x0B,
	MAX77693_MUIC_REG_CTRL1		= 0x0C,
	MAX77693_MUIC_REG_CTRL2		= 0x0D,
	MAX77693_MUIC_REG_CTRL3		= 0x0E,

	MAX77693_MUIC_REG_END,
};

/* Slave addr = 0x90: Haptic */
enum max77693_haptic_reg {
	MAX77693_HAPTIC_REG_STATUS		= 0x00,
	MAX77693_HAPTIC_REG_CONFIG1		= 0x01,
	MAX77693_HAPTIC_REG_CONFIG2		= 0x02,
	MAX77693_HAPTIC_REG_CONFIG_CHNL		= 0x03,
	MAX77693_HAPTIC_REG_CONFG_CYC1		= 0x04,
	MAX77693_HAPTIC_REG_CONFG_CYC2		= 0x05,
	MAX77693_HAPTIC_REG_CONFIG_PER1		= 0x06,
	MAX77693_HAPTIC_REG_CONFIG_PER2		= 0x07,
	MAX77693_HAPTIC_REG_CONFIG_PER3		= 0x08,
	MAX77693_HAPTIC_REG_CONFIG_PER4		= 0x09,
	MAX77693_HAPTIC_REG_CONFIG_DUTY1	= 0x0A,
	MAX77693_HAPTIC_REG_CONFIG_DUTY2	= 0x0B,
	MAX77693_HAPTIC_REG_CONFIG_PWM1		= 0x0C,
	MAX77693_HAPTIC_REG_CONFIG_PWM2		= 0x0D,
	MAX77693_HAPTIC_REG_CONFIG_PWM3		= 0x0E,
	MAX77693_HAPTIC_REG_CONFIG_PWM4		= 0x0F,
	MAX77693_HAPTIC_REG_REV			= 0x10,

	MAX77693_HAPTIC_REG_END,
};

enum max77693_irq_source {
	LED_INT = 0,
	TOPSYS_INT,
	CHG_INT,
	MUIC_INT1,
	MUIC_INT2,
	MUIC_INT3,

	MAX77693_IRQ_GROUP_NR,
};

enum max77693_irq {
	/* PMIC - FLASH */
	MAX77693_LED_IRQ_FLED2_OPEN,
	MAX77693_LED_IRQ_FLED2_SHORT,
	MAX77693_LED_IRQ_FLED1_OPEN,
	MAX77693_LED_IRQ_FLED1_SHORT,
	MAX77693_LED_IRQ_MAX_FLASH,

	/* PMIC - TOPSYS */
	MAX77693_TOPSYS_IRQ_T120C_INT,
	MAX77693_TOPSYS_IRQ_T140C_INT,
	MAX77693_TOPSYS_IRQ_LOWSYS_INT,

	/* PMIC - Charger */
	MAX77693_CHG_IRQ_BYP_I,
	MAX77693_CHG_IRQ_THM_I,
	MAX77693_CHG_IRQ_BAT_I,
	MAX77693_CHG_IRQ_CHG_I,
	MAX77693_CHG_IRQ_CHGIN_I,

	/* MUIC INT1 */
	MAX77693_MUIC_IRQ_INT1_ADC,
	MAX77693_MUIC_IRQ_INT1_ADC_LOW,
	MAX77693_MUIC_IRQ_INT1_ADC_ERR,
	MAX77693_MUIC_IRQ_INT1_ADC1K,

	/* MUIC INT2 */
	MAX77693_MUIC_IRQ_INT2_CHGTYP,
	MAX77693_MUIC_IRQ_INT2_CHGDETREUN,
	MAX77693_MUIC_IRQ_INT2_DCDTMR,
	MAX77693_MUIC_IRQ_INT2_DXOVP,
	MAX77693_MUIC_IRQ_INT2_VBVOLT,
	MAX77693_MUIC_IRQ_INT2_VIDRM,

	/* MUIC INT3 */
	MAX77693_MUIC_IRQ_INT3_EOC,
	MAX77693_MUIC_IRQ_INT3_CGMBC,
	MAX77693_MUIC_IRQ_INT3_OVP,
	MAX77693_MUIC_IRQ_INT3_MBCCHG_ERR,
	MAX77693_MUIC_IRQ_INT3_CHG_ENABLED,
	MAX77693_MUIC_IRQ_INT3_BAT_DET,

	MAX77693_IRQ_NR,
};

struct max77693_dev {
	struct device *dev;
	struct i2c_client *i2c;		/* 0xCC , PMIC, Charger, Flash LED */
	struct i2c_client *muic;	/* 0x4A , MUIC */
	struct i2c_client *haptic;	/* 0x90 , Haptic */
	struct mutex iolock;

	int type;

	struct regmap *regmap;
	struct regmap *regmap_muic;
	struct regmap *regmap_haptic;

	int irq;
	bool wakeup;
};

enum max77693_types {
	TYPE_MAX77693,
};

extern int max77693_read_reg(struct regmap *map, u8 reg, u8 *dest);
extern int max77693_bulk_read(struct regmap *map, u8 reg, int count,
				u8 *buf);
extern int max77693_write_reg(struct regmap *map, u8 reg, u8 value);
extern int max77693_bulk_write(struct regmap *map, u8 reg, int count,
				u8 *buf);
extern int max77693_update_reg(struct regmap *map, u8 reg, u8 val, u8 mask);

#endif /*  __LINUX_MFD_MAX77693_PRIV_H */
