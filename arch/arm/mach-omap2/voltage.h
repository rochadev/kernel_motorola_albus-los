/*
 * OMAP Voltage Management Routines
 *
 * Author: Thara Gopinath	<thara@ti.com>
 *
 * Copyright (C) 2009 Texas Instruments, Inc.
 * Thara Gopinath <thara@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ARCH_ARM_MACH_OMAP2_VOLTAGE_H
#define __ARCH_ARM_MACH_OMAP2_VOLTAGE_H

#include <linux/err.h>

#include "vc.h"
#include "vp.h"

struct powerdomain;

/* XXX document */
#define VOLTSCALE_VPFORCEUPDATE		1
#define VOLTSCALE_VCBYPASS		2

/*
 * OMAP3 GENERIC setup times. Revisit to see if these needs to be
 * passed from board or PMIC file
 */
#define OMAP3_CLKSETUP		0xff
#define OMAP3_VOLTOFFSET	0xff
#define OMAP3_VOLTSETUP2	0xff

struct omap_vdd_info;

/**
 * struct omap_vfsm_instance_data - per-voltage manager FSM register/bitfield
 * data
 * @voltsetup_mask: SETUP_TIME* bitmask in the PRM_VOLTSETUP* register
 * @voltsetup_reg: register offset of PRM_VOLTSETUP from PRM base
 * @voltsetup_shift: SETUP_TIME* field shift in the PRM_VOLTSETUP* register
 *
 * XXX What about VOLTOFFSET/VOLTCTRL?
 * XXX It is not necessary to have both a _mask and a _shift for the same
 *     bitfield - remove one!
 */
struct omap_vfsm_instance_data {
	u32 voltsetup_mask;
	u8 voltsetup_reg;
	u8 voltsetup_shift;
};

/**
 * struct voltagedomain - omap voltage domain global structure.
 * @name: Name of the voltage domain which can be used as a unique identifier.
 * @scalable: Whether or not this voltage domain is scalable
 * @node: list_head linking all voltage domains
 * @pwrdm_list: list_head linking all powerdomains in this voltagedomain
 * @vc: pointer to VC channel associated with this voltagedomain
 * @vdd: to be removed
 */
struct voltagedomain {
	char *name;
	bool scalable;
	struct list_head node;
	struct list_head pwrdm_list;
	struct omap_vc_channel *vc;

	struct omap_vdd_info *vdd;
};

/**
 * struct omap_volt_data - Omap voltage specific data.
 * @voltage_nominal:	The possible voltage value in uV
 * @sr_efuse_offs:	The offset of the efuse register(from system
 *			control module base address) from where to read
 *			the n-target value for the smartreflex module.
 * @sr_errminlimit:	Error min limit value for smartreflex. This value
 *			differs at differnet opp and thus is linked
 *			with voltage.
 * @vp_errorgain:	Error gain value for the voltage processor. This
 *			field also differs according to the voltage/opp.
 */
struct omap_volt_data {
	u32	volt_nominal;
	u32	sr_efuse_offs;
	u8	sr_errminlimit;
	u8	vp_errgain;
};

/**
 * struct omap_volt_pmic_info - PMIC specific data required by voltage driver.
 * @slew_rate:	PMIC slew rate (in uv/us)
 * @step_size:	PMIC voltage step size (in uv)
 * @vsel_to_uv:	PMIC API to convert vsel value to actual voltage in uV.
 * @uv_to_vsel:	PMIC API to convert voltage in uV to vsel value.
 * @volt_reg_addr: voltage configuration register address
 * @cmd_reg_addr: command (on, on-LP, ret, off) configuration register address
 */
