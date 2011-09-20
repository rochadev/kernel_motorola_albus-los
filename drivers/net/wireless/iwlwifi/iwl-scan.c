/******************************************************************************
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2008 - 2011 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110,
 * USA
 *
 * The full GNU General Public License is included in this distribution
 * in the file called LICENSE.GPL.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *****************************************************************************/
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>

#include "iwl-eeprom.h"
#include "iwl-dev.h"
#include "iwl-core.h"
#include "iwl-sta.h"
#include "iwl-io.h"
#include "iwl-helpers.h"
#include "iwl-agn.h"
#include "iwl-trans.h"

/* For active scan, listen ACTIVE_DWELL_TIME (msec) on each channel after
 * sending probe req.  This should be set long enough to hear probe responses
 * from more than one AP.  */
#define IWL_ACTIVE_DWELL_TIME_24    (30)       /* all times in msec */
#define IWL_ACTIVE_DWELL_TIME_52    (20)

#define IWL_ACTIVE_DWELL_FACTOR_24GHZ (3)
#define IWL_ACTIVE_DWELL_FACTOR_52GHZ (2)

/* For passive scan, listen PASSIVE_DWELL_TIME (msec) on each channel.
 * Must be set longer than active dwell time.
 * For the most reliable scan, set > AP beacon interval (typically 100msec). */
#define IWL_PASSIVE_DWELL_TIME_24   (20)       /* all times in msec */
#define IWL_PASSIVE_DWELL_TIME_52   (10)
#define IWL_PASSIVE_DWELL_BASE      (100)
#define IWL_CHANNEL_TUNE_TIME       5

static int iwl_send_scan_abort(struct iwl_priv *priv)
{
	int ret;
	struct iwl_rx_packet *pkt;
	struct iwl_host_cmd cmd = {
		.id = REPLY_SCAN_ABORT_CMD,
		.flags = CMD_SYNC | CMD_WANT_SKB,
	};

	/* Exit instantly with error when device is not ready
	 * to receive scan abort command or it does not perform
	 * hardware scan currently */
	if (!test_bit(STATUS_READY, &priv->shrd->status) ||
	    !test_bit(STATUS_GEO_CONFIGURED, &priv->shrd->status) ||
	    !test_bit(STATUS_SCAN_HW, &priv->shrd->status) ||
	    test_bit(STATUS_FW_ERROR, &priv->shrd->status) ||
	    test_bit(STATUS_EXIT_PENDING, &priv->shrd->status))
		return -EIO;

	ret = iwl_trans_send_cmd(trans(priv), &cmd);
	if (ret)
		return ret;

	pkt = (struct iwl_rx_packet *)cmd.reply_page;
	if (pkt->u.status != CAN_ABORT_STATUS) {
		/* The scan abort will return 1 for success or
		 * 2 for "failure".  A failure condition can be
		 * due to simply not being in an active scan which
		 * can occur if we send the scan abort before we
		 * the microcode has notified us that a scan is
		 * completed. */
		IWL_DEBUG_SCAN(priv, "SCAN_ABORT ret %d.\n", pkt->u.status);
		ret = -EIO;
	}

	iwl_free_pages(priv->shrd, cmd.reply_page);
	return ret;
}

static void iwl_complete_scan(struct iwl_priv *priv, bool aborted)
{
	/* check if scan was requested from mac80211 */
	if (priv->scan_request) {
		IWL_DEBUG_SCAN(priv, "Complete scan in mac80211\n");
		ieee80211_scan_completed(priv->hw, aborted);
	}

	if (priv->scan_type == IWL_SCAN_ROC) {
		ieee80211_remain_on_channel_expired(priv->hw);
		priv->hw_roc_channel = NULL;
		schedule_delayed_work(&priv->hw_roc_disable_work, 10 * HZ);
	}

	priv->scan_type = IWL_SCAN_NORMAL;
	priv->scan_vif = NULL;
	priv->scan_request = NULL;
}

