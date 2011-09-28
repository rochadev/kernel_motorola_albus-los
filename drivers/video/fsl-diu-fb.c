/*
 * Copyright 2008 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 *  Freescale DIU Frame Buffer device driver
 *
 *  Authors: Hongjun Chen <hong-jun.chen@freescale.com>
 *           Paul Widmer <paul.widmer@freescale.com>
 *           Srikanth Srinivasan <srikanth.srinivasan@freescale.com>
 *           York Sun <yorksun@freescale.com>
 *
 *   Based on imxfb.c Copyright (C) 2004 S.Hauer, Pengutronix
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include <sysdev/fsl_soc.h>
#include <linux/fsl-diu-fb.h>
#include "edid.h"

#define FSL_AOI_NUM	6	/* 5 AOIs and one dummy AOI */
				/* 1 for plane 0, 2 for plane 1&2 each */

/* HW cursor parameters */
#define MAX_CURS		32

/* INT_STATUS/INT_MASK field descriptions */
#define INT_VSYNC	0x01	/* Vsync interrupt  */
#define INT_VSYNC_WB	0x02	/* Vsync interrupt for write back operation */
#define INT_UNDRUN	0x04	/* Under run exception interrupt */
#define INT_PARERR	0x08	/* Display parameters error interrupt */
#define INT_LS_BF_VS	0x10	/* Lines before vsync. interrupt */

struct diu_hw {
	struct diu __iomem *diu_reg;
	spinlock_t reg_lock;
	unsigned int mode;		/* DIU operation mode */
};

struct diu_addr {
	void *vaddr;		/* Virtual address */
	dma_addr_t paddr;	/* Physical address */
	__u32 offset;
};

struct diu_pool {
	struct diu_addr ad;
	struct diu_addr gamma;
	struct diu_addr pallete;
	struct diu_addr cursor;
};

/*
 * List of supported video modes
 *
 * The first entry is the default video mode.  The remain entries are in
 * order if increasing resolution and frequency.  The 320x240-60 mode is
 * the initial AOI for the second and third planes.
 */
static struct fb_videomode __devinitdata fsl_diu_mode_db[] = {
	{
		.refresh	= 60,
		.xres		= 1024,
		.yres		= 768,
		.pixclock	= 15385,
		.left_margin	= 160,
		.right_margin	= 24,
		.upper_margin	= 29,
		.lower_margin	= 3,
		.hsync_len	= 136,
		.vsync_len	= 6,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 320,
		.yres		= 240,
		.pixclock	= 79440,
		.left_margin	= 16,
		.right_margin	= 16,
		.upper_margin	= 16,
		.lower_margin	= 5,
		.hsync_len	= 48,
		.vsync_len	= 1,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 60,
		.xres           = 640,
		.yres           = 480,
		.pixclock       = 39722,
		.left_margin    = 48,
		.right_margin   = 16,
		.upper_margin   = 33,
		.lower_margin   = 10,
		.hsync_len      = 96,
		.vsync_len      = 2,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 72,
		.xres           = 640,
		.yres           = 480,
		.pixclock       = 32052,
		.left_margin    = 128,
		.right_margin   = 24,
		.upper_margin   = 28,
		.lower_margin   = 9,
		.hsync_len      = 40,
		.vsync_len      = 3,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 75,
		.xres           = 640,
		.yres           = 480,
		.pixclock       = 31747,
		.left_margin    = 120,
		.right_margin   = 16,
		.upper_margin   = 16,
		.lower_margin   = 1,
		.hsync_len      = 64,
		.vsync_len      = 3,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 90,
		.xres           = 640,
		.yres           = 480,
		.pixclock       = 25057,
		.left_margin    = 120,
		.right_margin   = 32,
		.upper_margin   = 14,
		.lower_margin   = 25,
		.hsync_len      = 40,
		.vsync_len      = 14,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 100,
		.xres           = 640,
		.yres           = 480,
		.pixclock       = 22272,
		.left_margin    = 48,
		.right_margin   = 32,
		.upper_margin   = 17,
		.lower_margin   = 22,
		.hsync_len      = 128,
		.vsync_len      = 12,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 800,
		.yres		= 480,
		.pixclock	= 33805,
		.left_margin	= 96,
		.right_margin	= 24,
		.upper_margin	= 10,
		.lower_margin	= 3,
		.hsync_len	= 72,
		.vsync_len	= 7,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh        = 60,
		.xres           = 800,
		.yres           = 600,
		.pixclock       = 25000,
		.left_margin    = 88,
		.right_margin   = 40,
		.upper_margin   = 23,
		.lower_margin   = 1,
		.hsync_len      = 128,
		.vsync_len      = 4,
		.sync           = FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode          = FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 854,
		.yres		= 480,
		.pixclock	= 31518,
		.left_margin	= 104,
		.right_margin	= 16,
		.upper_margin	= 13,
		.lower_margin	= 1,
		.hsync_len	= 88,
		.vsync_len	= 3,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 70,
		.xres		= 1024,
		.yres		= 768,
		.pixclock	= 16886,
		.left_margin	= 3,
		.right_margin	= 3,
		.upper_margin	= 2,
		.lower_margin	= 2,
		.hsync_len	= 40,
		.vsync_len	= 18,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 75,
		.xres		= 1024,
		.yres		= 768,
		.pixclock	= 15009,
		.left_margin	= 3,
		.right_margin	= 3,
		.upper_margin	= 2,
		.lower_margin	= 2,
		.hsync_len	= 80,
		.vsync_len	= 32,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 1280,
		.yres		= 480,
		.pixclock	= 18939,
		.left_margin	= 353,
		.right_margin	= 47,
		.upper_margin	= 39,
		.lower_margin	= 4,
		.hsync_len	= 8,
		.vsync_len	= 2,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 1280,
		.yres		= 720,
		.pixclock	= 13426,
		.left_margin	= 192,
		.right_margin	= 64,
		.upper_margin	= 22,
		.lower_margin	= 1,
		.hsync_len	= 136,
		.vsync_len	= 3,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 1280,
		.yres		= 1024,
		.pixclock	= 9375,
		.left_margin	= 38,
		.right_margin	= 128,
		.upper_margin	= 2,
		.lower_margin	= 7,
		.hsync_len	= 216,
		.vsync_len	= 37,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 70,
		.xres		= 1280,
		.yres		= 1024,
		.pixclock	= 9380,
		.left_margin	= 6,
		.right_margin	= 6,
		.upper_margin	= 4,
		.lower_margin	= 4,
		.hsync_len	= 60,
		.vsync_len	= 94,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 75,
		.xres		= 1280,
		.yres		= 1024,
		.pixclock	= 9380,
		.left_margin	= 6,
		.right_margin	= 6,
		.upper_margin	= 4,
		.lower_margin	= 4,
		.hsync_len	= 60,
		.vsync_len	= 15,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
	{
		.refresh	= 60,
		.xres		= 1920,
		.yres		= 1080,
		.pixclock	= 5787,
		.left_margin	= 328,
		.right_margin	= 120,
		.upper_margin	= 34,
		.lower_margin	= 1,
		.hsync_len	= 208,
		.vsync_len	= 3,
		.sync		= FB_SYNC_COMP_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
		.vmode		= FB_VMODE_NONINTERLACED
	},
};

static char *fb_mode;
static unsigned long default_bpp = 32;
static enum fsl_diu_monitor_port monitor_port;
static char *monitor_string;

#if defined(CONFIG_NOT_COHERENT_CACHE)
static u8 *coherence_data;
static size_t coherence_data_size;
static unsigned int d_cache_line_size;
#endif

static DEFINE_SPINLOCK(diu_lock);

struct fsl_diu_data {
	struct fb_info *fsl_diu_info[FSL_AOI_NUM - 1];
				/*FSL_AOI_NUM has one dummy AOI */
	struct device_attribute dev_attr;
	struct diu_ad *dummy_ad;
	void *dummy_aoi_virt;
	unsigned int irq;
	int fb_enabled;
	enum fsl_diu_monitor_port monitor_port;
};

enum mfb_index {
	PLANE0 = 0,	/* Plane 0, only one AOI that fills the screen */
	PLANE1_AOI0,	/* Plane 1, first AOI */
	PLANE1_AOI1,	/* Plane 1, second AOI */
	PLANE2_AOI0,	/* Plane 2, first AOI */
	PLANE2_AOI1,	/* Plane 2, second AOI */
};

struct mfb_info {
	enum mfb_index index;
	char *id;
	int registered;
	unsigned long pseudo_palette[16];
	struct diu_ad *ad;
	int cursor_reset;
	unsigned char g_alpha;
	unsigned int count;
	int x_aoi_d;		/* aoi display x offset to physical screen */
	int y_aoi_d;		/* aoi display y offset to physical screen */
	struct fsl_diu_data *parent;
	u8 *edid_data;
};


static struct mfb_info mfb_template[] = {
	{
		.index = PLANE0,
		.id = "Panel0",
		.registered = 0,
		.count = 0,
		.x_aoi_d = 0,
		.y_aoi_d = 0,
	},
	{
		.index = PLANE1_AOI0,
		.id = "Panel1 AOI0",
		.registered = 0,
		.g_alpha = 0xff,
		.count = 0,
		.x_aoi_d = 0,
		.y_aoi_d = 0,
	},
	{
		.index = PLANE1_AOI1,
		.id = "Panel1 AOI1",
		.registered = 0,
		.g_alpha = 0xff,
		.count = 0,
		.x_aoi_d = 0,
		.y_aoi_d = 480,
	},
	{
		.index = PLANE2_AOI0,
		.id = "Panel2 AOI0",
		.registered = 0,
		.g_alpha = 0xff,
		.count = 0,
		.x_aoi_d = 640,
		.y_aoi_d = 0,
	},
	{
		.index = PLANE2_AOI1,
		.id = "Panel2 AOI1",
		.registered = 0,
		.g_alpha = 0xff,
		.count = 0,
		.x_aoi_d = 640,
		.y_aoi_d = 480,
	},
};

static struct diu_hw dr = {
	.mode = MFB_MODE1,
	.reg_lock = __SPIN_LOCK_UNLOCKED(diu_hw.reg_lock),
};

static struct diu_pool pool;

/**
 * fsl_diu_name_to_port - convert a port name to a monitor port enum
 *
 * Takes the name of a monitor port ("dvi", "lvds", or "dlvds") and returns
 * the enum fsl_diu_monitor_port that corresponds to that string.
 *
 * For compatibility with older versions, a number ("0", "1", or "2") is also
 * supported.
 *
 * If the string is unknown, DVI is assumed.
 *
 * If the particular port is not supported by the platform, another port
 * (platform-specific) is chosen instead.
 */
static enum fsl_diu_monitor_port fsl_diu_name_to_port(const char *s)
{
	enum fsl_diu_monitor_port port = FSL_DIU_PORT_DVI;
	unsigned long val;

