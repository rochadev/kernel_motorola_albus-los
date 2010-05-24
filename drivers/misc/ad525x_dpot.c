/*
 * ad525x_dpot: Driver for the Analog Devices digital potentiometers
 * Copyright (c) 2009-2010 Analog Devices, Inc.
 * Author: Michael Hennerich <hennerich@blackfin.uclinux.org>
 *
 * DEVID		#Wipers		#Positions 	Resistor Options (kOhm)
 * AD5258		1		64		1, 10, 50, 100
 * AD5259		1		256		5, 10, 50, 100
 * AD5251		2		64		1, 10, 50, 100
 * AD5252		2		256		1, 10, 50, 100
 * AD5255		3		512		25, 250
 * AD5253		4		64		1, 10, 50, 100
 * AD5254		4		256		1, 10, 50, 100
 * AD5160		1		256		5, 10, 50, 100
 * AD5161		1		256		5, 10, 50, 100
 * AD5162		2		256		2.5, 10, 50, 100
 * AD5165		1		256		100
 * AD5200		1		256		10, 50
 * AD5201		1		33		10, 50
 * AD5203		4		64		10, 100
 * AD5204		4		256		10, 50, 100
 * AD5206		6		256		10, 50, 100
 * AD5207		2		256		10, 50, 100
 * AD5231		1		1024		10, 50, 100
 * AD5232		2		256		10, 50, 100
 * AD5233		4		64		10, 50, 100
 * AD5235		2		1024		25, 250
 * AD5260		1		256		20, 50, 200
 * AD5262		2		256		20, 50, 200
 * AD5263		4		256		20, 50, 200
 * AD5290		1		256		10, 50, 100
 * AD5291		1		256		20
 * AD5292		1		1024		20
 * AD5293		1		1024		20
 * AD7376		1		128		10, 50, 100, 1M
 * AD8400		1		256		1, 10, 50, 100
 * AD8402		2		256		1, 10, 50, 100
 * AD8403		4		256		1, 10, 50, 100
 * ADN2850		3		512		25, 250
 * AD5241		1		256		10, 100, 1M
 * AD5246		1		128		5, 10, 50, 100
 * AD5247		1		128		5, 10, 50, 100
 * AD5245		1		256		5, 10, 50, 100
 * AD5243		2		256		2.5, 10, 50, 100
 * AD5248		2		256		2.5, 10, 50, 100
 * AD5242		2		256		20, 50, 200
 * AD5280		1		256		20, 50, 200
 * AD5282		2		256		20, 50, 200
 * ADN2860		3		512		25, 250
 *
 * See Documentation/misc-devices/ad525x_dpot.txt for more info.
 *
 * derived from ad5258.c
 * Copyright (c) 2009 Cyber Switching, Inc.
 * Author: Chris Verges <chrisv@cyberswitching.com>
 *
 * derived from ad5252.c
 * Copyright (c) 2006 Michael Hennerich <hennerich@blackfin.uclinux.org>
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>

#define DRIVER_VERSION			"0.2"

#include "ad525x_dpot.h"

/*
 * Client data (each client gets its own)
 */

struct dpot_data {
	struct ad_dpot_bus_data	bdata;
	struct mutex update_lock;
	unsigned rdac_mask;
	unsigned max_pos;
	unsigned long devid;
	unsigned uid;
	unsigned feat;
	unsigned wipers;
	u16 rdac_cache[8];
};

static inline int dpot_read_d8(struct dpot_data *dpot)
{
	return dpot->bdata.bops->read_d8(dpot->bdata.client);
}

static inline int dpot_read_r8d8(struct dpot_data *dpot, u8 reg)
{
	return dpot->bdata.bops->read_r8d8(dpot->bdata.client, reg);
}

static inline int dpot_read_r8d16(struct dpot_data *dpot, u8 reg)
{
	return dpot->bdata.bops->read_r8d16(dpot->bdata.client, reg);
}

static inline int dpot_write_d8(struct dpot_data *dpot, u8 val)
{
	return dpot->bdata.bops->write_d8(dpot->bdata.client, val);
}

static inline int dpot_write_r8d8(struct dpot_data *dpot, u8 reg, u16 val)
{
	return dpot->bdata.bops->write_r8d8(dpot->bdata.client, reg, val);
}

