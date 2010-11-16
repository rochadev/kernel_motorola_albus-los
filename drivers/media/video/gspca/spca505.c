/*
 * SPCA505 chip based cameras initialization data
 *
 * V4L2 by Jean-Francis Moine <http://moinejf.free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#define MODULE_NAME "spca505"

#include "gspca.h"

MODULE_AUTHOR("Michel Xhaard <mxhaard@users.sourceforge.net>");
MODULE_DESCRIPTION("GSPCA/SPCA505 USB Camera Driver");
MODULE_LICENSE("GPL");

/* specific webcam descriptor */
struct sd {
	struct gspca_dev gspca_dev;		/* !! must be the first item */

	u8 brightness;

	u8 subtype;
#define IntelPCCameraPro 0
#define Nxultra 1
};

/* V4L2 controls supported by the driver */
static int sd_setbrightness(struct gspca_dev *gspca_dev, __s32 val);
static int sd_getbrightness(struct gspca_dev *gspca_dev, __s32 *val);

static const struct ctrl sd_ctrls[] = {
	{
	    {
		.id      = V4L2_CID_BRIGHTNESS,
		.type    = V4L2_CTRL_TYPE_INTEGER,
		.name    = "Brightness",
		.minimum = 0,
		.maximum = 255,
		.step    = 1,
#define BRIGHTNESS_DEF 127
		.default_value = BRIGHTNESS_DEF,
	    },
	    .set = sd_setbrightness,
	    .get = sd_getbrightness,
	},
};

static const struct v4l2_pix_format vga_mode[] = {
	{160, 120, V4L2_PIX_FMT_SPCA505, V4L2_FIELD_NONE,
		.bytesperline = 160,
		.sizeimage = 160 * 120 * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.priv = 4},
	{176, 144, V4L2_PIX_FMT_SPCA505, V4L2_FIELD_NONE,
		.bytesperline = 176,
		.sizeimage = 176 * 144 * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.priv = 3},
	{320, 240, V4L2_PIX_FMT_SPCA505, V4L2_FIELD_NONE,
		.bytesperline = 320,
		.sizeimage = 320 * 240 * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.priv = 2},
	{352, 288, V4L2_PIX_FMT_SPCA505, V4L2_FIELD_NONE,
		.bytesperline = 352,
		.sizeimage = 352 * 288 * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.priv = 1},
	{640, 480, V4L2_PIX_FMT_SPCA505, V4L2_FIELD_NONE,
		.bytesperline = 640,
		.sizeimage = 640 * 480 * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.priv = 0},
};

#define SPCA50X_OFFSET_DATA 10

#define SPCA50X_REG_USB 0x02	/* spca505 501 */

#define SPCA50X_USB_CTRL 0x00	/* spca505 */
#define SPCA50X_CUSB_ENABLE 0x01 /* spca505 */

#define SPCA50X_REG_GLOBAL 0x03	/* spca505 */
#define SPCA50X_GMISC0_IDSEL 0x01 /* Global control device ID select spca505 */
#define SPCA50X_GLOBAL_MISC0 0x00 /* Global control miscellaneous 0 spca505 */

#define SPCA50X_GLOBAL_MISC1 0x01 /* 505 */
#define SPCA50X_GLOBAL_MISC3 0x03 /* 505 */
#define SPCA50X_GMISC3_SAA7113RST 0x20	/* Not sure about this one spca505 */

/* Image format and compression control */
#define SPCA50X_REG_COMPRESS 0x04

/*
 * Data to initialize a SPCA505. Common to the CCD and external modes
 */