	if (s) {
		if (!strict_strtoul(s, 10, &val) && (val <= 2))
			port = (enum fsl_diu_monitor_port) val;
		else if (strncmp(s, "lvds", 4) == 0)
			port = FSL_DIU_PORT_LVDS;
		else if (strncmp(s, "dlvds", 5) == 0)
			port = FSL_DIU_PORT_DLVDS;
	}

	return diu_ops.valid_monitor_port(port);
}

/**
 * fsl_diu_alloc - allocate memory for the DIU
 * @size: number of bytes to allocate
 * @param: returned physical address of memory
 *
 * This function allocates a physically-contiguous block of memory.
 */
static void *fsl_diu_alloc(size_t size, phys_addr_t *phys)
{
	void *virt;

	virt = alloc_pages_exact(size, GFP_DMA | __GFP_ZERO);
	if (virt)
		*phys = virt_to_phys(virt);

	return virt;
}

/**
 * fsl_diu_free - release DIU memory
 * @virt: pointer returned by fsl_diu_alloc()
 * @size: number of bytes allocated by fsl_diu_alloc()
 *
 * This function releases memory allocated by fsl_diu_alloc().
 */
static void fsl_diu_free(void *virt, size_t size)
{
	if (virt && size)
		free_pages_exact(virt, size);
}

/*
 * Workaround for failed writing desc register of planes.
 * Needed with MPC5121 DIU rev 2.0 silicon.
 */
void wr_reg_wa(u32 *reg, u32 val)
{
	do {
		out_be32(reg, val);
	} while (in_be32(reg) != val);
}

static void fsl_diu_enable_panel(struct fb_info *info)
{
	struct mfb_info *pmfbi, *cmfbi, *mfbi = info->par;
	struct diu *hw = dr.diu_reg;
	struct diu_ad *ad = mfbi->ad;
	struct fsl_diu_data *machine_data = mfbi->parent;

	switch (mfbi->index) {
	case PLANE0:
		if (hw->desc[0] != ad->paddr)
			wr_reg_wa(&hw->desc[0], ad->paddr);
		break;
	case PLANE1_AOI0:
		cmfbi = machine_data->fsl_diu_info[2]->par;
		if (hw->desc[1] != ad->paddr) {	/* AOI0 closed */
			if (cmfbi->count > 0)	/* AOI1 open */
				ad->next_ad =
					cpu_to_le32(cmfbi->ad->paddr);
			else
				ad->next_ad = 0;
			wr_reg_wa(&hw->desc[1], ad->paddr);
		}
		break;
	case PLANE2_AOI0:
		cmfbi = machine_data->fsl_diu_info[4]->par;
		if (hw->desc[2] != ad->paddr) {	/* AOI0 closed */
			if (cmfbi->count > 0)	/* AOI1 open */
				ad->next_ad =
					cpu_to_le32(cmfbi->ad->paddr);
			else
				ad->next_ad = 0;
			wr_reg_wa(&hw->desc[2], ad->paddr);
		}
		break;
	case PLANE1_AOI1:
		pmfbi = machine_data->fsl_diu_info[1]->par;
		ad->next_ad = 0;
		if (hw->desc[1] == machine_data->dummy_ad->paddr)
			wr_reg_wa(&hw->desc[1], ad->paddr);
		else					/* AOI0 open */
			pmfbi->ad->next_ad = cpu_to_le32(ad->paddr);
		break;
	case PLANE2_AOI1:
		pmfbi = machine_data->fsl_diu_info[3]->par;
		ad->next_ad = 0;
		if (hw->desc[2] == machine_data->dummy_ad->paddr)
			wr_reg_wa(&hw->desc[2], ad->paddr);
		else				/* AOI0 was open */
			pmfbi->ad->next_ad = cpu_to_le32(ad->paddr);
		break;
	}
}

static void fsl_diu_disable_panel(struct fb_info *info)
{
	struct mfb_info *pmfbi, *cmfbi, *mfbi = info->par;
	struct diu *hw = dr.diu_reg;
	struct diu_ad *ad = mfbi->ad;
	struct fsl_diu_data *machine_data = mfbi->parent;

	switch (mfbi->index) {
	case PLANE0:
		if (hw->desc[0] != machine_data->dummy_ad->paddr)
			wr_reg_wa(&hw->desc[0], machine_data->dummy_ad->paddr);
		break;
	case PLANE1_AOI0:
		cmfbi = machine_data->fsl_diu_info[2]->par;
		if (cmfbi->count > 0)	/* AOI1 is open */
			wr_reg_wa(&hw->desc[1], cmfbi->ad->paddr);
					/* move AOI1 to the first */
		else			/* AOI1 was closed */
			wr_reg_wa(&hw->desc[1], machine_data->dummy_ad->paddr);
					/* close AOI 0 */
		break;
	case PLANE2_AOI0:
		cmfbi = machine_data->fsl_diu_info[4]->par;
		if (cmfbi->count > 0)	/* AOI1 is open */
			wr_reg_wa(&hw->desc[2], cmfbi->ad->paddr);
					/* move AOI1 to the first */
		else			/* AOI1 was closed */
			wr_reg_wa(&hw->desc[2], machine_data->dummy_ad->paddr);
					/* close AOI 0 */
		break;
	case PLANE1_AOI1:
		pmfbi = machine_data->fsl_diu_info[1]->par;
		if (hw->desc[1] != ad->paddr) {
				/* AOI1 is not the first in the chain */
			if (pmfbi->count > 0)
					/* AOI0 is open, must be the first */
				pmfbi->ad->next_ad = 0;
		} else			/* AOI1 is the first in the chain */
			wr_reg_wa(&hw->desc[1], machine_data->dummy_ad->paddr);
					/* close AOI 1 */
		break;
	case PLANE2_AOI1:
		pmfbi = machine_data->fsl_diu_info[3]->par;
		if (hw->desc[2] != ad->paddr) {
				/* AOI1 is not the first in the chain */
			if (pmfbi->count > 0)
				/* AOI0 is open, must be the first */
				pmfbi->ad->next_ad = 0;
		} else		/* AOI1 is the first in the chain */
			wr_reg_wa(&hw->desc[2], machine_data->dummy_ad->paddr);
				/* close AOI 1 */
		break;
	}
}

static void enable_lcdc(struct fb_info *info)
{
	struct diu *hw = dr.diu_reg;
	struct mfb_info *mfbi = info->par;
	struct fsl_diu_data *machine_data = mfbi->parent;

	if (!machine_data->fb_enabled) {
		out_be32(&hw->diu_mode, dr.mode);
		machine_data->fb_enabled++;
	}
}

static void disable_lcdc(struct fb_info *info)
{
	struct diu *hw = dr.diu_reg;
	struct mfb_info *mfbi = info->par;
	struct fsl_diu_data *machine_data = mfbi->parent;

	if (machine_data->fb_enabled) {
		out_be32(&hw->diu_mode, 0);
		machine_data->fb_enabled = 0;
	}
}

static void adjust_aoi_size_position(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	struct mfb_info *lower_aoi_mfbi, *upper_aoi_mfbi, *mfbi = info->par;
	struct fsl_diu_data *machine_data = mfbi->parent;
	int available_height, upper_aoi_bottom;
	enum mfb_index index = mfbi->index;
	int lower_aoi_is_open, upper_aoi_is_open;
	__u32 base_plane_width, base_plane_height, upper_aoi_height;

	base_plane_width = machine_data->fsl_diu_info[0]->var.xres;
	base_plane_height = machine_data->fsl_diu_info[0]->var.yres;

	if (mfbi->x_aoi_d < 0)
		mfbi->x_aoi_d = 0;
	if (mfbi->y_aoi_d < 0)
		mfbi->y_aoi_d = 0;
	switch (index) {
	case PLANE0:
		if (mfbi->x_aoi_d != 0)
			mfbi->x_aoi_d = 0;
		if (mfbi->y_aoi_d != 0)
			mfbi->y_aoi_d = 0;
		break;
	case PLANE1_AOI0:
	case PLANE2_AOI0:
		lower_aoi_mfbi = machine_data->fsl_diu_info[index+1]->par;
		lower_aoi_is_open = lower_aoi_mfbi->count > 0 ? 1 : 0;
		if (var->xres > base_plane_width)
			var->xres = base_plane_width;
		if ((mfbi->x_aoi_d + var->xres) > base_plane_width)
			mfbi->x_aoi_d = base_plane_width - var->xres;

		if (lower_aoi_is_open)
			available_height = lower_aoi_mfbi->y_aoi_d;
		else
			available_height = base_plane_height;
		if (var->yres > available_height)
			var->yres = available_height;
		if ((mfbi->y_aoi_d + var->yres) > available_height)
			mfbi->y_aoi_d = available_height - var->yres;
		break;
	case PLANE1_AOI1:
	case PLANE2_AOI1:
		upper_aoi_mfbi = machine_data->fsl_diu_info[index-1]->par;
		upper_aoi_height =
				machine_data->fsl_diu_info[index-1]->var.yres;
		upper_aoi_bottom = upper_aoi_mfbi->y_aoi_d + upper_aoi_height;
		upper_aoi_is_open = upper_aoi_mfbi->count > 0 ? 1 : 0;
		if (var->xres > base_plane_width)
			var->xres = base_plane_width;
		if ((mfbi->x_aoi_d + var->xres) > base_plane_width)
			mfbi->x_aoi_d = base_plane_width - var->xres;
		if (mfbi->y_aoi_d < 0)
			mfbi->y_aoi_d = 0;
		if (upper_aoi_is_open) {
			if (mfbi->y_aoi_d < upper_aoi_bottom)
				mfbi->y_aoi_d = upper_aoi_bottom;
			available_height = base_plane_height
						- upper_aoi_bottom;
		} else
			available_height = base_plane_height;
		if (var->yres > available_height)
			var->yres = available_height;
		if ((mfbi->y_aoi_d + var->yres) > base_plane_height)
			mfbi->y_aoi_d = base_plane_height - var->yres;
		break;
	}
}
/*
 * Checks to see if the hardware supports the state requested by var passed
 * in. This function does not alter the hardware state! If the var passed in
 * is slightly off by what the hardware can support then we alter the var
 * PASSED in to what we can do. If the hardware doesn't support mode change
 * a -EINVAL will be returned by the upper layers.
 */
static int fsl_diu_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;

