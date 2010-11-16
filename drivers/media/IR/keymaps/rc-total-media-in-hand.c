/*
 * Total Media In Hand remote controller keytable
 *
 * Copyright (C) 2010 Antti Palosaari <crope@iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License along
 *    with this program; if not, write to the Free Software Foundation, Inc.,
 *    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <media/rc-map.h>

/* Uses NEC extended 0x02bd */
static struct ir_scancode total_media_in_hand[] = {
	{ 0x02bd00, KEY_1 },
	{ 0x02bd01, KEY_2 },
	{ 0x02bd02, KEY_3 },
	{ 0x02bd03, KEY_4 },
	{ 0x02bd04, KEY_5 },
	{ 0x02bd05, KEY_6 },
	{ 0x02bd06, KEY_7 },
	{ 0x02bd07, KEY_8 },
	{ 0x02bd08, KEY_9 },
	{ 0x02bd09, KEY_0 },
	{ 0x02bd0a, KEY_MUTE },
	{ 0x02bd0b, KEY_CYCLEWINDOWS },    /* yellow, [min / max] */
	{ 0x02bd0c, KEY_VIDEO },           /* TV / AV */
	{ 0x02bd0e, KEY_VOLUMEDOWN },
	{ 0x02bd0f, KEY_TIME },            /* TimeShift */
	{ 0x02bd10, KEY_RIGHT },           /* right arrow */
	{ 0x02bd11, KEY_LEFT },            /* left arrow */
	{ 0x02bd12, KEY_UP },              /* up arrow */
	{ 0x02bd13, KEY_DOWN },            /* down arrow */
	{ 0x02bd14, KEY_POWER2 },          /* [red] */
	{ 0x02bd15, KEY_OK },              /* OK */
	{ 0x02bd16, KEY_STOP },
	{ 0x02bd17, KEY_CAMERA },          /* Snapshot */
	{ 0x02bd18, KEY_CHANNELUP },
	{ 0x02bd19, KEY_RECORD },
	{ 0x02bd1a, KEY_CHANNELDOWN },
	{ 0x02bd1c, KEY_ESC },             /* Esc */
	{ 0x02bd1e, KEY_PLAY },
	{ 0x02bd1f, KEY_VOLUMEUP },
	{ 0x02bd40, KEY_PAUSE },
	{ 0x02bd41, KEY_FASTFORWARD },     /* FF >> */
	{ 0x02bd42, KEY_REWIND },          /* FR << */
	{ 0x02bd43, KEY_ZOOM },            /* [window + mouse pointer] */
	{ 0x02bd44, KEY_SHUFFLE },         /* Shuffle */
	{ 0x02bd45, KEY_INFO },            /* [red (I)] */
};

static struct rc_keymap total_media_in_hand_map = {
	.map = {
		.scan    = total_media_in_hand,
		.size    = ARRAY_SIZE(total_media_in_hand),
		.ir_type = IR_TYPE_NEC,
		.name    = RC_MAP_TOTAL_MEDIA_IN_HAND,
	}
};

static int __init init_rc_map_total_media_in_hand(void)
{
	return ir_register_map(&total_media_in_hand_map);
}

static void __exit exit_rc_map_total_media_in_hand(void)
{
	ir_unregister_map(&total_media_in_hand_map);
}

module_init(init_rc_map_total_media_in_hand)
module_exit(exit_rc_map_total_media_in_hand)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antti Palosaari <crope@iki.fi>");