void iwl_force_scan_end(struct iwl_priv *priv)
{
	lockdep_assert_held(&priv->shrd->mutex);

	if (!test_bit(STATUS_SCANNING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Forcing scan end while not scanning\n");
		return;
	}

	IWL_DEBUG_SCAN(priv, "Forcing scan end\n");
	clear_bit(STATUS_SCANNING, &priv->shrd->status);
	clear_bit(STATUS_SCAN_HW, &priv->shrd->status);
	clear_bit(STATUS_SCAN_ABORTING, &priv->shrd->status);
	iwl_complete_scan(priv, true);
}

static void iwl_do_scan_abort(struct iwl_priv *priv)
{
	int ret;

	lockdep_assert_held(&priv->shrd->mutex);

	if (!test_bit(STATUS_SCANNING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Not performing scan to abort\n");
		return;
	}

	if (test_and_set_bit(STATUS_SCAN_ABORTING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Scan abort in progress\n");
		return;
	}

	ret = iwl_send_scan_abort(priv);
	if (ret) {
		IWL_DEBUG_SCAN(priv, "Send scan abort failed %d\n", ret);
		iwl_force_scan_end(priv);
	} else
		IWL_DEBUG_SCAN(priv, "Successfully send scan abort\n");
}

/**
 * iwl_scan_cancel - Cancel any currently executing HW scan
 */
int iwl_scan_cancel(struct iwl_priv *priv)
{
	IWL_DEBUG_SCAN(priv, "Queuing abort scan\n");
	queue_work(priv->shrd->workqueue, &priv->abort_scan);
	return 0;
}

/**
 * iwl_scan_cancel_timeout - Cancel any currently executing HW scan
 * @ms: amount of time to wait (in milliseconds) for scan to abort
 *
 */
int iwl_scan_cancel_timeout(struct iwl_priv *priv, unsigned long ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(ms);

	lockdep_assert_held(&priv->shrd->mutex);

	IWL_DEBUG_SCAN(priv, "Scan cancel timeout\n");

	iwl_do_scan_abort(priv);

	while (time_before_eq(jiffies, timeout)) {
		if (!test_bit(STATUS_SCAN_HW, &priv->shrd->status))
			break;
		msleep(20);
	}

	return test_bit(STATUS_SCAN_HW, &priv->shrd->status);
}

/* Service response to REPLY_SCAN_CMD (0x80) */
static void iwl_rx_reply_scan(struct iwl_priv *priv,
			      struct iwl_rx_mem_buffer *rxb)
{
#ifdef CONFIG_IWLWIFI_DEBUG
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_scanreq_notification *notif =
	    (struct iwl_scanreq_notification *)pkt->u.raw;

	IWL_DEBUG_SCAN(priv, "Scan request status = 0x%x\n", notif->status);
#endif
}

/* Service SCAN_START_NOTIFICATION (0x82) */
static void iwl_rx_scan_start_notif(struct iwl_priv *priv,
				    struct iwl_rx_mem_buffer *rxb)
{
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_scanstart_notification *notif =
	    (struct iwl_scanstart_notification *)pkt->u.raw;
	priv->scan_start_tsf = le32_to_cpu(notif->tsf_low);
	IWL_DEBUG_SCAN(priv, "Scan start: "
		       "%d [802.11%s] "
		       "(TSF: 0x%08X:%08X) - %d (beacon timer %u)\n",
		       notif->channel,
		       notif->band ? "bg" : "a",
		       le32_to_cpu(notif->tsf_high),
		       le32_to_cpu(notif->tsf_low),
		       notif->status, notif->beacon_timer);

	if (priv->scan_type == IWL_SCAN_ROC)
		ieee80211_ready_on_channel(priv->hw);
}

/* Service SCAN_RESULTS_NOTIFICATION (0x83) */
static void iwl_rx_scan_results_notif(struct iwl_priv *priv,
				      struct iwl_rx_mem_buffer *rxb)
{
#ifdef CONFIG_IWLWIFI_DEBUG
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_scanresults_notification *notif =
	    (struct iwl_scanresults_notification *)pkt->u.raw;

	IWL_DEBUG_SCAN(priv, "Scan ch.res: "
		       "%d [802.11%s] "
		       "(TSF: 0x%08X:%08X) - %d "
		       "elapsed=%lu usec\n",
		       notif->channel,
		       notif->band ? "bg" : "a",
		       le32_to_cpu(notif->tsf_high),
		       le32_to_cpu(notif->tsf_low),
		       le32_to_cpu(notif->statistics[0]),
		       le32_to_cpu(notif->tsf_low) - priv->scan_start_tsf);
#endif
}

/* Service SCAN_COMPLETE_NOTIFICATION (0x84) */
static void iwl_rx_scan_complete_notif(struct iwl_priv *priv,
				       struct iwl_rx_mem_buffer *rxb)
{
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	struct iwl_scancomplete_notification *scan_notif = (void *)pkt->u.raw;

	IWL_DEBUG_SCAN(priv, "Scan complete: %d channels (TSF 0x%08X:%08X) - %d\n",
		       scan_notif->scanned_channels,
		       scan_notif->tsf_low,
		       scan_notif->tsf_high, scan_notif->status);

	/* The HW is no longer scanning */
	clear_bit(STATUS_SCAN_HW, &priv->shrd->status);

	IWL_DEBUG_SCAN(priv, "Scan on %sGHz took %dms\n",
		       (priv->scan_band == IEEE80211_BAND_2GHZ) ? "2.4" : "5.2",
		       jiffies_to_msecs(jiffies - priv->scan_start));

	queue_work(priv->shrd->workqueue, &priv->scan_completed);

	if (priv->iw_mode != NL80211_IFTYPE_ADHOC &&
	    iwl_advanced_bt_coexist(priv) &&
	    priv->bt_status != scan_notif->bt_status) {
		if (scan_notif->bt_status) {
			/* BT on */
			if (!priv->bt_ch_announce)
				priv->bt_traffic_load =
					IWL_BT_COEX_TRAFFIC_LOAD_HIGH;
			/*
			 * otherwise, no traffic load information provided
			 * no changes made
			 */
		} else {
			/* BT off */
			priv->bt_traffic_load =
				IWL_BT_COEX_TRAFFIC_LOAD_NONE;
		}
		priv->bt_status = scan_notif->bt_status;
		queue_work(priv->shrd->workqueue,
			   &priv->bt_traffic_change_work);
	}
}

void iwl_setup_rx_scan_handlers(struct iwl_priv *priv)
{
	/* scan handlers */
	priv->rx_handlers[REPLY_SCAN_CMD] = iwl_rx_reply_scan;
	priv->rx_handlers[SCAN_START_NOTIFICATION] = iwl_rx_scan_start_notif;
	priv->rx_handlers[SCAN_RESULTS_NOTIFICATION] =
					iwl_rx_scan_results_notif;
	priv->rx_handlers[SCAN_COMPLETE_NOTIFICATION] =
					iwl_rx_scan_complete_notif;
}

static u16 iwl_get_active_dwell_time(struct iwl_priv *priv,
				     enum ieee80211_band band, u8 n_probes)
{
	if (band == IEEE80211_BAND_5GHZ)
		return IWL_ACTIVE_DWELL_TIME_52 +
			IWL_ACTIVE_DWELL_FACTOR_52GHZ * (n_probes + 1);
	else
		return IWL_ACTIVE_DWELL_TIME_24 +
			IWL_ACTIVE_DWELL_FACTOR_24GHZ * (n_probes + 1);
}

static u16 iwl_get_passive_dwell_time(struct iwl_priv *priv,
				      enum ieee80211_band band)
{
	struct iwl_rxon_context *ctx;
	u16 passive = (band == IEEE80211_BAND_2GHZ) ?
	    IWL_PASSIVE_DWELL_BASE + IWL_PASSIVE_DWELL_TIME_24 :
	    IWL_PASSIVE_DWELL_BASE + IWL_PASSIVE_DWELL_TIME_52;

	if (iwl_is_any_associated(priv)) {
		/*
		 * If we're associated, we clamp the maximum passive
		 * dwell time to be 98% of the smallest beacon interval
		 * (minus 2 * channel tune time)
		 */
		for_each_context(priv, ctx) {
			u16 value;

			if (!iwl_is_associated_ctx(ctx))
				continue;
			value = ctx->vif ? ctx->vif->bss_conf.beacon_int : 0;
			if ((value > IWL_PASSIVE_DWELL_BASE) || !value)
				value = IWL_PASSIVE_DWELL_BASE;
			value = (value * 98) / 100 - IWL_CHANNEL_TUNE_TIME * 2;
			passive = min(value, passive);
		}
	}

	return passive;
}

static int iwl_get_single_channel_for_scan(struct iwl_priv *priv,
					   struct ieee80211_vif *vif,
					   enum ieee80211_band band,
					   struct iwl_scan_channel *scan_ch)
{
	const struct ieee80211_supported_band *sband;
	u16 passive_dwell = 0;
	u16 active_dwell = 0;
	int added = 0;
	u16 channel = 0;

	sband = iwl_get_hw_mode(priv, band);
	if (!sband) {
		IWL_ERR(priv, "invalid band\n");
		return added;
	}

	active_dwell = iwl_get_active_dwell_time(priv, band, 0);
	passive_dwell = iwl_get_passive_dwell_time(priv, band);

	if (passive_dwell <= active_dwell)
		passive_dwell = active_dwell + 1;

	channel = iwl_get_single_channel_number(priv, band);
	if (channel) {
		scan_ch->channel = cpu_to_le16(channel);
		scan_ch->type = SCAN_CHANNEL_TYPE_PASSIVE;
		scan_ch->active_dwell = cpu_to_le16(active_dwell);
		scan_ch->passive_dwell = cpu_to_le16(passive_dwell);
		/* Set txpower levels to defaults */
		scan_ch->dsp_atten = 110;
		if (band == IEEE80211_BAND_5GHZ)
			scan_ch->tx_gain = ((1 << 5) | (3 << 3)) | 3;
		else
			scan_ch->tx_gain = ((1 << 5) | (5 << 3));
		added++;
	} else
		IWL_ERR(priv, "no valid channel found\n");
	return added;
}

static int iwl_get_channels_for_scan(struct iwl_priv *priv,
				     struct ieee80211_vif *vif,
				     enum ieee80211_band band,
				     u8 is_active, u8 n_probes,
				     struct iwl_scan_channel *scan_ch)
{
	struct ieee80211_channel *chan;
	const struct ieee80211_supported_band *sband;
	const struct iwl_channel_info *ch_info;
	u16 passive_dwell = 0;
	u16 active_dwell = 0;
	int added, i;
	u16 channel;

	sband = iwl_get_hw_mode(priv, band);
	if (!sband)
		return 0;

	active_dwell = iwl_get_active_dwell_time(priv, band, n_probes);
	passive_dwell = iwl_get_passive_dwell_time(priv, band);

	if (passive_dwell <= active_dwell)
		passive_dwell = active_dwell + 1;

	for (i = 0, added = 0; i < priv->scan_request->n_channels; i++) {
		chan = priv->scan_request->channels[i];

		if (chan->band != band)
			continue;

		channel = chan->hw_value;
		scan_ch->channel = cpu_to_le16(channel);

		ch_info = iwl_get_channel_info(priv, band, channel);
		if (!is_channel_valid(ch_info)) {
			IWL_DEBUG_SCAN(priv,
				       "Channel %d is INVALID for this band.\n",
				       channel);
			continue;
		}

		if (!is_active || is_channel_passive(ch_info) ||
		    (chan->flags & IEEE80211_CHAN_PASSIVE_SCAN))
			scan_ch->type = SCAN_CHANNEL_TYPE_PASSIVE;
		else
			scan_ch->type = SCAN_CHANNEL_TYPE_ACTIVE;

		if (n_probes)
			scan_ch->type |= IWL_SCAN_PROBE_MASK(n_probes);

		scan_ch->active_dwell = cpu_to_le16(active_dwell);
		scan_ch->passive_dwell = cpu_to_le16(passive_dwell);

		/* Set txpower levels to defaults */
		scan_ch->dsp_atten = 110;

		/* NOTE: if we were doing 6Mb OFDM for scans we'd use
		 * power level:
		 * scan_ch->tx_gain = ((1 << 5) | (2 << 3)) | 3;
		 */
		if (band == IEEE80211_BAND_5GHZ)
			scan_ch->tx_gain = ((1 << 5) | (3 << 3)) | 3;
		else
			scan_ch->tx_gain = ((1 << 5) | (5 << 3));

		IWL_DEBUG_SCAN(priv, "Scanning ch=%d prob=0x%X [%s %d]\n",
			       channel, le32_to_cpu(scan_ch->type),
			       (scan_ch->type & SCAN_CHANNEL_TYPE_ACTIVE) ?
				"ACTIVE" : "PASSIVE",
			       (scan_ch->type & SCAN_CHANNEL_TYPE_ACTIVE) ?
			       active_dwell : passive_dwell);

		scan_ch++;
		added++;
	}

	IWL_DEBUG_SCAN(priv, "total channels to scan %d\n", added);
	return added;
}

static int iwlagn_request_scan(struct iwl_priv *priv, struct ieee80211_vif *vif)
{
	struct iwl_host_cmd cmd = {
		.id = REPLY_SCAN_CMD,
		.len = { sizeof(struct iwl_scan_cmd), },
		.flags = CMD_SYNC,
	};
	struct iwl_scan_cmd *scan;
	struct iwl_rxon_context *ctx = &priv->contexts[IWL_RXON_CTX_BSS];
	u32 rate_flags = 0;
	u16 cmd_len;
	u16 rx_chain = 0;
	enum ieee80211_band band;
	u8 n_probes = 0;
	u8 rx_ant = hw_params(priv).valid_rx_ant;
	u8 rate;
	bool is_active = false;
	int  chan_mod;
	u8 active_chains;
	u8 scan_tx_antennas = hw_params(priv).valid_tx_ant;
	int ret;

	lockdep_assert_held(&priv->shrd->mutex);

	if (vif)
		ctx = iwl_rxon_ctx_from_vif(vif);

	if (!priv->scan_cmd) {
		priv->scan_cmd = kmalloc(sizeof(struct iwl_scan_cmd) +
					 IWL_MAX_SCAN_SIZE, GFP_KERNEL);
		if (!priv->scan_cmd) {
			IWL_DEBUG_SCAN(priv,
				       "fail to allocate memory for scan\n");
			return -ENOMEM;
		}
	}
	scan = priv->scan_cmd;
	memset(scan, 0, sizeof(struct iwl_scan_cmd) + IWL_MAX_SCAN_SIZE);

	scan->quiet_plcp_th = IWL_PLCP_QUIET_THRESH;
	scan->quiet_time = IWL_ACTIVE_QUIET_TIME;

	if (priv->scan_type != IWL_SCAN_ROC &&
	    iwl_is_any_associated(priv)) {
		u16 interval = 0;
		u32 extra;
		u32 suspend_time = 100;
		u32 scan_suspend_time = 100;

		IWL_DEBUG_INFO(priv, "Scanning while associated...\n");
		switch (priv->scan_type) {
		case IWL_SCAN_ROC:
			WARN_ON(1);
			break;
		case IWL_SCAN_RADIO_RESET:
			interval = 0;
			break;
		case IWL_SCAN_NORMAL:
			interval = vif->bss_conf.beacon_int;
			break;
		}

		scan->suspend_time = 0;
		scan->max_out_time = cpu_to_le32(200 * 1024);
		if (!interval)
			interval = suspend_time;

		extra = (suspend_time / interval) << 22;
		scan_suspend_time = (extra |
		    ((suspend_time % interval) * 1024));
		scan->suspend_time = cpu_to_le32(scan_suspend_time);
		IWL_DEBUG_SCAN(priv, "suspend_time 0x%X beacon interval %d\n",
			       scan_suspend_time, interval);
	} else if (priv->scan_type == IWL_SCAN_ROC) {
		scan->suspend_time = 0;
		scan->max_out_time = 0;
		scan->quiet_time = 0;
		scan->quiet_plcp_th = 0;
	}

	switch (priv->scan_type) {
	case IWL_SCAN_RADIO_RESET:
		IWL_DEBUG_SCAN(priv, "Start internal passive scan.\n");
		break;
	case IWL_SCAN_NORMAL:
		if (priv->scan_request->n_ssids) {
			int i, p = 0;
			IWL_DEBUG_SCAN(priv, "Kicking off active scan\n");
			for (i = 0; i < priv->scan_request->n_ssids; i++) {
				/* always does wildcard anyway */
				if (!priv->scan_request->ssids[i].ssid_len)
					continue;
				scan->direct_scan[p].id = WLAN_EID_SSID;
				scan->direct_scan[p].len =
					priv->scan_request->ssids[i].ssid_len;
				memcpy(scan->direct_scan[p].ssid,
				       priv->scan_request->ssids[i].ssid,
				       priv->scan_request->ssids[i].ssid_len);
				n_probes++;
				p++;
			}
			is_active = true;
		} else
			IWL_DEBUG_SCAN(priv, "Start passive scan.\n");
		break;
	case IWL_SCAN_ROC:
		IWL_DEBUG_SCAN(priv, "Start ROC scan.\n");
		break;
	}

	scan->tx_cmd.tx_flags = TX_CMD_FLG_SEQ_CTL_MSK;
	scan->tx_cmd.sta_id = ctx->bcast_sta_id;
	scan->tx_cmd.stop_time.life_time = TX_CMD_LIFE_TIME_INFINITE;

	switch (priv->scan_band) {
	case IEEE80211_BAND_2GHZ:
		scan->flags = RXON_FLG_BAND_24G_MSK | RXON_FLG_AUTO_DETECT_MSK;
		chan_mod = le32_to_cpu(
			priv->contexts[IWL_RXON_CTX_BSS].active.flags &
						RXON_FLG_CHANNEL_MODE_MSK)
				       >> RXON_FLG_CHANNEL_MODE_POS;
		if (chan_mod == CHANNEL_MODE_PURE_40) {
			rate = IWL_RATE_6M_PLCP;
		} else {
			rate = IWL_RATE_1M_PLCP;
			rate_flags = RATE_MCS_CCK_MSK;
		}
		/*
		 * Internal scans are passive, so we can indiscriminately set
		 * the BT ignore flag on 2.4 GHz since it applies to TX only.
		 */
		if (priv->cfg->bt_params &&
		    priv->cfg->bt_params->advanced_bt_coexist)
			scan->tx_cmd.tx_flags |= TX_CMD_FLG_IGNORE_BT;
		break;
	case IEEE80211_BAND_5GHZ:
		rate = IWL_RATE_6M_PLCP;
		break;
	default:
		IWL_WARN(priv, "Invalid scan band\n");
		return -EIO;
	}

	/*
	 * If active scanning is requested but a certain channel is
	 * marked passive, we can do active scanning if we detect
	 * transmissions.
	 *
	 * There is an issue with some firmware versions that triggers
	 * a sysassert on a "good CRC threshold" of zero (== disabled),
	 * on a radar channel even though this means that we should NOT
	 * send probes.
	 *
	 * The "good CRC threshold" is the number of frames that we
	 * need to receive during our dwell time on a channel before
	 * sending out probes -- setting this to a huge value will
	 * mean we never reach it, but at the same time work around
	 * the aforementioned issue. Thus use IWL_GOOD_CRC_TH_NEVER
	 * here instead of IWL_GOOD_CRC_TH_DISABLED.
	 *
	 * This was fixed in later versions along with some other
	 * scan changes, and the threshold behaves as a flag in those
	 * versions.
	 */
	if (priv->new_scan_threshold_behaviour)
		scan->good_CRC_th = is_active ? IWL_GOOD_CRC_TH_DEFAULT :
						IWL_GOOD_CRC_TH_DISABLED;
	else
		scan->good_CRC_th = is_active ? IWL_GOOD_CRC_TH_DEFAULT :
						IWL_GOOD_CRC_TH_NEVER;

	band = priv->scan_band;

	if (priv->cfg->scan_rx_antennas[band])
		rx_ant = priv->cfg->scan_rx_antennas[band];

	if (band == IEEE80211_BAND_2GHZ &&
	    priv->cfg->bt_params &&
	    priv->cfg->bt_params->advanced_bt_coexist) {
		/* transmit 2.4 GHz probes only on first antenna */
		scan_tx_antennas = first_antenna(scan_tx_antennas);
	}

	priv->scan_tx_ant[band] = iwl_toggle_tx_ant(priv,
						    priv->scan_tx_ant[band],
						    scan_tx_antennas);
	rate_flags |= iwl_ant_idx_to_flags(priv->scan_tx_ant[band]);
	scan->tx_cmd.rate_n_flags = iwl_hw_set_rate_n_flags(rate, rate_flags);

	/* In power save mode use one chain, otherwise use all chains */
	if (test_bit(STATUS_POWER_PMI, &priv->shrd->status)) {
		/* rx_ant has been set to all valid chains previously */
		active_chains = rx_ant &
				((u8)(priv->chain_noise_data.active_chains));
		if (!active_chains)
			active_chains = rx_ant;

		IWL_DEBUG_SCAN(priv, "chain_noise_data.active_chains: %u\n",
				priv->chain_noise_data.active_chains);

		rx_ant = first_antenna(active_chains);
	}
	if (priv->cfg->bt_params &&
	    priv->cfg->bt_params->advanced_bt_coexist &&
	    priv->bt_full_concurrent) {
		/* operated as 1x1 in full concurrency mode */
		rx_ant = first_antenna(rx_ant);
	}

	/* MIMO is not used here, but value is required */
	rx_chain |=
		hw_params(priv).valid_rx_ant << RXON_RX_CHAIN_VALID_POS;
	rx_chain |= rx_ant << RXON_RX_CHAIN_FORCE_MIMO_SEL_POS;
	rx_chain |= rx_ant << RXON_RX_CHAIN_FORCE_SEL_POS;
	rx_chain |= 0x1 << RXON_RX_CHAIN_DRIVER_FORCE_POS;
	scan->rx_chain = cpu_to_le16(rx_chain);
	switch (priv->scan_type) {
	case IWL_SCAN_NORMAL:
		cmd_len = iwl_fill_probe_req(priv,
					(struct ieee80211_mgmt *)scan->data,
					vif->addr,
					priv->scan_request->ie,
					priv->scan_request->ie_len,
					IWL_MAX_SCAN_SIZE - sizeof(*scan));
		break;
	case IWL_SCAN_RADIO_RESET:
	case IWL_SCAN_ROC:
		/* use bcast addr, will not be transmitted but must be valid */
		cmd_len = iwl_fill_probe_req(priv,
					(struct ieee80211_mgmt *)scan->data,
					iwl_bcast_addr, NULL, 0,
					IWL_MAX_SCAN_SIZE - sizeof(*scan));
		break;
	default:
		BUG();
	}
	scan->tx_cmd.len = cpu_to_le16(cmd_len);

	scan->filter_flags |= (RXON_FILTER_ACCEPT_GRP_MSK |
			       RXON_FILTER_BCON_AWARE_MSK);

	switch (priv->scan_type) {
	case IWL_SCAN_RADIO_RESET:
		scan->channel_count =
			iwl_get_single_channel_for_scan(priv, vif, band,
				(void *)&scan->data[cmd_len]);
		break;
	case IWL_SCAN_NORMAL:
		scan->channel_count =
			iwl_get_channels_for_scan(priv, vif, band,
				is_active, n_probes,
				(void *)&scan->data[cmd_len]);
		break;
	case IWL_SCAN_ROC: {
		struct iwl_scan_channel *scan_ch;

		scan->channel_count = 1;

		scan_ch = (void *)&scan->data[cmd_len];
		scan_ch->type = SCAN_CHANNEL_TYPE_PASSIVE;
		scan_ch->channel =
			cpu_to_le16(priv->hw_roc_channel->hw_value);
		scan_ch->active_dwell =
		scan_ch->passive_dwell =
			cpu_to_le16(priv->hw_roc_duration);

		/* Set txpower levels to defaults */
		scan_ch->dsp_atten = 110;

		/* NOTE: if we were doing 6Mb OFDM for scans we'd use
		 * power level:
		 * scan_ch->tx_gain = ((1 << 5) | (2 << 3)) | 3;
		 */
		if (priv->hw_roc_channel->band == IEEE80211_BAND_5GHZ)
			scan_ch->tx_gain = ((1 << 5) | (3 << 3)) | 3;
		else
			scan_ch->tx_gain = ((1 << 5) | (5 << 3));
		}
		break;
	}

	if (scan->channel_count == 0) {
		IWL_DEBUG_SCAN(priv, "channel count %d\n", scan->channel_count);
		return -EIO;
	}

	cmd.len[0] += le16_to_cpu(scan->tx_cmd.len) +
	    scan->channel_count * sizeof(struct iwl_scan_channel);
	cmd.data[0] = scan;
	cmd.dataflags[0] = IWL_HCMD_DFL_NOCOPY;
	scan->len = cpu_to_le16(cmd.len[0]);

	/* set scan bit here for PAN params */
	set_bit(STATUS_SCAN_HW, &priv->shrd->status);

	ret = iwlagn_set_pan_params(priv);
	if (ret)
		return ret;

	ret = iwl_trans_send_cmd(trans(priv), &cmd);
	if (ret) {
		clear_bit(STATUS_SCAN_HW, &priv->shrd->status);
		iwlagn_set_pan_params(priv);
	}

	return ret;
}

void iwl_init_scan_params(struct iwl_priv *priv)
{
	u8 ant_idx = fls(hw_params(priv).valid_tx_ant) - 1;
	if (!priv->scan_tx_ant[IEEE80211_BAND_5GHZ])
		priv->scan_tx_ant[IEEE80211_BAND_5GHZ] = ant_idx;
	if (!priv->scan_tx_ant[IEEE80211_BAND_2GHZ])
		priv->scan_tx_ant[IEEE80211_BAND_2GHZ] = ant_idx;
}

int __must_check iwl_scan_initiate(struct iwl_priv *priv,
				   struct ieee80211_vif *vif,
				   enum iwl_scan_type scan_type,
				   enum ieee80211_band band)
{
	int ret;

	lockdep_assert_held(&priv->shrd->mutex);

	cancel_delayed_work(&priv->scan_check);

	if (!iwl_is_ready_rf(priv->shrd)) {
		IWL_WARN(priv, "Request scan called when driver not ready.\n");
		return -EIO;
	}

	if (test_bit(STATUS_SCAN_HW, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv,
			"Multiple concurrent scan requests in parallel.\n");
		return -EBUSY;
	}

	if (test_bit(STATUS_SCAN_ABORTING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Scan request while abort pending.\n");
		return -EBUSY;
	}

	IWL_DEBUG_SCAN(priv, "Starting %sscan...\n",
			scan_type == IWL_SCAN_NORMAL ? "" :
			scan_type == IWL_SCAN_ROC ? "remain-on-channel " :
			"internal short ");

	set_bit(STATUS_SCANNING, &priv->shrd->status);
	priv->scan_type = scan_type;
	priv->scan_start = jiffies;
	priv->scan_band = band;

	ret = iwlagn_request_scan(priv, vif);
	if (ret) {
		clear_bit(STATUS_SCANNING, &priv->shrd->status);
		priv->scan_type = IWL_SCAN_NORMAL;
		return ret;
	}

	queue_delayed_work(priv->shrd->workqueue, &priv->scan_check,
			   IWL_SCAN_CHECK_WATCHDOG);

	return 0;
}

int iwl_mac_hw_scan(struct ieee80211_hw *hw,
		    struct ieee80211_vif *vif,
		    struct cfg80211_scan_request *req)
{
	struct iwl_priv *priv = hw->priv;
	int ret;

	IWL_DEBUG_MAC80211(priv, "enter\n");

	if (req->n_channels == 0)
		return -EINVAL;

	mutex_lock(&priv->shrd->mutex);

	if (test_bit(STATUS_SCANNING, &priv->shrd->status) &&
	    priv->scan_type != IWL_SCAN_NORMAL) {
		IWL_DEBUG_SCAN(priv, "Scan already in progress.\n");
		ret = -EAGAIN;
		goto out_unlock;
	}

	/* mac80211 will only ask for one band at a time */
	priv->scan_request = req;
	priv->scan_vif = vif;

	/*
	 * If an internal scan is in progress, just set
	 * up the scan_request as per above.
	 */
	if (priv->scan_type != IWL_SCAN_NORMAL) {
		IWL_DEBUG_SCAN(priv, "SCAN request during internal scan\n");
		ret = 0;
	} else
		ret = iwl_scan_initiate(priv, vif, IWL_SCAN_NORMAL,
					req->channels[0]->band);

	IWL_DEBUG_MAC80211(priv, "leave\n");

out_unlock:
	mutex_unlock(&priv->shrd->mutex);

	return ret;
}

/*
 * internal short scan, this function should only been called while associated.
 * It will reset and tune the radio to prevent possible RF related problem
 */
void iwl_internal_short_hw_scan(struct iwl_priv *priv)
{
	queue_work(priv->shrd->workqueue, &priv->start_internal_scan);
}

static void iwl_bg_start_internal_scan(struct work_struct *work)
{
	struct iwl_priv *priv =
		container_of(work, struct iwl_priv, start_internal_scan);

	IWL_DEBUG_SCAN(priv, "Start internal scan\n");

	mutex_lock(&priv->shrd->mutex);

	if (priv->scan_type == IWL_SCAN_RADIO_RESET) {
		IWL_DEBUG_SCAN(priv, "Internal scan already in progress\n");
		goto unlock;
	}

	if (test_bit(STATUS_SCANNING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Scan already in progress.\n");
		goto unlock;
	}

	if (iwl_scan_initiate(priv, NULL, IWL_SCAN_RADIO_RESET, priv->band))
		IWL_DEBUG_SCAN(priv, "failed to start internal short scan\n");
 unlock:
	mutex_unlock(&priv->shrd->mutex);
}

static void iwl_bg_scan_check(struct work_struct *data)
{
	struct iwl_priv *priv =
	    container_of(data, struct iwl_priv, scan_check.work);

	IWL_DEBUG_SCAN(priv, "Scan check work\n");

	/* Since we are here firmware does not finish scan and
	 * most likely is in bad shape, so we don't bother to
	 * send abort command, just force scan complete to mac80211 */
	mutex_lock(&priv->shrd->mutex);
	iwl_force_scan_end(priv);
	mutex_unlock(&priv->shrd->mutex);
}

/**
 * iwl_fill_probe_req - fill in all required fields and IE for probe request
 */

u16 iwl_fill_probe_req(struct iwl_priv *priv, struct ieee80211_mgmt *frame,
		       const u8 *ta, const u8 *ies, int ie_len, int left)
{
	int len = 0;
	u8 *pos = NULL;

	/* Make sure there is enough space for the probe request,
	 * two mandatory IEs and the data */
	left -= 24;
	if (left < 0)
		return 0;

	frame->frame_control = cpu_to_le16(IEEE80211_STYPE_PROBE_REQ);
	memcpy(frame->da, iwl_bcast_addr, ETH_ALEN);
	memcpy(frame->sa, ta, ETH_ALEN);
	memcpy(frame->bssid, iwl_bcast_addr, ETH_ALEN);
	frame->seq_ctrl = 0;

	len += 24;

	/* ...next IE... */
	pos = &frame->u.probe_req.variable[0];

	/* fill in our indirect SSID IE */
	left -= 2;
	if (left < 0)
		return 0;
	*pos++ = WLAN_EID_SSID;
	*pos++ = 0;

	len += 2;

	if (WARN_ON(left < ie_len))
		return len;

	if (ies && ie_len) {
		memcpy(pos, ies, ie_len);
		len += ie_len;
	}

	return (u16)len;
}

static void iwl_bg_abort_scan(struct work_struct *work)
{
	struct iwl_priv *priv = container_of(work, struct iwl_priv, abort_scan);

	IWL_DEBUG_SCAN(priv, "Abort scan work\n");

	/* We keep scan_check work queued in case when firmware will not
	 * report back scan completed notification */
	mutex_lock(&priv->shrd->mutex);
	iwl_scan_cancel_timeout(priv, 200);
	mutex_unlock(&priv->shrd->mutex);
}

static void iwl_bg_scan_completed(struct work_struct *work)
{
	struct iwl_priv *priv =
	    container_of(work, struct iwl_priv, scan_completed);
	bool aborted;

	IWL_DEBUG_SCAN(priv, "Completed scan.\n");

	cancel_delayed_work(&priv->scan_check);

	mutex_lock(&priv->shrd->mutex);

	aborted = test_and_clear_bit(STATUS_SCAN_ABORTING, &priv->shrd->status);
	if (aborted)
		IWL_DEBUG_SCAN(priv, "Aborted scan completed.\n");

	if (!test_and_clear_bit(STATUS_SCANNING, &priv->shrd->status)) {
		IWL_DEBUG_SCAN(priv, "Scan already completed.\n");
		goto out_settings;
	}

	if (priv->scan_type == IWL_SCAN_ROC) {
		ieee80211_remain_on_channel_expired(priv->hw);
		priv->hw_roc_channel = NULL;
		schedule_delayed_work(&priv->hw_roc_disable_work, 10 * HZ);
	}

	if (priv->scan_type != IWL_SCAN_NORMAL && !aborted) {
		int err;

		/* Check if mac80211 requested scan during our internal scan */
		if (priv->scan_request == NULL)
			goto out_complete;

		/* If so request a new scan */
		err = iwl_scan_initiate(priv, priv->scan_vif, IWL_SCAN_NORMAL,
					priv->scan_request->channels[0]->band);
		if (err) {
			IWL_DEBUG_SCAN(priv,
				"failed to initiate pending scan: %d\n", err);
			aborted = true;
			goto out_complete;
		}

		goto out;
	}

out_complete:
	iwl_complete_scan(priv, aborted);

out_settings:
	/* Can we still talk to firmware ? */
	if (!iwl_is_ready_rf(priv->shrd))
		goto out;

	iwlagn_post_scan(priv);

out:
	mutex_unlock(&priv->shrd->mutex);
}

void iwl_setup_scan_deferred_work(struct iwl_priv *priv)
{
	INIT_WORK(&priv->scan_completed, iwl_bg_scan_completed);
	INIT_WORK(&priv->abort_scan, iwl_bg_abort_scan);
	INIT_WORK(&priv->start_internal_scan, iwl_bg_start_internal_scan);
	INIT_DELAYED_WORK(&priv->scan_check, iwl_bg_scan_check);
}

void iwl_cancel_scan_deferred_work(struct iwl_priv *priv)
{
	cancel_work_sync(&priv->start_internal_scan);
	cancel_work_sync(&priv->abort_scan);
	cancel_work_sync(&priv->scan_completed);

	if (cancel_delayed_work_sync(&priv->scan_check)) {
		mutex_lock(&priv->shrd->mutex);
		iwl_force_scan_end(priv);
		mutex_unlock(&priv->shrd->mutex);
	}
}