	if (var->xoffset < 0)
		var->xoffset = 0;

	if (var->yoffset < 0)
		var->yoffset = 0;

	if (var->xoffset + info->var.xres > info->var.xres_virtual)
		var->xoffset = info->var.xres_virtual - info->var.xres;

	if (var->yoffset + info->var.yres > info->var.yres_virtual)
		var->yoffset = info->var.yres_virtual - info->var.yres;

	if ((var->bits_per_pixel != 32) && (var->bits_per_pixel != 24) &&
	    (var->bits_per_pixel != 16))
		var->bits_per_pixel = default_bpp;

	switch (var->bits_per_pixel) {
	case 16:
		var->red.length = 5;
		var->red.offset = 11;
		var->red.msb_right = 0;

		var->green.length = 6;
		var->green.offset = 5;
		var->green.msb_right = 0;

		var->blue.length = 5;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 24:
		var->red.length = 8;
		var->red.offset = 0;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 16;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 32:
		var->red.length = 8;
		var->red.offset = 16;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 8;
		var->transp.offset = 24;
		var->transp.msb_right = 0;

		break;
	}

	var->height = -1;
	var->width = -1;
	var->grayscale = 0;

	/* Copy nonstd field to/from sync for fbset usage */
	var->sync |= var->nonstd;
	var->nonstd |= var->sync;