static const u8 spca505_init_data[][3] = {
	/* bmRequest,value,index */
	{SPCA50X_REG_GLOBAL, SPCA50X_GMISC3_SAA7113RST, SPCA50X_GLOBAL_MISC3},
	/* Sensor reset */
	{SPCA50X_REG_GLOBAL, 0x00, SPCA50X_GLOBAL_MISC3},
	{SPCA50X_REG_GLOBAL, 0x00, SPCA50X_GLOBAL_MISC1},
	/* Block USB reset */
	{SPCA50X_REG_GLOBAL, SPCA50X_GMISC0_IDSEL, SPCA50X_GLOBAL_MISC0},

	{0x05, 0x01, 0x10},
					/* Maybe power down some stuff */
	{0x05, 0x0f, 0x11},

	/* Setup internal CCD  ? */
	{0x06, 0x10, 0x08},
	{0x06, 0x00, 0x09},
	{0x06, 0x00, 0x0a},
	{0x06, 0x00, 0x0b},
	{0x06, 0x10, 0x0c},
	{0x06, 0x00, 0x0d},
	{0x06, 0x00, 0x0e},
	{0x06, 0x00, 0x0f},
	{0x06, 0x10, 0x10},
	{0x06, 0x02, 0x11},
	{0x06, 0x00, 0x12},
	{0x06, 0x04, 0x13},
	{0x06, 0x02, 0x14},
	{0x06, 0x8a, 0x51},
	{0x06, 0x40, 0x52},
	{0x06, 0xb6, 0x53},
	{0x06, 0x3d, 0x54},
	{}
};

/*
 * Data to initialize the camera using the internal CCD
 */
static const u8 spca505_open_data_ccd[][3] = {
	/* bmRequest,value,index */
	/* Internal CCD data set */
	{0x03, 0x04, 0x01},
	/* This could be a reset */
	{0x03, 0x00, 0x01},

	/* Setup compression and image registers. 0x6 and 0x7 seem to be
	   related to H&V hold, and are resolution mode specific */
		{0x04, 0x10, 0x01},
		/* DIFF(0x50), was (0x10) */
	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},
	{0x04, 0x20, 0x06},
	{0x04, 0x20, 0x07},

	{0x08, 0x0a, 0x00},
	/* DIFF (0x4a), was (0xa) */

	{0x05, 0x00, 0x10},
	{0x05, 0x00, 0x11},
	{0x05, 0x00, 0x00},
	/* DIFF not written */
	{0x05, 0x00, 0x01},
	/* DIFF not written */
	{0x05, 0x00, 0x02},
	/* DIFF not written */
	{0x05, 0x00, 0x03},
	/* DIFF not written */
	{0x05, 0x00, 0x04},
	/* DIFF not written */
		{0x05, 0x80, 0x05},
		/* DIFF not written */
		{0x05, 0xe0, 0x06},
		/* DIFF not written */
		{0x05, 0x20, 0x07},
		/* DIFF not written */
		{0x05, 0xa0, 0x08},
		/* DIFF not written */
		{0x05, 0x0, 0x12},
		/* DIFF not written */
	{0x05, 0x02, 0x0f},
	/* DIFF not written */
		{0x05, 0x10, 0x46},
		/* DIFF not written */
		{0x05, 0x8, 0x4a},
		/* DIFF not written */

	{0x03, 0x08, 0x03},
	/* DIFF (0x3,0x28,0x3) */
	{0x03, 0x08, 0x01},
	{0x03, 0x0c, 0x03},
	/* DIFF not written */
		{0x03, 0x21, 0x00},
		/* DIFF (0x39) */