static inline int dpot_write_r8d16(struct dpot_data *dpot, u8 reg, u16 val)
{
	return dpot->bdata.bops->write_r8d16(dpot->bdata.client, reg, val);
}

static s32 dpot_read_spi(struct dpot_data *dpot, u8 reg)
{
	unsigned ctrl = 0;

	if (!(reg & (DPOT_ADDR_EEPROM | DPOT_ADDR_CMD))) {

		if (dpot->feat & F_RDACS_WONLY)
			return dpot->rdac_cache[reg & DPOT_RDAC_MASK];

		if (dpot->uid == DPOT_UID(AD5291_ID) ||
			dpot->uid == DPOT_UID(AD5292_ID) ||
			dpot->uid == DPOT_UID(AD5293_ID))
			return dpot_read_r8d8(dpot,
				DPOT_AD5291_READ_RDAC << 2);

		ctrl = DPOT_SPI_READ_RDAC;
	} else if (reg & DPOT_ADDR_EEPROM) {
		ctrl = DPOT_SPI_READ_EEPROM;
	}

	if (dpot->feat & F_SPI_16BIT)
		return dpot_read_r8d8(dpot, ctrl);
	else if (dpot->feat & F_SPI_24BIT)
		return dpot_read_r8d16(dpot, ctrl);

	return -EFAULT;
}

static s32 dpot_read_i2c(struct dpot_data *dpot, u8 reg)
{
	unsigned ctrl = 0;
	switch (dpot->uid) {
	case DPOT_UID(AD5246_ID):
	case DPOT_UID(AD5247_ID):
		return dpot_read_d8(dpot);
	case DPOT_UID(AD5245_ID):
	case DPOT_UID(AD5241_ID):
	case DPOT_UID(AD5242_ID):
	case DPOT_UID(AD5243_ID):
	case DPOT_UID(AD5248_ID):
	case DPOT_UID(AD5280_ID):
	case DPOT_UID(AD5282_ID):
		ctrl = ((reg & DPOT_RDAC_MASK) == DPOT_RDAC0) ?
			0 : DPOT_AD5291_RDAC_AB;
		return dpot_read_r8d8(dpot, ctrl);
	default:
		if ((reg & DPOT_REG_TOL) || (dpot->max_pos > 256))
			return dpot_read_r8d16(dpot, (reg & 0xF8) |
					((reg & 0x7) << 1));
		else
			return dpot_read_r8d8(dpot, reg);
	}
}

static s32 dpot_read(struct dpot_data *dpot, u8 reg)
{
	if (dpot->feat & F_SPI)
		return dpot_read_spi(dpot, reg);
	else
		return dpot_read_i2c(dpot, reg);
}

static s32 dpot_write_spi(struct dpot_data *dpot, u8 reg, u16 value)
{
	unsigned val = 0;

	if (!(reg & (DPOT_ADDR_EEPROM | DPOT_ADDR_CMD))) {
		if (dpot->feat & F_RDACS_WONLY)
			dpot->rdac_cache[reg & DPOT_RDAC_MASK] = value;

		if (dpot->feat & F_AD_APPDATA) {
			if (dpot->feat & F_SPI_8BIT) {
				val = ((reg & DPOT_RDAC_MASK) <<
					DPOT_MAX_POS(dpot->devid)) |
					value;
				return dpot_write_d8(dpot, val);
			} else if (dpot->feat & F_SPI_16BIT) {
				val = ((reg & DPOT_RDAC_MASK) <<
					DPOT_MAX_POS(dpot->devid)) |
					value;
				return dpot_write_r8d8(dpot, val >> 8,
					val & 0xFF);
			} else
				BUG();
		} else {
			if (dpot->uid == DPOT_UID(AD5291_ID) ||
				dpot->uid == DPOT_UID(AD5292_ID) ||
				dpot->uid == DPOT_UID(AD5293_ID))
				return dpot_write_r8d8(dpot,
					(DPOT_AD5291_RDAC << 2) |
					(value >> 8), value & 0xFF);

			val = DPOT_SPI_RDAC | (reg & DPOT_RDAC_MASK);
		}
	} else if (reg & DPOT_ADDR_EEPROM) {
		val = DPOT_SPI_EEPROM | (reg & DPOT_RDAC_MASK);
	} else if (reg & DPOT_ADDR_CMD) {
		switch (reg) {
		case DPOT_DEC_ALL_6DB:
			val = DPOT_SPI_DEC_ALL_6DB;
			break;
		case DPOT_INC_ALL_6DB:
			val = DPOT_SPI_INC_ALL_6DB;
			break;
		case DPOT_DEC_ALL:
			val = DPOT_SPI_DEC_ALL;
			break;
		case DPOT_INC_ALL:
			val = DPOT_SPI_INC_ALL;
			break;
		}
	} else
		BUG();

	if (dpot->feat & F_SPI_16BIT)
		return dpot_write_r8d8(dpot, val, value);
	else if (dpot->feat & F_SPI_24BIT)
		return dpot_write_r8d16(dpot, val, value);

	return -EFAULT;
}