	adjust_aoi_size_position(var, info);
	return 0;
}

static void set_fix(struct fb_info *info)
{
	struct fb_fix_screeninfo *fix = &info->fix;
	struct fb_var_screeninfo *var = &info->var;
	struct mfb_info *mfbi = info->par;

	strncpy(fix->id, mfbi->id, sizeof(fix->id));
	fix->line_length = var->xres_virtual * var->bits_per_pixel / 8;
	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->accel = FB_ACCEL_NONE;
	fix->visual = FB_VISUAL_TRUECOLOR;
	fix->xpanstep = 1;
	fix->ypanstep = 1;
}

static void update_lcdc(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &info->var;
	struct mfb_info *mfbi = info->par;
	struct fsl_diu_data *machine_data = mfbi->parent;
	struct diu *hw;
	int i, j;
	char __iomem *cursor_base, *gamma_table_base;

	u32 temp;

	hw = dr.diu_reg;

	diu_ops.set_monitor_port(machine_data->monitor_port);
	gamma_table_base = pool.gamma.vaddr;
	cursor_base = pool.cursor.vaddr;
	/* Prep for DIU init  - gamma table, cursor table */

	for (i = 0; i <= 2; i++)
		for (j = 0; j <= 255; j++)
			*gamma_table_base++ = j;

	diu_ops.set_gamma_table(machine_data->monitor_port, pool.gamma.vaddr);

	disable_lcdc(info);

	/* Program DIU registers */

	out_be32(&hw->gamma, pool.gamma.paddr);
	out_be32(&hw->cursor, pool.cursor.paddr);

	out_be32(&hw->bgnd, 0x007F7F7F); 	/* BGND */
	out_be32(&hw->bgnd_wb, 0); 		/* BGND_WB */
	out_be32(&hw->disp_size, (var->yres << 16 | var->xres));
						/* DISP SIZE */
	out_be32(&hw->wb_size, 0); /* WB SIZE */
	out_be32(&hw->wb_mem_addr, 0); /* WB MEM ADDR */

	/* Horizontal and vertical configuration register */
	temp = var->left_margin << 22 | /* BP_H */
	       var->hsync_len << 11 |   /* PW_H */
	       var->right_margin;       /* FP_H */

	out_be32(&hw->hsyn_para, temp);

	temp = var->upper_margin << 22 | /* BP_V */
	       var->vsync_len << 11 |    /* PW_V  */
	       var->lower_margin;        /* FP_V  */

	out_be32(&hw->vsyn_para, temp);

	diu_ops.set_pixel_clock(var->pixclock);

	out_be32(&hw->syn_pol, 0);	/* SYNC SIGNALS POLARITY */
	out_be32(&hw->thresholds, 0x00037800); /* The Thresholds */
	out_be32(&hw->int_status, 0);	/* INTERRUPT STATUS */
	out_be32(&hw->plut, 0x01F5F666);

	/* Enable the DIU */
	enable_lcdc(info);
}

static int map_video_memory(struct fb_info *info)
{
	phys_addr_t phys;
	u32 smem_len = info->fix.line_length * info->var.yres_virtual;

	info->screen_base = fsl_diu_alloc(smem_len, &phys);
	if (info->screen_base == NULL) {
		dev_err(info->dev, "unable to allocate fb memory\n");
		return -ENOMEM;
	}
	mutex_lock(&info->mm_lock);
	info->fix.smem_start = (unsigned long) phys;
	info->fix.smem_len = smem_len;
	mutex_unlock(&info->mm_lock);
	info->screen_size = info->fix.smem_len;

	return 0;
}

static void unmap_video_memory(struct fb_info *info)
{
	fsl_diu_free(info->screen_base, info->fix.smem_len);
	mutex_lock(&info->mm_lock);
	info->screen_base = NULL;
	info->fix.smem_start = 0;
	info->fix.smem_len = 0;
	mutex_unlock(&info->mm_lock);
}

/*
 * Using the fb_var_screeninfo in fb_info we set the aoi of this
 * particular framebuffer. It is a light version of fsl_diu_set_par.
 */
static int fsl_diu_set_aoi(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &info->var;
	struct mfb_info *mfbi = info->par;
	struct diu_ad *ad = mfbi->ad;

	/* AOI should not be greater than display size */
	ad->offset_xyi = cpu_to_le32((var->yoffset << 16) | var->xoffset);
	ad->offset_xyd = cpu_to_le32((mfbi->y_aoi_d << 16) | mfbi->x_aoi_d);
	return 0;
}

/*
 * Using the fb_var_screeninfo in fb_info we set the resolution of this
 * particular framebuffer. This function alters the fb_fix_screeninfo stored
 * in fb_info. It does not alter var in fb_info since we are using that
 * data. This means we depend on the data in var inside fb_info to be
 * supported by the hardware. fsl_diu_check_var is always called before
 * fsl_diu_set_par to ensure this.
 */
static int fsl_diu_set_par(struct fb_info *info)
{
	unsigned long len;
	struct fb_var_screeninfo *var = &info->var;
	struct mfb_info *mfbi = info->par;
	struct fsl_diu_data *machine_data = mfbi->parent;
	struct diu_ad *ad = mfbi->ad;
	struct diu *hw;

	hw = dr.diu_reg;

	set_fix(info);
	mfbi->cursor_reset = 1;

	len = info->var.yres_virtual * info->fix.line_length;
	/* Alloc & dealloc each time resolution/bpp change */
	if (len != info->fix.smem_len) {
		if (info->fix.smem_start)
			unmap_video_memory(info);

		/* Memory allocation for framebuffer */
		if (map_video_memory(info)) {
			dev_err(info->dev, "unable to allocate fb memory 1\n");
			return -ENOMEM;
		}
	}

	ad->pix_fmt = diu_ops.get_pixel_format(machine_data->monitor_port,
					       var->bits_per_pixel);
	ad->addr    = cpu_to_le32(info->fix.smem_start);
	ad->src_size_g_alpha = cpu_to_le32((var->yres_virtual << 12) |
				var->xres_virtual) | mfbi->g_alpha;
	/* AOI should not be greater than display size */
	ad->aoi_size 	= cpu_to_le32((var->yres << 16) | var->xres);
	ad->offset_xyi = cpu_to_le32((var->yoffset << 16) | var->xoffset);
	ad->offset_xyd = cpu_to_le32((mfbi->y_aoi_d << 16) | mfbi->x_aoi_d);

	/* Disable chroma keying function */
	ad->ckmax_r = 0;
	ad->ckmax_g = 0;
	ad->ckmax_b = 0;

	ad->ckmin_r = 255;
	ad->ckmin_g = 255;
	ad->ckmin_b = 255;

	if (mfbi->index == PLANE0)
		update_lcdc(info);
	return 0;
}