/* Extra block copied from init to hopefully ensure CCD is in a sane state */
	{0x06, 0x10, 0x08},
	{0x06, 0x00, 0x09},
	{0x06, 0x00, 0x0a},
	{0x06, 0x00, 0x0b},
	{0x06, 0x10, 0x0c},
	{0x06, 0x00, 0x0d},
	{0x06, 0x00, 0x0e},
	{0x06, 0x00, 0x0f},
	{0x06, 0x10, 0x10},
	{0x06, 0x02, 0x11},
	{0x06, 0x00, 0x12},
	{0x06, 0x04, 0x13},
	{0x06, 0x02, 0x14},
	{0x06, 0x8a, 0x51},
	{0x06, 0x40, 0x52},
	{0x06, 0xb6, 0x53},
	{0x06, 0x3d, 0x54},
	/* End of extra block */

		{0x06, 0x3f, 0x1},
		/* Block skipped */
	{0x06, 0x10, 0x02},
	{0x06, 0x64, 0x07},
	{0x06, 0x10, 0x08},
	{0x06, 0x00, 0x09},
	{0x06, 0x00, 0x0a},
	{0x06, 0x00, 0x0b},
	{0x06, 0x10, 0x0c},
	{0x06, 0x00, 0x0d},
	{0x06, 0x00, 0x0e},
	{0x06, 0x00, 0x0f},
	{0x06, 0x10, 0x10},
	{0x06, 0x02, 0x11},
	{0x06, 0x00, 0x12},
	{0x06, 0x04, 0x13},
	{0x06, 0x02, 0x14},
	{0x06, 0x8a, 0x51},
	{0x06, 0x40, 0x52},
	{0x06, 0xb6, 0x53},
	{0x06, 0x3d, 0x54},
	{0x06, 0x60, 0x57},
	{0x06, 0x20, 0x58},
	{0x06, 0x15, 0x59},
	{0x06, 0x05, 0x5a},

	{0x05, 0x01, 0xc0},
	{0x05, 0x10, 0xcb},
		{0x05, 0x80, 0xc1},
		/* */
		{0x05, 0x0, 0xc2},
		/* 4 was 0 */
	{0x05, 0x00, 0xca},
		{0x05, 0x80, 0xc1},
		/*  */
	{0x05, 0x04, 0xc2},
	{0x05, 0x00, 0xca},
		{0x05, 0x0, 0xc1},
		/*  */
	{0x05, 0x00, 0xc2},
	{0x05, 0x00, 0xca},
		{0x05, 0x40, 0xc1},
		/* */
	{0x05, 0x17, 0xc2},
	{0x05, 0x00, 0xca},
		{0x05, 0x80, 0xc1},
		/* */
	{0x05, 0x06, 0xc2},
	{0x05, 0x00, 0xca},
		{0x05, 0x80, 0xc1},
		/* */
	{0x05, 0x04, 0xc2},
	{0x05, 0x00, 0xca},

	{0x03, 0x4c, 0x3},
	{0x03, 0x18, 0x1},

	{0x06, 0x70, 0x51},
	{0x06, 0xbe, 0x53},
	{0x06, 0x71, 0x57},
	{0x06, 0x20, 0x58},
	{0x06, 0x05, 0x59},
	{0x06, 0x15, 0x5a},

	{0x04, 0x00, 0x08},
	/* Compress = OFF (0x1 to turn on) */
	{0x04, 0x12, 0x09},
	{0x04, 0x21, 0x0a},
	{0x04, 0x10, 0x0b},
	{0x04, 0x21, 0x0c},
	{0x04, 0x05, 0x00},
	/* was 5 (Image Type ? ) */
	{0x04, 0x00, 0x01},

	{0x06, 0x3f, 0x01},

	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},
	{0x04, 0x40, 0x06},
	{0x04, 0x40, 0x07},

	{0x06, 0x1c, 0x17},
	{0x06, 0xe2, 0x19},
	{0x06, 0x1c, 0x1b},
	{0x06, 0xe2, 0x1d},
	{0x06, 0xaa, 0x1f},
	{0x06, 0x70, 0x20},

	{0x05, 0x01, 0x10},
	{0x05, 0x00, 0x11},
	{0x05, 0x01, 0x00},
	{0x05, 0x05, 0x01},
		{0x05, 0x00, 0xc1},
		/* */
	{0x05, 0x00, 0xc2},
	{0x05, 0x00, 0xca},

	{0x06, 0x70, 0x51},
	{0x06, 0xbe, 0x53},
	{}
};

/*
 * Made by Tomasz Zablocki (skalamandra@poczta.onet.pl)
 * SPCA505b chip based cameras initialization data
 */
/* jfm */
#define initial_brightness 0x7f	/* 0x0(white)-0xff(black) */
/* #define initial_brightness 0x0	//0x0(white)-0xff(black) */
/*
 * Data to initialize a SPCA505. Common to the CCD and external modes
 */