struct omap_volt_pmic_info {
	int slew_rate;
	int step_size;
	u32 on_volt;
	u32 onlp_volt;
	u32 ret_volt;
	u32 off_volt;
	u16 volt_setup_time;
	u8 vp_erroroffset;
	u8 vp_vstepmin;
	u8 vp_vstepmax;
	u8 vp_vddmin;
	u8 vp_vddmax;
	u8 vp_timeout_us;
	u8 i2c_slave_addr;
	u8 volt_reg_addr;
	u8 cmd_reg_addr;
	unsigned long (*vsel_to_uv) (const u8 vsel);
	u8 (*uv_to_vsel) (unsigned long uV);
};

/**
 * omap_vdd_info - Per Voltage Domain info
 *
 * @volt_data		: voltage table having the distinct voltages supported
 *			  by the domain and other associated per voltage data.
 * @pmic_info		: pmic specific parameters which should be populted by
 *			  the pmic drivers.
 * @vp_data		: the register values, shifts, masks for various
 *			  vp registers
 * @vp_rt_data          : VP data derived at runtime, not predefined
 * @vfsm                : voltage manager FSM data
 * @debug_dir		: debug directory for this voltage domain.
 * @curr_volt		: current voltage for this vdd.
 * @prm_irqst_mod       : PRM module id used for PRM IRQ status register access
 * @vp_enabled		: flag to keep track of whether vp is enabled or not
 * @volt_scale		: API to scale the voltage of the vdd.
 */
struct omap_vdd_info {
	struct omap_volt_data *volt_data;
	struct omap_volt_pmic_info *pmic_info;
	struct omap_vp_instance_data *vp_data;
	struct omap_vp_runtime_data vp_rt_data;
	const struct omap_vfsm_instance_data *vfsm;
	struct dentry *debug_dir;
	u32 curr_volt;
	bool vp_enabled;

	s16 prm_irqst_mod;
	u8 prm_irqst_reg;
	u32 (*read_reg) (u16 mod, u8 offset);
	void (*write_reg) (u32 val, u16 mod, u8 offset);
	int (*volt_scale) (struct voltagedomain *voltdm,
		unsigned long target_volt);
};

int omap_voltage_scale_vdd(struct voltagedomain *voltdm,
		unsigned long target_volt);
void omap_voltage_reset(struct voltagedomain *voltdm);
void omap_voltage_get_volttable(struct voltagedomain *voltdm,
		struct omap_volt_data **volt_data);
struct omap_volt_data *omap_voltage_get_voltdata(struct voltagedomain *voltdm,
		unsigned long volt);
unsigned long omap_voltage_get_nom_volt(struct voltagedomain *voltdm);
struct dentry *omap_voltage_get_dbgdir(struct voltagedomain *voltdm);
#ifdef CONFIG_PM
int omap_voltage_register_pmic(struct voltagedomain *voltdm,
		struct omap_volt_pmic_info *pmic_info);
void omap_change_voltscale_method(struct voltagedomain *voltdm,
		int voltscale_method);
int omap_voltage_late_init(void);
#else
static inline int omap_voltage_register_pmic(struct voltagedomain *voltdm,
		struct omap_volt_pmic_info *pmic_info)
{
	return -EINVAL;
}
static inline  void omap_change_voltscale_method(struct voltagedomain *voltdm,
		int voltscale_method) {}
static inline int omap_voltage_late_init(void)
{
	return -EINVAL;
}
#endif

extern void omap2xxx_voltagedomains_init(void);
extern void omap3xxx_voltagedomains_init(void);
extern void omap44xx_voltagedomains_init(void);

struct voltagedomain *voltdm_lookup(const char *name);
void voltdm_init(struct voltagedomain **voltdm_list);
int voltdm_add_pwrdm(struct voltagedomain *voltdm, struct powerdomain *pwrdm);
int voltdm_for_each(int (*fn)(struct voltagedomain *voltdm, void *user),
		    void *user);
int voltdm_for_each_pwrdm(struct voltagedomain *voltdm,
			  int (*fn)(struct voltagedomain *voltdm,
				    struct powerdomain *pwrdm));
#endif