static inline __u32 CNVT_TOHW(__u32 val, __u32 width)
{
	return ((val << width) + 0x7FFF - val) >> 16;
}

/*
 * Set a single color register. The values supplied have a 16 bit magnitude
 * which needs to be scaled in this function for the hardware. Things to take
 * into consideration are how many color registers, if any, are supported with
 * the current color visual. With truecolor mode no color palettes are
 * supported. Here a pseudo palette is created which we store the value in
 * pseudo_palette in struct fb_info. For pseudocolor mode we have a limited
 * color palette.
 */
static int fsl_diu_setcolreg(unsigned int regno, unsigned int red,
			     unsigned int green, unsigned int blue,
			     unsigned int transp, struct fb_info *info)
{
	int ret = 1;

	/*
	 * If greyscale is true, then we convert the RGB value
	 * to greyscale no matter what visual we are using.
	 */
	if (info->var.grayscale)
		red = green = blue = (19595 * red + 38470 * green +
				      7471 * blue) >> 16;
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
		/*
		 * 16-bit True Colour.  We encode the RGB value
		 * according to the RGB bitfield information.
		 */
		if (regno < 16) {
			u32 *pal = info->pseudo_palette;
			u32 v;

			red = CNVT_TOHW(red, info->var.red.length);
			green = CNVT_TOHW(green, info->var.green.length);
			blue = CNVT_TOHW(blue, info->var.blue.length);
			transp = CNVT_TOHW(transp, info->var.transp.length);

			v = (red << info->var.red.offset) |
			    (green << info->var.green.offset) |
			    (blue << info->var.blue.offset) |
			    (transp << info->var.transp.offset);

			pal[regno] = v;
			ret = 0;
		}
		break;
	}

	return ret;
}

/*
 * Pan (or wrap, depending on the `vmode' field) the display using the
 * 'xoffset' and 'yoffset' fields of the 'var' structure. If the values
 * don't fit, return -EINVAL.
 */
static int fsl_diu_pan_display(struct fb_var_screeninfo *var,
			     struct fb_info *info)
{
	if ((info->var.xoffset == var->xoffset) &&
	    (info->var.yoffset == var->yoffset))
		return 0;	/* No change, do nothing */

	if (var->xoffset < 0 || var->yoffset < 0
	    || var->xoffset + info->var.xres > info->var.xres_virtual
	    || var->yoffset + info->var.yres > info->var.yres_virtual)
		return -EINVAL;

	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;

	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;

	fsl_diu_set_aoi(info);

	return 0;
}

static int fsl_diu_ioctl(struct fb_info *info, unsigned int cmd,
		       unsigned long arg)
{
	struct mfb_info *mfbi = info->par;
	struct diu_ad *ad = mfbi->ad;
	struct mfb_chroma_key ck;
	unsigned char global_alpha;
	struct aoi_display_offset aoi_d;
	__u32 pix_fmt;
	void __user *buf = (void __user *)arg;

	if (!arg)
		return -EINVAL;
	switch (cmd) {
	case MFB_SET_PIXFMT_OLD:
		dev_warn(info->dev,
			 "MFB_SET_PIXFMT value of 0x%08x is deprecated.\n",
			 MFB_SET_PIXFMT_OLD);
	case MFB_SET_PIXFMT:
		if (copy_from_user(&pix_fmt, buf, sizeof(pix_fmt)))
			return -EFAULT;
		ad->pix_fmt = pix_fmt;
		break;
	case MFB_GET_PIXFMT_OLD:
		dev_warn(info->dev,
			 "MFB_GET_PIXFMT value of 0x%08x is deprecated.\n",
			 MFB_GET_PIXFMT_OLD);
	case MFB_GET_PIXFMT:
		pix_fmt = ad->pix_fmt;
		if (copy_to_user(buf, &pix_fmt, sizeof(pix_fmt)))
			return -EFAULT;
		break;
	case MFB_SET_AOID:
		if (copy_from_user(&aoi_d, buf, sizeof(aoi_d)))
			return -EFAULT;
		mfbi->x_aoi_d = aoi_d.x_aoi_d;
		mfbi->y_aoi_d = aoi_d.y_aoi_d;
		fsl_diu_check_var(&info->var, info);
		fsl_diu_set_aoi(info);
		break;
	case MFB_GET_AOID:
		aoi_d.x_aoi_d = mfbi->x_aoi_d;
		aoi_d.y_aoi_d = mfbi->y_aoi_d;
		if (copy_to_user(buf, &aoi_d, sizeof(aoi_d)))
			return -EFAULT;
		break;
	case MFB_GET_ALPHA:
		global_alpha = mfbi->g_alpha;
		if (copy_to_user(buf, &global_alpha, sizeof(global_alpha)))
			return -EFAULT;
		break;
	case MFB_SET_ALPHA:
		/* set panel information */
		if (copy_from_user(&global_alpha, buf, sizeof(global_alpha)))
			return -EFAULT;
		ad->src_size_g_alpha = (ad->src_size_g_alpha & (~0xff)) |
							(global_alpha & 0xff);
		mfbi->g_alpha = global_alpha;
		break;
	case MFB_SET_CHROMA_KEY:
		/* set panel winformation */
		if (copy_from_user(&ck, buf, sizeof(ck)))
			return -EFAULT;

		if (ck.enable &&
		   (ck.red_max < ck.red_min ||
		    ck.green_max < ck.green_min ||
		    ck.blue_max < ck.blue_min))
			return -EINVAL;

		if (!ck.enable) {
			ad->ckmax_r = 0;
			ad->ckmax_g = 0;
			ad->ckmax_b = 0;
			ad->ckmin_r = 255;
			ad->ckmin_g = 255;
			ad->ckmin_b = 255;
		} else {
			ad->ckmax_r = ck.red_max;
			ad->ckmax_g = ck.green_max;
			ad->ckmax_b = ck.blue_max;
			ad->ckmin_r = ck.red_min;
			ad->ckmin_g = ck.green_min;
			ad->ckmin_b = ck.blue_min;
		}
		break;
	default:
		dev_err(info->dev, "unknown ioctl command (0x%08X)\n", cmd);
		return -ENOIOCTLCMD;
	}

	return 0;
}

/* turn on fb if count == 1
 */
static int fsl_diu_open(struct fb_info *info, int user)
{
	struct mfb_info *mfbi = info->par;
	int res = 0;

	/* free boot splash memory on first /dev/fb0 open */
	if ((mfbi->index == PLANE0) && diu_ops.release_bootmem)
		diu_ops.release_bootmem();

	spin_lock(&diu_lock);
	mfbi->count++;
	if (mfbi->count == 1) {
		fsl_diu_check_var(&info->var, info);
		res = fsl_diu_set_par(info);
		if (res < 0)
			mfbi->count--;
		else
			fsl_diu_enable_panel(info);
	}

	spin_unlock(&diu_lock);
	return res;
}