static const u8 spca505b_init_data[][3] = {
/* start */
	{0x02, 0x00, 0x00},		/* init */
	{0x02, 0x00, 0x01},
	{0x02, 0x00, 0x02},
	{0x02, 0x00, 0x03},
	{0x02, 0x00, 0x04},
	{0x02, 0x00, 0x05},
	{0x02, 0x00, 0x06},
	{0x02, 0x00, 0x07},
	{0x02, 0x00, 0x08},
	{0x02, 0x00, 0x09},
	{0x03, 0x00, 0x00},
	{0x03, 0x00, 0x01},
	{0x03, 0x00, 0x02},
	{0x03, 0x00, 0x03},
	{0x03, 0x00, 0x04},
	{0x03, 0x00, 0x05},
	{0x03, 0x00, 0x06},
	{0x04, 0x00, 0x00},
	{0x04, 0x00, 0x02},
	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},
	{0x04, 0x00, 0x06},
	{0x04, 0x00, 0x07},
	{0x04, 0x00, 0x08},
	{0x04, 0x00, 0x09},
	{0x04, 0x00, 0x0a},
	{0x04, 0x00, 0x0b},
	{0x04, 0x00, 0x0c},
	{0x07, 0x00, 0x00},
	{0x07, 0x00, 0x03},
	{0x08, 0x00, 0x00},
	{0x08, 0x00, 0x01},
	{0x08, 0x00, 0x02},
	{0x06, 0x18, 0x08},
	{0x06, 0xfc, 0x09},
	{0x06, 0xfc, 0x0a},
	{0x06, 0xfc, 0x0b},
	{0x06, 0x18, 0x0c},
	{0x06, 0xfc, 0x0d},
	{0x06, 0xfc, 0x0e},
	{0x06, 0xfc, 0x0f},
	{0x06, 0x18, 0x10},
	{0x06, 0xfe, 0x12},
	{0x06, 0x00, 0x11},
	{0x06, 0x00, 0x14},
	{0x06, 0x00, 0x13},
	{0x06, 0x28, 0x51},
	{0x06, 0xff, 0x53},
	{0x02, 0x00, 0x08},

	{0x03, 0x00, 0x03},
	{0x03, 0x10, 0x03},
	{}
};

/*
 * Data to initialize the camera using the internal CCD
 */