static s32 dpot_write_i2c(struct dpot_data *dpot, u8 reg, u16 value)
{
	/* Only write the instruction byte for certain commands */
	unsigned ctrl = 0;

	switch (dpot->uid) {
	case DPOT_UID(AD5246_ID):
	case DPOT_UID(AD5247_ID):
		return dpot_write_d8(dpot, value);
		break;

	case DPOT_UID(AD5245_ID):
	case DPOT_UID(AD5241_ID):
	case DPOT_UID(AD5242_ID):
	case DPOT_UID(AD5243_ID):
	case DPOT_UID(AD5248_ID):
	case DPOT_UID(AD5280_ID):
	case DPOT_UID(AD5282_ID):
		ctrl = ((reg & DPOT_RDAC_MASK) == DPOT_RDAC0) ?
			0 : DPOT_AD5291_RDAC_AB;
		return dpot_write_r8d8(dpot, ctrl, value);
		break;
	default:
		if (reg & DPOT_ADDR_CMD)
			return dpot_write_d8(dpot, reg);

		if (dpot->max_pos > 256)
			return dpot_write_r8d16(dpot, (reg & 0xF8) |
						((reg & 0x7) << 1), value);
		else
			/* All other registers require instruction + data bytes */
			return dpot_write_r8d8(dpot, reg, value);
	}
}


static s32 dpot_write(struct dpot_data *dpot, u8 reg, u16 value)
{
	if (dpot->feat & F_SPI)
		return dpot_write_spi(dpot, reg, value);
	else
		return dpot_write_i2c(dpot, reg, value);
}

/* sysfs functions */

static ssize_t sysfs_show_reg(struct device *dev,
			      struct device_attribute *attr,
			      char *buf, u32 reg)
{
	struct dpot_data *data = dev_get_drvdata(dev);
	s32 value;

	mutex_lock(&data->update_lock);
	value = dpot_read(data, reg);
	mutex_unlock(&data->update_lock);

	if (value < 0)
		return -EINVAL;
	/*
	 * Let someone else deal with converting this ...
	 * the tolerance is a two-byte value where the MSB
	 * is a sign + integer value, and the LSB is a
	 * decimal value.  See page 18 of the AD5258
	 * datasheet (Rev. A) for more details.
	 */

	if (reg & DPOT_REG_TOL)
		return sprintf(buf, "0x%04x\n", value & 0xFFFF);
	else
		return sprintf(buf, "%u\n", value & data->rdac_mask);
}

static ssize_t sysfs_set_reg(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count, u32 reg)
{
	struct dpot_data *data = dev_get_drvdata(dev);
	unsigned long value;
	int err;

	err = strict_strtoul(buf, 10, &value);
	if (err)
		return err;

	if (value > data->rdac_mask)
		value = data->rdac_mask;

	mutex_lock(&data->update_lock);
	dpot_write(data, reg, value);
	if (reg & DPOT_ADDR_EEPROM)
		msleep(26);	/* Sleep while the EEPROM updates */
	mutex_unlock(&data->update_lock);

	return count;
}

static ssize_t sysfs_do_cmd(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count, u32 reg)
{
	struct dpot_data *data = dev_get_drvdata(dev);

	mutex_lock(&data->update_lock);
	dpot_write(data, reg, 0);
	mutex_unlock(&data->update_lock);

	return count;
}

/* ------------------------------------------------------------------------- */

#define DPOT_DEVICE_SHOW(_name, _reg) static ssize_t \
show_##_name(struct device *dev, \
			  struct device_attribute *attr, char *buf) \
{ \
	return sysfs_show_reg(dev, attr, buf, _reg); \
}