/* turn off fb if count == 0
 */
static int fsl_diu_release(struct fb_info *info, int user)
{
	struct mfb_info *mfbi = info->par;
	int res = 0;

	spin_lock(&diu_lock);
	mfbi->count--;
	if (mfbi->count == 0)
		fsl_diu_disable_panel(info);

	spin_unlock(&diu_lock);
	return res;
}

static struct fb_ops fsl_diu_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = fsl_diu_check_var,
	.fb_set_par = fsl_diu_set_par,
	.fb_setcolreg = fsl_diu_setcolreg,
	.fb_pan_display = fsl_diu_pan_display,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_ioctl = fsl_diu_ioctl,
	.fb_open = fsl_diu_open,
	.fb_release = fsl_diu_release,
};

static int init_fbinfo(struct fb_info *info)
{
	struct mfb_info *mfbi = info->par;

	info->device = NULL;
	info->var.activate = FB_ACTIVATE_NOW;
	info->fbops = &fsl_diu_ops;
	info->flags = FBINFO_FLAG_DEFAULT;
	info->pseudo_palette = &mfbi->pseudo_palette;

	/* Allocate colormap */
	fb_alloc_cmap(&info->cmap, 16, 0);
	return 0;
}

static int __devinit install_fb(struct fb_info *info)
{
	int rc;
	struct mfb_info *mfbi = info->par;
	const char *aoi_mode, *init_aoi_mode = "320x240";
	struct fb_videomode *db = fsl_diu_mode_db;
	unsigned int dbsize = ARRAY_SIZE(fsl_diu_mode_db);
	int has_default_mode = 1;

	if (init_fbinfo(info))
		return -EINVAL;

	if (mfbi->index == PLANE0) {
		if (mfbi->edid_data) {
			/* Now build modedb from EDID */
			fb_edid_to_monspecs(mfbi->edid_data, &info->monspecs);
			fb_videomode_to_modelist(info->monspecs.modedb,
						 info->monspecs.modedb_len,
						 &info->modelist);
			db = info->monspecs.modedb;
			dbsize = info->monspecs.modedb_len;
		}
		aoi_mode = fb_mode;
	} else {
		aoi_mode = init_aoi_mode;
	}
	rc = fb_find_mode(&info->var, info, aoi_mode, db, dbsize, NULL,
			  default_bpp);
	if (!rc) {
		/*
		 * For plane 0 we continue and look into
		 * driver's internal modedb.
		 */
		if ((mfbi->index == PLANE0) && mfbi->edid_data)
			has_default_mode = 0;
		else
			return -EINVAL;
	}

	if (!has_default_mode) {
		rc = fb_find_mode(&info->var, info, aoi_mode, fsl_diu_mode_db,
			ARRAY_SIZE(fsl_diu_mode_db), NULL, default_bpp);
		if (rc)
			has_default_mode = 1;
	}

	/* Still not found, use preferred mode from database if any */
	if (!has_default_mode && info->monspecs.modedb) {
		struct fb_monspecs *specs = &info->monspecs;
		struct fb_videomode *modedb = &specs->modedb[0];

		/*
		 * Get preferred timing. If not found,
		 * first mode in database will be used.
		 */
		if (specs->misc & FB_MISC_1ST_DETAIL) {
			int i;

			for (i = 0; i < specs->modedb_len; i++) {
				if (specs->modedb[i].flag & FB_MODE_IS_FIRST) {
					modedb = &specs->modedb[i];
					break;
				}
			}
		}

		info->var.bits_per_pixel = default_bpp;
		fb_videomode_to_var(&info->var, modedb);
	}

	if (fsl_diu_check_var(&info->var, info)) {
		dev_err(info->dev, "fsl_diu_check_var failed\n");
		unmap_video_memory(info);
		fb_dealloc_cmap(&info->cmap);
		return -EINVAL;
	}

	if (register_framebuffer(info) < 0) {
		dev_err(info->dev, "register_framebuffer failed\n");
		unmap_video_memory(info);
		fb_dealloc_cmap(&info->cmap);
		return -EINVAL;
	}

	mfbi->registered = 1;
	dev_info(info->dev, "%s registered successfully\n", mfbi->id);

	return 0;
}

static void uninstall_fb(struct fb_info *info)
{
	struct mfb_info *mfbi = info->par;

	if (!mfbi->registered)
		return;

	if (mfbi->index == PLANE0)
		kfree(mfbi->edid_data);

	unregister_framebuffer(info);
	unmap_video_memory(info);
	if (&info->cmap)
		fb_dealloc_cmap(&info->cmap);

	mfbi->registered = 0;
}