static const u8 spca505b_open_data_ccd[][3] = {

/* {0x02,0x00,0x00}, */
	{0x03, 0x04, 0x01},		/* rst */
	{0x03, 0x00, 0x01},
	{0x03, 0x00, 0x00},
	{0x03, 0x21, 0x00},
	{0x03, 0x00, 0x04},
	{0x03, 0x00, 0x03},
	{0x03, 0x18, 0x03},
	{0x03, 0x08, 0x01},
	{0x03, 0x1c, 0x03},
	{0x03, 0x5c, 0x03},
	{0x03, 0x5c, 0x03},
	{0x03, 0x18, 0x01},

/* same as 505 */
	{0x04, 0x10, 0x01},
	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},
	{0x04, 0x20, 0x06},
	{0x04, 0x20, 0x07},

	{0x08, 0x0a, 0x00},

	{0x05, 0x00, 0x10},
	{0x05, 0x00, 0x11},
	{0x05, 0x00, 0x12},
	{0x05, 0x6f, 0x00},
	{0x05, initial_brightness >> 6, 0x00},
	{0x05, (initial_brightness << 2) & 0xff, 0x01},
	{0x05, 0x00, 0x02},
	{0x05, 0x01, 0x03},
	{0x05, 0x00, 0x04},
	{0x05, 0x03, 0x05},
	{0x05, 0xe0, 0x06},
	{0x05, 0x20, 0x07},
	{0x05, 0xa0, 0x08},
	{0x05, 0x00, 0x12},
	{0x05, 0x02, 0x0f},
	{0x05, 0x80, 0x14},		/* max exposure off (0=on) */
	{0x05, 0x01, 0xb0},
	{0x05, 0x01, 0xbf},
	{0x03, 0x02, 0x06},
	{0x05, 0x10, 0x46},
	{0x05, 0x08, 0x4a},

	{0x06, 0x00, 0x01},
	{0x06, 0x10, 0x02},
	{0x06, 0x64, 0x07},
	{0x06, 0x18, 0x08},
	{0x06, 0xfc, 0x09},
	{0x06, 0xfc, 0x0a},
	{0x06, 0xfc, 0x0b},
	{0x04, 0x00, 0x01},
	{0x06, 0x18, 0x0c},
	{0x06, 0xfc, 0x0d},
	{0x06, 0xfc, 0x0e},
	{0x06, 0xfc, 0x0f},
	{0x06, 0x11, 0x10},		/* contrast */
	{0x06, 0x00, 0x11},
	{0x06, 0xfe, 0x12},
	{0x06, 0x00, 0x13},
	{0x06, 0x00, 0x14},
	{0x06, 0x9d, 0x51},
	{0x06, 0x40, 0x52},
	{0x06, 0x7c, 0x53},
	{0x06, 0x40, 0x54},
	{0x06, 0x02, 0x57},
	{0x06, 0x03, 0x58},
	{0x06, 0x15, 0x59},
	{0x06, 0x05, 0x5a},
	{0x06, 0x03, 0x56},
	{0x06, 0x02, 0x3f},
	{0x06, 0x00, 0x40},
	{0x06, 0x39, 0x41},
	{0x06, 0x69, 0x42},
	{0x06, 0x87, 0x43},
	{0x06, 0x9e, 0x44},
	{0x06, 0xb1, 0x45},
	{0x06, 0xbf, 0x46},
	{0x06, 0xcc, 0x47},
	{0x06, 0xd5, 0x48},
	{0x06, 0xdd, 0x49},
	{0x06, 0xe3, 0x4a},
	{0x06, 0xe8, 0x4b},
	{0x06, 0xed, 0x4c},
	{0x06, 0xf2, 0x4d},
	{0x06, 0xf7, 0x4e},
	{0x06, 0xfc, 0x4f},
	{0x06, 0xff, 0x50},

	{0x05, 0x01, 0xc0},
	{0x05, 0x10, 0xcb},
	{0x05, 0x40, 0xc1},
	{0x05, 0x04, 0xc2},
	{0x05, 0x00, 0xca},
	{0x05, 0x40, 0xc1},
	{0x05, 0x09, 0xc2},
	{0x05, 0x00, 0xca},
	{0x05, 0xc0, 0xc1},
	{0x05, 0x09, 0xc2},
	{0x05, 0x00, 0xca},
	{0x05, 0x40, 0xc1},
	{0x05, 0x59, 0xc2},
	{0x05, 0x00, 0xca},
	{0x04, 0x00, 0x01},
	{0x05, 0x80, 0xc1},
	{0x05, 0xec, 0xc2},
	{0x05, 0x0, 0xca},

	{0x06, 0x02, 0x57},
	{0x06, 0x01, 0x58},
	{0x06, 0x15, 0x59},
	{0x06, 0x0a, 0x5a},
	{0x06, 0x01, 0x57},
	{0x06, 0x8a, 0x03},
	{0x06, 0x0a, 0x6c},
	{0x06, 0x30, 0x01},
	{0x06, 0x20, 0x02},
	{0x06, 0x00, 0x03},

	{0x05, 0x8c, 0x25},

	{0x06, 0x4d, 0x51},		/* maybe saturation (4d) */
	{0x06, 0x84, 0x53},		/* making green (84) */
	{0x06, 0x00, 0x57},		/* sharpness (1) */
	{0x06, 0x18, 0x08},
	{0x06, 0xfc, 0x09},
	{0x06, 0xfc, 0x0a},
	{0x06, 0xfc, 0x0b},
	{0x06, 0x18, 0x0c},		/* maybe hue (18) */
	{0x06, 0xfc, 0x0d},
	{0x06, 0xfc, 0x0e},
	{0x06, 0xfc, 0x0f},
	{0x06, 0x18, 0x10},		/* maybe contrast (18) */

	{0x05, 0x01, 0x02},

	{0x04, 0x00, 0x08},		/* compression */
	{0x04, 0x12, 0x09},
	{0x04, 0x21, 0x0a},
	{0x04, 0x10, 0x0b},
	{0x04, 0x21, 0x0c},
	{0x04, 0x1d, 0x00},		/* imagetype (1d) */
	{0x04, 0x41, 0x01},		/* hardware snapcontrol */

	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},
	{0x04, 0x10, 0x06},
	{0x04, 0x10, 0x07},
	{0x04, 0x40, 0x06},
	{0x04, 0x40, 0x07},
	{0x04, 0x00, 0x04},
	{0x04, 0x00, 0x05},

	{0x06, 0x1c, 0x17},
	{0x06, 0xe2, 0x19},
	{0x06, 0x1c, 0x1b},
	{0x06, 0xe2, 0x1d},
	{0x06, 0x5f, 0x1f},
	{0x06, 0x32, 0x20},

	{0x05, initial_brightness >> 6, 0x00},
	{0x05, (initial_brightness << 2) & 0xff, 0x01},
	{0x05, 0x06, 0xc1},
	{0x05, 0x58, 0xc2},
	{0x05, 0x00, 0xca},
	{0x05, 0x00, 0x11},
	{}
};

