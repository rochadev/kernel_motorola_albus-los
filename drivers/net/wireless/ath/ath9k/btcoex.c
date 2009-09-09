/*
 * Copyright (c) 2009 Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ath9k.h"

static const struct ath_btcoex_config ath_bt_config = { 0, true, true,
			ATH_BT_COEX_MODE_SLOTTED, true, true, 2, 5, true };

static const u16 ath_subsysid_tbl[] = {
	AR9280_COEX2WIRE_SUBSYSID,
	AT9285_COEX3WIRE_SA_SUBSYSID,
	AT9285_COEX3WIRE_DA_SUBSYSID
};

/*
 * Checks the subsystem id of the device to see if it
 * supports btcoex
 */
bool ath_btcoex_supported(u16 subsysid)
{
	int i;

	if (!subsysid)
		return false;

	for (i = 0; i < ARRAY_SIZE(ath_subsysid_tbl); i++)
		if (subsysid == ath_subsysid_tbl[i])
			return true;

	return false;
}

void ath9k_hw_init_btcoex_hw_info(struct ath_hw *ah, int qnum)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;
	u32 i;

	btcoex_info->bt_coex_mode =
		(btcoex_info->bt_coex_mode & AR_BT_QCU_THRESH) |
		SM(ath_bt_config.bt_time_extend, AR_BT_TIME_EXTEND) |
		SM(ath_bt_config.bt_txstate_extend, AR_BT_TXSTATE_EXTEND) |
		SM(ath_bt_config.bt_txframe_extend, AR_BT_TX_FRAME_EXTEND) |
		SM(ath_bt_config.bt_mode, AR_BT_MODE) |
		SM(ath_bt_config.bt_quiet_collision, AR_BT_QUIET) |
		SM(ath_bt_config.bt_rxclear_polarity, AR_BT_RX_CLEAR_POLARITY) |
		SM(ath_bt_config.bt_priority_time, AR_BT_PRIORITY_TIME) |
		SM(ath_bt_config.bt_first_slot_time, AR_BT_FIRST_SLOT_TIME) |
		SM(qnum, AR_BT_QCU_THRESH);

	btcoex_info->bt_coex_mode2 =
		SM(ath_bt_config.bt_hold_rx_clear, AR_BT_HOLD_RX_CLEAR) |
		SM(ATH_BTCOEX_BMISS_THRESH, AR_BT_BCN_MISS_THRESH) |
		AR_BT_DISABLE_BT_ANT;

	for (i = 0; i < 32; i++)
		ah->hw_gen_timers.gen_timer_index[(debruijn32 << i) >> 27] = i;
}

void ath9k_hw_btcoex_init_2wire(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	/* connect bt_active to baseband */
	REG_CLR_BIT(ah, AR_GPIO_INPUT_EN_VAL,
		    (AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_DEF |
		     AR_GPIO_INPUT_EN_VAL_BT_FREQUENCY_DEF));

	REG_SET_BIT(ah, AR_GPIO_INPUT_EN_VAL,
		    AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_BB);

	/* Set input mux for bt_active to gpio pin */
	REG_RMW_FIELD(ah, AR_GPIO_INPUT_MUX1,
		      AR_GPIO_INPUT_MUX1_BT_ACTIVE,
		      btcoex_info->btactive_gpio);

	/* Configure the desired gpio port for input */
	ath9k_hw_cfg_gpio_input(ah, btcoex_info->btactive_gpio);
}

void ath9k_hw_btcoex_init_3wire(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	/* btcoex 3-wire */
	REG_SET_BIT(ah, AR_GPIO_INPUT_EN_VAL,
			(AR_GPIO_INPUT_EN_VAL_BT_PRIORITY_BB |
			 AR_GPIO_INPUT_EN_VAL_BT_ACTIVE_BB));

	/* Set input mux for bt_prority_async and
	 *                  bt_active_async to GPIO pins */
	REG_RMW_FIELD(ah, AR_GPIO_INPUT_MUX1,
			AR_GPIO_INPUT_MUX1_BT_ACTIVE,
			btcoex_info->btactive_gpio);

	REG_RMW_FIELD(ah, AR_GPIO_INPUT_MUX1,
			AR_GPIO_INPUT_MUX1_BT_PRIORITY,
			btcoex_info->btpriority_gpio);

	/* Configure the desired GPIO ports for input */

	ath9k_hw_cfg_gpio_input(ah, btcoex_info->btactive_gpio);
	ath9k_hw_cfg_gpio_input(ah, btcoex_info->btpriority_gpio);
}

static void ath9k_hw_btcoex_enable_2wire(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	/* Configure the desired GPIO port for TX_FRAME output */
	ath9k_hw_cfg_output(ah, btcoex_info->wlanactive_gpio,
			    AR_GPIO_OUTPUT_MUX_AS_TX_FRAME);
}

static void ath9k_hw_btcoex_enable_3wire(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	/*
	 * Program coex mode and weight registers to
	 * enable coex 3-wire
	 */
	REG_WRITE(ah, AR_BT_COEX_MODE, btcoex_info->bt_coex_mode);
	REG_WRITE(ah, AR_BT_COEX_WEIGHT, btcoex_info->bt_coex_weights);
	REG_WRITE(ah, AR_BT_COEX_MODE2, btcoex_info->bt_coex_mode2);

	REG_RMW_FIELD(ah, AR_QUIET1, AR_QUIET1_QUIET_ACK_CTS_ENABLE, 1);
	REG_RMW_FIELD(ah, AR_PCU_MISC, AR_PCU_BT_ANT_PREVENT_RX, 0);

	ath9k_hw_cfg_output(ah, btcoex_info->wlanactive_gpio,
			    AR_GPIO_OUTPUT_MUX_AS_RX_CLEAR_EXTERNAL);
}

void ath9k_hw_btcoex_enable(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	switch (btcoex_info->scheme) {
	case ATH_BTCOEX_CFG_NONE:
		break;
	case ATH_BTCOEX_CFG_2WIRE:
		ath9k_hw_btcoex_enable_2wire(ah);
		break;
	case ATH_BTCOEX_CFG_3WIRE:
		ath9k_hw_btcoex_enable_3wire(ah);
		break;
	}

	REG_RMW(ah, AR_GPIO_PDPU,
		(0x2 << (btcoex_info->btactive_gpio * 2)),
		(0x3 << (btcoex_info->btactive_gpio * 2)));

	ah->btcoex_info.enabled = true;
}

void ath9k_hw_btcoex_disable(struct ath_hw *ah)
{
	struct ath_btcoex_info *btcoex_info = &ah->btcoex_info;

	ath9k_hw_set_gpio(ah, btcoex_info->wlanactive_gpio, 0);

	ath9k_hw_cfg_output(ah, btcoex_info->wlanactive_gpio,
			AR_GPIO_OUTPUT_MUX_AS_OUTPUT);

	if (btcoex_info->scheme == ATH_BTCOEX_CFG_3WIRE) {
		REG_WRITE(ah, AR_BT_COEX_MODE, AR_BT_QUIET | AR_BT_MODE);
		REG_WRITE(ah, AR_BT_COEX_WEIGHT, 0);
		REG_WRITE(ah, AR_BT_COEX_MODE2, 0);
	}

	ah->btcoex_info.enabled = false;
}