#define DPOT_DEVICE_SET(_name, _reg) static ssize_t \
set_##_name(struct device *dev, \
			 struct device_attribute *attr, \
			 const char *buf, size_t count) \
{ \
	return sysfs_set_reg(dev, attr, buf, count, _reg); \
}

#define DPOT_DEVICE_SHOW_SET(name, reg) \
DPOT_DEVICE_SHOW(name, reg) \
DPOT_DEVICE_SET(name, reg) \
static DEVICE_ATTR(name, S_IWUSR | S_IRUGO, show_##name, set_##name);

#define DPOT_DEVICE_SHOW_ONLY(name, reg) \
DPOT_DEVICE_SHOW(name, reg) \
static DEVICE_ATTR(name, S_IWUSR | S_IRUGO, show_##name, NULL);

DPOT_DEVICE_SHOW_SET(rdac0, DPOT_ADDR_RDAC | DPOT_RDAC0);
DPOT_DEVICE_SHOW_SET(eeprom0, DPOT_ADDR_EEPROM | DPOT_RDAC0);
DPOT_DEVICE_SHOW_ONLY(tolerance0, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC0);

DPOT_DEVICE_SHOW_SET(rdac1, DPOT_ADDR_RDAC | DPOT_RDAC1);
DPOT_DEVICE_SHOW_SET(eeprom1, DPOT_ADDR_EEPROM | DPOT_RDAC1);
DPOT_DEVICE_SHOW_ONLY(tolerance1, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC1);

DPOT_DEVICE_SHOW_SET(rdac2, DPOT_ADDR_RDAC | DPOT_RDAC2);
DPOT_DEVICE_SHOW_SET(eeprom2, DPOT_ADDR_EEPROM | DPOT_RDAC2);
DPOT_DEVICE_SHOW_ONLY(tolerance2, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC2);

DPOT_DEVICE_SHOW_SET(rdac3, DPOT_ADDR_RDAC | DPOT_RDAC3);
DPOT_DEVICE_SHOW_SET(eeprom3, DPOT_ADDR_EEPROM | DPOT_RDAC3);
DPOT_DEVICE_SHOW_ONLY(tolerance3, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC3);

DPOT_DEVICE_SHOW_SET(rdac4, DPOT_ADDR_RDAC | DPOT_RDAC4);
DPOT_DEVICE_SHOW_SET(eeprom4, DPOT_ADDR_EEPROM | DPOT_RDAC4);
DPOT_DEVICE_SHOW_ONLY(tolerance4, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC4);

DPOT_DEVICE_SHOW_SET(rdac5, DPOT_ADDR_RDAC | DPOT_RDAC5);
DPOT_DEVICE_SHOW_SET(eeprom5, DPOT_ADDR_EEPROM | DPOT_RDAC5);
DPOT_DEVICE_SHOW_ONLY(tolerance5, DPOT_ADDR_EEPROM | DPOT_TOL_RDAC5);

static const struct attribute *dpot_attrib_wipers[] = {
	&dev_attr_rdac0.attr,
	&dev_attr_rdac1.attr,
	&dev_attr_rdac2.attr,
	&dev_attr_rdac3.attr,
	&dev_attr_rdac4.attr,
	&dev_attr_rdac5.attr,
	NULL
};

static const struct attribute *dpot_attrib_eeprom[] = {
	&dev_attr_eeprom0.attr,
	&dev_attr_eeprom1.attr,
	&dev_attr_eeprom2.attr,
	&dev_attr_eeprom3.attr,
	&dev_attr_eeprom4.attr,
	&dev_attr_eeprom5.attr,
	NULL
};

static const struct attribute *dpot_attrib_tolerance[] = {
	&dev_attr_tolerance0.attr,
	&dev_attr_tolerance1.attr,
	&dev_attr_tolerance2.attr,
	&dev_attr_tolerance3.attr,
	&dev_attr_tolerance4.attr,
	&dev_attr_tolerance5.attr,
	NULL
};

/* ------------------------------------------------------------------------- */

#define DPOT_DEVICE_DO_CMD(_name, _cmd) static ssize_t \
set_##_name(struct device *dev, \
			 struct device_attribute *attr, \
			 const char *buf, size_t count) \
{ \
	return sysfs_do_cmd(dev, attr, buf, count, _cmd); \
} \
static DEVICE_ATTR(_name, S_IWUSR | S_IRUGO, NULL, set_##_name);

DPOT_DEVICE_DO_CMD(inc_all, DPOT_INC_ALL);
DPOT_DEVICE_DO_CMD(dec_all, DPOT_DEC_ALL);
DPOT_DEVICE_DO_CMD(inc_all_6db, DPOT_INC_ALL_6DB);
DPOT_DEVICE_DO_CMD(dec_all_6db, DPOT_DEC_ALL_6DB);

static struct attribute *ad525x_attributes_commands[] = {
	&dev_attr_inc_all.attr,
	&dev_attr_dec_all.attr,
	&dev_attr_inc_all_6db.attr,
	&dev_attr_dec_all_6db.attr,
	NULL
};

static const struct attribute_group ad525x_group_commands = {
	.attrs = ad525x_attributes_commands,
};

__devinit int ad_dpot_add_files(struct device *dev,
		unsigned features, unsigned rdac)
{
	int err = sysfs_create_file(&dev->kobj,
		dpot_attrib_wipers[rdac]);
	if (features & F_CMD_EEP)
		err |= sysfs_create_file(&dev->kobj,
			dpot_attrib_eeprom[rdac]);
	if (features & F_CMD_TOL)
		err |= sysfs_create_file(&dev->kobj,
			dpot_attrib_tolerance[rdac]);

	if (err)
		dev_err(dev, "failed to register sysfs hooks for RDAC%d\n",
			rdac);

	return err;
}

inline void ad_dpot_remove_files(struct device *dev,
		unsigned features, unsigned rdac)
{
	sysfs_remove_file(&dev->kobj,
		dpot_attrib_wipers[rdac]);
	if (features & F_CMD_EEP)
		sysfs_remove_file(&dev->kobj,
			dpot_attrib_eeprom[rdac]);
	if (features & F_CMD_TOL)
		sysfs_remove_file(&dev->kobj,
			dpot_attrib_tolerance[rdac]);
}

__devinit int ad_dpot_probe(struct device *dev,
		struct ad_dpot_bus_data *bdata, const struct ad_dpot_id *id)
{

	struct dpot_data *data;
	int i, err = 0;

	data = kzalloc(sizeof(struct dpot_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto exit;
	}

	dev_set_drvdata(dev, data);
	mutex_init(&data->update_lock);

	data->bdata = *bdata;
	data->devid = id->devid;

	data->max_pos = 1 << DPOT_MAX_POS(data->devid);
	data->rdac_mask = data->max_pos - 1;
	data->feat = DPOT_FEAT(data->devid);
	data->uid = DPOT_UID(data->devid);
	data->wipers = DPOT_WIPERS(data->devid);

	for (i = DPOT_RDAC0; i <= DPOT_RDAC5; i++)
		if (data->wipers & (1 << i)) {
			err = ad_dpot_add_files(dev, data->feat, i);
			if (err)
				goto exit_remove_files;
			/* power-up midscale */
			if (data->feat & F_RDACS_WONLY)
				data->rdac_cache[i] = data->max_pos / 2;
		}

	if (data->feat & F_CMD_INC)
		err = sysfs_create_group(&dev->kobj, &ad525x_group_commands);

	if (err) {
		dev_err(dev, "failed to register sysfs hooks\n");
		goto exit_free;
	}

	dev_info(dev, "%s %d-Position Digital Potentiometer registered\n",
		 id->name, data->max_pos);

	return 0;

exit_remove_files:
	for (i = DPOT_RDAC0; i <= DPOT_RDAC5; i++)
		if (data->wipers & (1 << i))
			ad_dpot_remove_files(dev, data->feat, i);

exit_free:
	kfree(data);
	dev_set_drvdata(dev, NULL);
exit:
	dev_err(dev, "failed to create client for %s ID 0x%lX\n",
			id->name, id->devid);
	return err;
}
EXPORT_SYMBOL(ad_dpot_probe);

__devexit int ad_dpot_remove(struct device *dev)
{
	struct dpot_data *data = dev_get_drvdata(dev);
	int i;

	for (i = DPOT_RDAC0; i <= DPOT_RDAC5; i++)
		if (data->wipers & (1 << i))
			ad_dpot_remove_files(dev, data->feat, i);

	kfree(data);

	return 0;
}
EXPORT_SYMBOL(ad_dpot_remove);


MODULE_AUTHOR("Chris Verges <chrisv@cyberswitching.com>, "
	      "Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("Digital potentiometer driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