static int reg_write(struct usb_device *dev,
		     u16 req, u16 index, u16 value)
{
	int ret;

	ret = usb_control_msg(dev,
			usb_sndctrlpipe(dev, 0),
			req,
			USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			value, index, NULL, 0, 500);
	PDEBUG(D_USBO, "reg write: 0x%02x,0x%02x:0x%02x, %d",
		req, index, value, ret);
	if (ret < 0)
		err("reg write: error %d", ret);
	return ret;
}

/* returns: negative is error, pos or zero is data */
static int reg_read(struct gspca_dev *gspca_dev,
			u16 req,	/* bRequest */
			u16 index)	/* wIndex */
{
	int ret;

	ret = usb_control_msg(gspca_dev->dev,
			usb_rcvctrlpipe(gspca_dev->dev, 0),
			req,
			USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			0,			/* value */
			index,
			gspca_dev->usb_buf, 2,
			500);			/* timeout */
	if (ret < 0)
		return ret;
	return (gspca_dev->usb_buf[1] << 8) + gspca_dev->usb_buf[0];
}

static int write_vector(struct gspca_dev *gspca_dev,
			const u8 data[][3])
{
	struct usb_device *dev = gspca_dev->dev;
	int ret, i = 0;

	while (data[i][0] != 0) {
		ret = reg_write(dev, data[i][0], data[i][2], data[i][1]);
		if (ret < 0)
			return ret;
		i++;
	}
	return 0;
}

/* this function is called at probe time */
static int sd_config(struct gspca_dev *gspca_dev,
			const struct usb_device_id *id)
{
	struct sd *sd = (struct sd *) gspca_dev;
	struct cam *cam;

	cam = &gspca_dev->cam;
	cam->cam_mode = vga_mode;
	sd->subtype = id->driver_info;
	if (sd->subtype != IntelPCCameraPro)
		cam->nmodes = ARRAY_SIZE(vga_mode);
	else			/* no 640x480 for IntelPCCameraPro */
		cam->nmodes = ARRAY_SIZE(vga_mode) - 1;
	sd->brightness = BRIGHTNESS_DEF;

	return 0;
}

/* this function is called at probe and resume time */
static int sd_init(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;

	if (write_vector(gspca_dev,
			 sd->subtype == Nxultra
				? spca505b_init_data
				: spca505_init_data))
		return -EIO;
	return 0;
}

static void setbrightness(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	u8 brightness = sd->brightness;

	reg_write(gspca_dev->dev, 0x05, 0x00, (255 - brightness) >> 6);
	reg_write(gspca_dev->dev, 0x05, 0x01, (255 - brightness) << 2);
}

static int sd_start(struct gspca_dev *gspca_dev)
{
	struct sd *sd = (struct sd *) gspca_dev;
	struct usb_device *dev = gspca_dev->dev;
	int ret, mode;
	static u8 mode_tb[][3] = {
	/*	  r00   r06   r07	*/
		{0x00, 0x10, 0x10},	/* 640x480 */
		{0x01, 0x1a, 0x1a},	/* 352x288 */
		{0x02, 0x1c, 0x1d},	/* 320x240 */
		{0x04, 0x34, 0x34},	/* 176x144 */
		{0x05, 0x40, 0x40}	/* 160x120 */
	};

	if (sd->subtype == Nxultra)
		write_vector(gspca_dev, spca505b_open_data_ccd);
	else
		write_vector(gspca_dev, spca505_open_data_ccd);
	ret = reg_read(gspca_dev, 0x06, 0x16);

	if (ret < 0) {
		PDEBUG(D_ERR|D_CONF,
		       "register read failed err: %d",
		       ret);
		return ret;
	}
	if (ret != 0x0101) {
		err("After vector read returns 0x%04x should be 0x0101",
			ret);
	}

	ret = reg_write(gspca_dev->dev, 0x06, 0x16, 0x0a);
	if (ret < 0)
		return ret;
	reg_write(gspca_dev->dev, 0x05, 0xc2, 0x12);

	/* necessary because without it we can see stream
	 * only once after loading module */
	/* stopping usb registers Tomasz change */
	reg_write(dev, 0x02, 0x00, 0x00);

	mode = gspca_dev->cam.cam_mode[(int) gspca_dev->curr_mode].priv;
	reg_write(dev, SPCA50X_REG_COMPRESS, 0x00, mode_tb[mode][0]);
	reg_write(dev, SPCA50X_REG_COMPRESS, 0x06, mode_tb[mode][1]);
	reg_write(dev, SPCA50X_REG_COMPRESS, 0x07, mode_tb[mode][2]);

	ret = reg_write(dev, SPCA50X_REG_USB,
			 SPCA50X_USB_CTRL,
			 SPCA50X_CUSB_ENABLE);

	setbrightness(gspca_dev);

	return ret;
}