static irqreturn_t fsl_diu_isr(int irq, void *dev_id)
{
	struct diu *hw = dr.diu_reg;
	unsigned int status = in_be32(&hw->int_status);

	if (status) {
		/* This is the workaround for underrun */
		if (status & INT_UNDRUN) {
			out_be32(&hw->diu_mode, 0);
			udelay(1);
			out_be32(&hw->diu_mode, 1);
		}
#if defined(CONFIG_NOT_COHERENT_CACHE)
		else if (status & INT_VSYNC) {
			unsigned int i;

			for (i = 0; i < coherence_data_size;
				i += d_cache_line_size)
				__asm__ __volatile__ (
					"dcbz 0, %[input]"
				::[input]"r"(&coherence_data[i]));
		}
#endif
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

static int request_irq_local(int irq)
{
	u32 ints;
	struct diu *hw;
	int ret;

	hw = dr.diu_reg;

	/* Read to clear the status */
	in_be32(&hw->int_status);

	ret = request_irq(irq, fsl_diu_isr, 0, "fsl-diu-fb", NULL);
	if (!ret) {
		ints = INT_PARERR | INT_LS_BF_VS;
#if !defined(CONFIG_NOT_COHERENT_CACHE)
		ints |=	INT_VSYNC;
#endif

		if (dr.mode == MFB_MODE2 || dr.mode == MFB_MODE3)
			ints |= INT_VSYNC_WB;

		/* Read to clear the status */
		in_be32(&hw->int_status);
		out_be32(&hw->int_mask, ints);
	}

	return ret;
}

static void free_irq_local(int irq)
{
	struct diu *hw = dr.diu_reg;

	/* Disable all LCDC interrupt */
	out_be32(&hw->int_mask, 0x1f);

	free_irq(irq, NULL);
}

#ifdef CONFIG_PM
/*
 * Power management hooks. Note that we won't be called from IRQ context,
 * unlike the blank functions above, so we may sleep.
 */
static int fsl_diu_suspend(struct platform_device *ofdev, pm_message_t state)
{
	struct fsl_diu_data *machine_data;

	machine_data = dev_get_drvdata(&ofdev->dev);
	disable_lcdc(machine_data->fsl_diu_info[0]);

	return 0;
}

static int fsl_diu_resume(struct platform_device *ofdev)
{
	struct fsl_diu_data *machine_data;

	machine_data = dev_get_drvdata(&ofdev->dev);
	enable_lcdc(machine_data->fsl_diu_info[0]);

	return 0;
}

#else
#define fsl_diu_suspend NULL
#define fsl_diu_resume NULL
#endif				/* CONFIG_PM */

/* Align to 64-bit(8-byte), 32-byte, etc. */
static int allocate_buf(struct device *dev, struct diu_addr *buf, u32 size,
			u32 bytes_align)
{
	u32 offset;
	dma_addr_t mask;

	buf->vaddr =
		dma_alloc_coherent(dev, size + bytes_align, &buf->paddr,
				   GFP_DMA | __GFP_ZERO);
	if (!buf->vaddr)
		return -ENOMEM;

	mask = bytes_align - 1;
	offset = buf->paddr & mask;
	if (offset) {
		buf->offset = bytes_align - offset;
		buf->paddr = buf->paddr + offset;
	} else
		buf->offset = 0;

	return 0;
}

static void free_buf(struct device *dev, struct diu_addr *buf, u32 size,
		     u32 bytes_align)
{
	dma_free_coherent(dev, size + bytes_align, buf->vaddr,
			  buf->paddr - buf->offset);
}

static ssize_t store_monitor(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	enum fsl_diu_monitor_port old_monitor_port;
	struct fsl_diu_data *machine_data =
		container_of(attr, struct fsl_diu_data, dev_attr);

	old_monitor_port = machine_data->monitor_port;
	machine_data->monitor_port = fsl_diu_name_to_port(buf);

	if (old_monitor_port != machine_data->monitor_port) {
		/* All AOIs need adjust pixel format
		 * fsl_diu_set_par only change the pixsel format here
		 * unlikely to fail. */
		fsl_diu_set_par(machine_data->fsl_diu_info[0]);
		fsl_diu_set_par(machine_data->fsl_diu_info[1]);
		fsl_diu_set_par(machine_data->fsl_diu_info[2]);
		fsl_diu_set_par(machine_data->fsl_diu_info[3]);
		fsl_diu_set_par(machine_data->fsl_diu_info[4]);
	}
	return count;
}

static ssize_t show_monitor(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct fsl_diu_data *machine_data =
		container_of(attr, struct fsl_diu_data, dev_attr);

	switch (machine_data->monitor_port) {
	case FSL_DIU_PORT_DVI:
		return sprintf(buf, "DVI\n");
	case FSL_DIU_PORT_LVDS:
		return sprintf(buf, "Single-link LVDS\n");
	case FSL_DIU_PORT_DLVDS:
		return sprintf(buf, "Dual-link LVDS\n");
	}

	return 0;
}

static int __devinit fsl_diu_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct mfb_info *mfbi;
	phys_addr_t dummy_ad_addr = 0;
	int ret, i, error = 0;
	struct fsl_diu_data *machine_data;
	int diu_mode;

	machine_data = kzalloc(sizeof(struct fsl_diu_data), GFP_KERNEL);
	if (!machine_data)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++) {
		machine_data->fsl_diu_info[i] =
			framebuffer_alloc(sizeof(struct mfb_info), &pdev->dev);
		if (!machine_data->fsl_diu_info[i]) {
			dev_err(&pdev->dev, "cannot allocate memory\n");
			ret = -ENOMEM;
			goto error2;
		}
		mfbi = machine_data->fsl_diu_info[i]->par;
		memcpy(mfbi, &mfb_template[i], sizeof(struct mfb_info));
		mfbi->parent = machine_data;

		if (mfbi->index == PLANE0) {
			const u8 *prop;
			int len;

			/* Get EDID */
			prop = of_get_property(np, "edid", &len);
			if (prop && len == EDID_LENGTH)
				mfbi->edid_data = kmemdup(prop, EDID_LENGTH,
							  GFP_KERNEL);
		}
	}

	dr.diu_reg = of_iomap(np, 0);
	if (!dr.diu_reg) {
		dev_err(&pdev->dev, "cannot map DIU registers\n");
		ret = -EFAULT;
		goto error2;
	}

	diu_mode = in_be32(&dr.diu_reg->diu_mode);
	if (diu_mode != MFB_MODE1)
		out_be32(&dr.diu_reg->diu_mode, 0);	/* disable DIU */

	/* Get the IRQ of the DIU */
	machine_data->irq = irq_of_parse_and_map(np, 0);

	if (!machine_data->irq) {
		dev_err(&pdev->dev, "could not get DIU IRQ\n");
		ret = -EINVAL;
		goto error;
	}
	machine_data->monitor_port = monitor_port;

	/* Area descriptor memory pool aligns to 64-bit boundary */
	if (allocate_buf(&pdev->dev, &pool.ad,
			 sizeof(struct diu_ad) * FSL_AOI_NUM, 8))
		return -ENOMEM;

	/* Get memory for Gamma Table  - 32-byte aligned memory */
	if (allocate_buf(&pdev->dev, &pool.gamma, 768, 32)) {
		ret = -ENOMEM;
		goto error;
	}

	/* For performance, cursor bitmap buffer aligns to 32-byte boundary */
	if (allocate_buf(&pdev->dev, &pool.cursor, MAX_CURS * MAX_CURS * 2,
			 32)) {
		ret = -ENOMEM;
		goto error;
	}

	i = ARRAY_SIZE(machine_data->fsl_diu_info);
	machine_data->dummy_ad = (struct diu_ad *)
			((u32)pool.ad.vaddr + pool.ad.offset) + i;
	machine_data->dummy_ad->paddr = pool.ad.paddr +
			i * sizeof(struct diu_ad);
	machine_data->dummy_aoi_virt = fsl_diu_alloc(64, &dummy_ad_addr);
	if (!machine_data->dummy_aoi_virt) {
		ret = -ENOMEM;
		goto error;
	}
	machine_data->dummy_ad->addr = cpu_to_le32(dummy_ad_addr);
	machine_data->dummy_ad->pix_fmt = 0x88882317;
	machine_data->dummy_ad->src_size_g_alpha = cpu_to_le32((4 << 12) | 4);
	machine_data->dummy_ad->aoi_size = cpu_to_le32((4 << 16) |  2);
	machine_data->dummy_ad->offset_xyi = 0;
	machine_data->dummy_ad->offset_xyd = 0;
	machine_data->dummy_ad->next_ad = 0;

	/*
	 * Let DIU display splash screen if it was pre-initialized
	 * by the bootloader, set dummy area descriptor otherwise.
	 */
	if (diu_mode != MFB_MODE1)
		out_be32(&dr.diu_reg->desc[0], machine_data->dummy_ad->paddr);

	out_be32(&dr.diu_reg->desc[1], machine_data->dummy_ad->paddr);
	out_be32(&dr.diu_reg->desc[2], machine_data->dummy_ad->paddr);

	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++) {
		machine_data->fsl_diu_info[i]->fix.smem_start = 0;
		mfbi = machine_data->fsl_diu_info[i]->par;
		mfbi->ad = (struct diu_ad *)((u32)pool.ad.vaddr
					+ pool.ad.offset) + i;
		mfbi->ad->paddr = pool.ad.paddr + i * sizeof(struct diu_ad);
		ret = install_fb(machine_data->fsl_diu_info[i]);
		if (ret) {
			dev_err(&pdev->dev, "could not register fb %d\n", i);
			goto error;
		}
	}

	if (request_irq_local(machine_data->irq)) {
		dev_err(&pdev->dev, "could not claim irq\n");
		goto error;
	}

	sysfs_attr_init(&machine_data->dev_attr.attr);
	machine_data->dev_attr.attr.name = "monitor";
	machine_data->dev_attr.attr.mode = S_IRUGO|S_IWUSR;
	machine_data->dev_attr.show = show_monitor;
	machine_data->dev_attr.store = store_monitor;
	error = device_create_file(machine_data->fsl_diu_info[0]->dev,
				  &machine_data->dev_attr);
	if (error) {
		dev_err(&pdev->dev, "could not create sysfs file %s\n",
			machine_data->dev_attr.attr.name);
	}

	dev_set_drvdata(&pdev->dev, machine_data);
	return 0;