static void sd_stopN(struct gspca_dev *gspca_dev)
{
	/* Disable ISO packet machine */
	reg_write(gspca_dev->dev, 0x02, 0x00, 0x00);
}

/* called on streamoff with alt 0 and on disconnect */
static void sd_stop0(struct gspca_dev *gspca_dev)
{
	if (!gspca_dev->present)
		return;

	/* This maybe reset or power control */
	reg_write(gspca_dev->dev, 0x03, 0x03, 0x20);
	reg_write(gspca_dev->dev, 0x03, 0x01, 0x00);
	reg_write(gspca_dev->dev, 0x03, 0x00, 0x01);
	reg_write(gspca_dev->dev, 0x05, 0x10, 0x01);
	reg_write(gspca_dev->dev, 0x05, 0x11, 0x0f);
}

static void sd_pkt_scan(struct gspca_dev *gspca_dev,
			u8 *data,			/* isoc packet */
			int len)			/* iso packet length */
{
	switch (data[0]) {
	case 0:				/* start of frame */
		gspca_frame_add(gspca_dev, LAST_PACKET, NULL, 0);
		data += SPCA50X_OFFSET_DATA;
		len -= SPCA50X_OFFSET_DATA;
		gspca_frame_add(gspca_dev, FIRST_PACKET, data, len);
		break;
	case 0xff:			/* drop */
		break;
	default:
		data += 1;
		len -= 1;
		gspca_frame_add(gspca_dev, INTER_PACKET, data, len);
		break;
	}
}

static int sd_setbrightness(struct gspca_dev *gspca_dev, __s32 val)
{
	struct sd *sd = (struct sd *) gspca_dev;

	sd->brightness = val;
	if (gspca_dev->streaming)
		setbrightness(gspca_dev);
	return 0;
}

static int sd_getbrightness(struct gspca_dev *gspca_dev, __s32 *val)
{
	struct sd *sd = (struct sd *) gspca_dev;

	*val = sd->brightness;
	return 0;
}

/* sub-driver description */
static const struct sd_desc sd_desc = {
	.name = MODULE_NAME,
	.ctrls = sd_ctrls,
	.nctrls = ARRAY_SIZE(sd_ctrls),
	.config = sd_config,
	.init = sd_init,
	.start = sd_start,
	.stopN = sd_stopN,
	.stop0 = sd_stop0,
	.pkt_scan = sd_pkt_scan,
};

/* -- module initialisation -- */
static const __devinitdata struct usb_device_id device_table[] = {
	{USB_DEVICE(0x041e, 0x401d), .driver_info = Nxultra},
	{USB_DEVICE(0x0733, 0x0430), .driver_info = IntelPCCameraPro},
/*fixme: may be UsbGrabberPV321 BRIDGE_SPCA506 SENSOR_SAA7113 */
	{}
};
MODULE_DEVICE_TABLE(usb, device_table);

/* -- device connect -- */
static int sd_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	return gspca_dev_probe(intf, id, &sd_desc, sizeof(struct sd),
				THIS_MODULE);
}

static struct usb_driver sd_driver = {
	.name = MODULE_NAME,
	.id_table = device_table,
	.probe = sd_probe,
	.disconnect = gspca_disconnect,
#ifdef CONFIG_PM
	.suspend = gspca_suspend,
	.resume = gspca_resume,
#endif
};

/* -- module insert / remove -- */
static int __init sd_mod_init(void)
{
	return usb_register(&sd_driver);
}
static void __exit sd_mod_exit(void)
{
	usb_deregister(&sd_driver);
}

module_init(sd_mod_init);
module_exit(sd_mod_exit);