error:
	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++)
		uninstall_fb(machine_data->fsl_diu_info[i]);

	if (pool.ad.vaddr)
		free_buf(&pdev->dev, &pool.ad,
			 sizeof(struct diu_ad) * FSL_AOI_NUM, 8);
	if (pool.gamma.vaddr)
		free_buf(&pdev->dev, &pool.gamma, 768, 32);
	if (pool.cursor.vaddr)
		free_buf(&pdev->dev, &pool.cursor, MAX_CURS * MAX_CURS * 2,
			 32);
	if (machine_data->dummy_aoi_virt)
		fsl_diu_free(machine_data->dummy_aoi_virt, 64);
	iounmap(dr.diu_reg);

error2:
	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++)
		if (machine_data->fsl_diu_info[i])
			framebuffer_release(machine_data->fsl_diu_info[i]);
	kfree(machine_data);

	return ret;
}

static int fsl_diu_remove(struct platform_device *pdev)
{
	struct fsl_diu_data *machine_data;
	int i;

	machine_data = dev_get_drvdata(&pdev->dev);
	disable_lcdc(machine_data->fsl_diu_info[0]);
	free_irq_local(machine_data->irq);
	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++)
		uninstall_fb(machine_data->fsl_diu_info[i]);
	if (pool.ad.vaddr)
		free_buf(&pdev->dev, &pool.ad,
			 sizeof(struct diu_ad) * FSL_AOI_NUM, 8);
	if (pool.gamma.vaddr)
		free_buf(&pdev->dev, &pool.gamma, 768, 32);
	if (pool.cursor.vaddr)
		free_buf(&pdev->dev, &pool.cursor, MAX_CURS * MAX_CURS * 2, 32);
	if (machine_data->dummy_aoi_virt)
		fsl_diu_free(machine_data->dummy_aoi_virt, 64);
	iounmap(dr.diu_reg);
	for (i = 0; i < ARRAY_SIZE(machine_data->fsl_diu_info); i++)
		if (machine_data->fsl_diu_info[i])
			framebuffer_release(machine_data->fsl_diu_info[i]);
	kfree(machine_data);

	return 0;
}

#ifndef MODULE
static int __init fsl_diu_setup(char *options)
{
	char *opt;
	unsigned long val;

	if (!options || !*options)
		return 0;

	while ((opt = strsep(&options, ",")) != NULL) {
		if (!*opt)
			continue;
		if (!strncmp(opt, "monitor=", 8)) {
			monitor_port = fsl_diu_name_to_port(opt + 8);
		} else if (!strncmp(opt, "bpp=", 4)) {
			if (!strict_strtoul(opt + 4, 10, &val))
				default_bpp = val;
		} else
			fb_mode = opt;
	}

	return 0;
}
#endif

static struct of_device_id fsl_diu_match[] = {
#ifdef CONFIG_PPC_MPC512x
	{
		.compatible = "fsl,mpc5121-diu",
	},
#endif
	{
		.compatible = "fsl,diu",
	},
	{}
};
MODULE_DEVICE_TABLE(of, fsl_diu_match);

static struct platform_driver fsl_diu_driver = {
	.driver = {
		.name = "fsl-diu-fb",
		.owner = THIS_MODULE,
		.of_match_table = fsl_diu_match,
	},
	.probe  	= fsl_diu_probe,
	.remove 	= fsl_diu_remove,
	.suspend	= fsl_diu_suspend,
	.resume		= fsl_diu_resume,
};

static int __init fsl_diu_init(void)
{
#ifdef CONFIG_NOT_COHERENT_CACHE
	struct device_node *np;
	const u32 *prop;
#endif
	int ret;
#ifndef MODULE
	char *option;

	/*
	 * For kernel boot options (in 'video=xxxfb:<options>' format)
	 */
	if (fb_get_options("fslfb", &option))
		return -ENODEV;
	fsl_diu_setup(option);
#else
	monitor_port = fsl_diu_name_to_port(monitor_string);
#endif
	pr_info("Freescale Display Interface Unit (DIU) framebuffer driver\n");

#ifdef CONFIG_NOT_COHERENT_CACHE
	np = of_find_node_by_type(NULL, "cpu");
	if (!np) {
		pr_err("fsl-diu-fb: can't find 'cpu' device node\n");
		return -ENODEV;
	}

	prop = of_get_property(np, "d-cache-size", NULL);
	if (prop == NULL) {
		pr_err("fsl-diu-fb: missing 'd-cache-size' property' "
		       "in 'cpu' node\n");
		of_node_put(np);
		return -ENODEV;
	}

	/*
	 * Freescale PLRU requires 13/8 times the cache size to do a proper
	 * displacement flush
	 */
	coherence_data_size = be32_to_cpup(prop) * 13;
	coherence_data_size /= 8;

	prop = of_get_property(np, "d-cache-line-size", NULL);
	if (prop == NULL) {
		pr_err("fsl-diu-fb: missing 'd-cache-line-size' property' "
		       "in 'cpu' node\n");
		of_node_put(np);
		return -ENODEV;
	}
	d_cache_line_size = be32_to_cpup(prop);

	of_node_put(np);
	coherence_data = vmalloc(coherence_data_size);
	if (!coherence_data)
		return -ENOMEM;
#endif

	ret = platform_driver_register(&fsl_diu_driver);
	if (ret) {
		pr_err("fsl-diu-fb: failed to register platform driver\n");
#if defined(CONFIG_NOT_COHERENT_CACHE)
		vfree(coherence_data);
#endif
		iounmap(dr.diu_reg);
	}
	return ret;
}

static void __exit fsl_diu_exit(void)
{
	platform_driver_unregister(&fsl_diu_driver);
#if defined(CONFIG_NOT_COHERENT_CACHE)
	vfree(coherence_data);
#endif
}

module_init(fsl_diu_init);
module_exit(fsl_diu_exit);

MODULE_AUTHOR("York Sun <yorksun@freescale.com>");
MODULE_DESCRIPTION("Freescale DIU framebuffer driver");
MODULE_LICENSE("GPL");

module_param_named(mode, fb_mode, charp, 0);
MODULE_PARM_DESC(mode,
	"Specify resolution as \"<xres>x<yres>[-<bpp>][@<refresh>]\" ");
module_param_named(bpp, default_bpp, ulong, 0);
MODULE_PARM_DESC(bpp, "Specify bit-per-pixel if not specified in 'mode'");
module_param_named(monitor, monitor_string, charp, 0);
MODULE_PARM_DESC(monitor, "Specify the monitor port "
	"(\"dvi\", \"lvds\", or \"dlvds\") if supported by the platform");

