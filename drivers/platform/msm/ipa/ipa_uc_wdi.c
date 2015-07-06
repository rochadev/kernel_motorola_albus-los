/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "ipa_i.h"
#include <linux/dmapool.h>
#include <linux/delay.h>

#define IPA_HOLB_TMR_DIS 0x0

#define IPA_HW_INTERFACE_WDI_VERSION 0x0001
#define IPA_HW_WDI_RX_MBOX_START_INDEX 48
#define IPA_HW_WDI_TX_MBOX_START_INDEX 50
#define IPA_WDI_RING_ALIGNMENT 8

#define IPA_WDI_CONNECTED BIT(0)
#define IPA_WDI_ENABLED BIT(1)
#define IPA_WDI_RESUMED BIT(2)
#define IPA_UC_POLL_SLEEP_USEC 100

#define IPA_WDI_RX_RING_RES	0
#define IPA_WDI_RX_RING_RP_RES	1
#define IPA_WDI_TX_RING_RES	2
#define IPA_WDI_CE_RING_RES	3
#define IPA_WDI_CE_DB_RES	4
#define IPA_WDI_MAX_RES		5

struct ipa_wdi_res {
	struct ipa_wdi_buffer_info *res;
	unsigned int nents;
	bool valid;
};

static struct ipa_wdi_res wdi_res[IPA_WDI_MAX_RES];

static void ipa_uc_wdi_loaded_handler(void);

/**
 * enum ipa_hw_2_cpu_wdi_events - Values that represent HW event to be sent to CPU.
 * @IPA_HW_2_CPU_EVENT_WDI_ERROR : Event to specify that HW detected an error
 * in WDI
 */
enum ipa_hw_2_cpu_wdi_events {
	IPA_HW_2_CPU_EVENT_WDI_ERROR =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 0),
};

/**
 * enum ipa_hw_wdi_channel_states - Values that represent WDI channel state
 * machine.
 * @IPA_HW_WDI_CHANNEL_STATE_INITED_DISABLED : Channel is initialized but
 * disabled
 * @IPA_HW_WDI_CHANNEL_STATE_ENABLED_SUSPEND : Channel is enabled but in
 * suspended state
 * @IPA_HW_WDI_CHANNEL_STATE_RUNNING : Channel is running. Entered after
 * SET_UP_COMMAND is processed successfully
 * @IPA_HW_WDI_CHANNEL_STATE_ERROR : Channel is in error state
 * @IPA_HW_WDI_CHANNEL_STATE_INVALID : Invalid state. Shall not be in use in
 * operational scenario
 *
 * These states apply to both Tx and Rx paths. These do not reflect the
 * sub-state the state machine may be in.
 */
enum ipa_hw_wdi_channel_states {
	IPA_HW_WDI_CHANNEL_STATE_INITED_DISABLED = 1,
	IPA_HW_WDI_CHANNEL_STATE_ENABLED_SUSPEND = 2,
	IPA_HW_WDI_CHANNEL_STATE_RUNNING         = 3,
	IPA_HW_WDI_CHANNEL_STATE_ERROR           = 4,
	IPA_HW_WDI_CHANNEL_STATE_INVALID         = 0xFF
};

/**
 * enum ipa_cpu_2_hw_commands -  Values that represent the WDI commands from CPU
 * @IPA_CPU_2_HW_CMD_WDI_TX_SET_UP : Command to set up WDI Tx Path
 * @IPA_CPU_2_HW_CMD_WDI_RX_SET_UP : Command to set up WDI Rx Path
 * @IPA_CPU_2_HW_CMD_WDI_RX_EXT_CFG : Provide extended config info for Rx path
 * @IPA_CPU_2_HW_CMD_WDI_CH_ENABLE : Command to enable a channel
 * @IPA_CPU_2_HW_CMD_WDI_CH_DISABLE : Command to disable a channel
 * @IPA_CPU_2_HW_CMD_WDI_CH_SUSPEND : Command to suspend a channel
 * @IPA_CPU_2_HW_CMD_WDI_CH_RESUME : Command to resume a channel
 * @IPA_CPU_2_HW_CMD_WDI_TEAR_DOWN : Command to tear down WDI Tx/ Rx Path
 */
enum ipa_cpu_2_hw_wdi_commands {
	IPA_CPU_2_HW_CMD_WDI_TX_SET_UP  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 0),
	IPA_CPU_2_HW_CMD_WDI_RX_SET_UP  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 1),
	IPA_CPU_2_HW_CMD_WDI_RX_EXT_CFG =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 2),
	IPA_CPU_2_HW_CMD_WDI_CH_ENABLE  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 3),
	IPA_CPU_2_HW_CMD_WDI_CH_DISABLE =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 4),
	IPA_CPU_2_HW_CMD_WDI_CH_SUSPEND =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 5),
	IPA_CPU_2_HW_CMD_WDI_CH_RESUME  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 6),
	IPA_CPU_2_HW_CMD_WDI_TEAR_DOWN  =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 7),
};

/**
 * enum ipa_hw_2_cpu_cmd_resp_status -  Values that represent WDI related
 * command response status to be sent to CPU.
 */
enum ipa_hw_2_cpu_cmd_resp_status {
	IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS            =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 0),
	IPA_HW_2_CPU_MAX_WDI_TX_CHANNELS               =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 1),
	IPA_HW_2_CPU_WDI_CE_RING_OVERRUN_POSSIBILITY   =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 2),
	IPA_HW_2_CPU_WDI_CE_RING_SET_UP_FAILURE        =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 3),
	IPA_HW_2_CPU_WDI_CE_RING_PARAMS_UNALIGNED      =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 4),
	IPA_HW_2_CPU_WDI_COMP_RING_OVERRUN_POSSIBILITY =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 5),
	IPA_HW_2_CPU_WDI_COMP_RING_SET_UP_FAILURE      =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 6),
	IPA_HW_2_CPU_WDI_COMP_RING_PARAMS_UNALIGNED    =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 7),
	IPA_HW_2_CPU_WDI_UNKNOWN_TX_CHANNEL            =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 8),
	IPA_HW_2_CPU_WDI_TX_INVALID_FSM_TRANSITION     =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 9),
	IPA_HW_2_CPU_WDI_TX_FSM_TRANSITION_ERROR       =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 10),
	IPA_HW_2_CPU_MAX_WDI_RX_CHANNELS               =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 11),
	IPA_HW_2_CPU_WDI_RX_RING_PARAMS_UNALIGNED      =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 12),
	IPA_HW_2_CPU_WDI_RX_RING_SET_UP_FAILURE        =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 13),
	IPA_HW_2_CPU_WDI_UNKNOWN_RX_CHANNEL            =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 14),
	IPA_HW_2_CPU_WDI_RX_INVALID_FSM_TRANSITION     =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 15),
	IPA_HW_2_CPU_WDI_RX_FSM_TRANSITION_ERROR       =
		FEATURE_ENUM_VAL(IPA_HW_FEATURE_WDI, 16),
};

/**
 * enum ipa_hw_wdi_errors - WDI specific error types.
 * @IPA_HW_WDI_ERROR_NONE : No error persists
 * @IPA_HW_WDI_CHANNEL_ERROR : Error is specific to channel
 */
enum ipa_hw_wdi_errors {
	IPA_HW_WDI_ERROR_NONE    = 0,
	IPA_HW_WDI_CHANNEL_ERROR = 1
};

/**
 * enum ipa_hw_wdi_ch_errors = List of WDI Channel error types. This is present
 * in the event param.
 * @IPA_HW_WDI_CH_ERR_NONE : No error persists
 * @IPA_HW_WDI_TX_COMP_RING_WP_UPDATE_FAIL : Write pointer update failed in Tx
 * Completion ring
 * @IPA_HW_WDI_TX_FSM_ERROR : Error in the state machine transition
 * @IPA_HW_WDI_TX_COMP_RE_FETCH_FAIL : Error while calculating num RE to bring
 * @IPA_HW_WDI_CH_ERR_RESERVED : Reserved - Not available for CPU to use
*/
enum ipa_hw_wdi_ch_errors {
	IPA_HW_WDI_CH_ERR_NONE                 = 0,
	IPA_HW_WDI_TX_COMP_RING_WP_UPDATE_FAIL = 1,
	IPA_HW_WDI_TX_FSM_ERROR                = 2,
	IPA_HW_WDI_TX_COMP_RE_FETCH_FAIL       = 3,
	IPA_HW_WDI_CH_ERR_RESERVED             = 0xFF
};

/**
 * struct IpaHwSharedMemWdiMapping_t  - Structure referring to the common and
 * WDI section of 128B shared memory located in offset zero of SW Partition in
 * IPA SRAM.
 *
 * The shared memory is used for communication between IPA HW and CPU.
 */
struct IpaHwSharedMemWdiMapping_t {
	struct IpaHwSharedMemCommonMapping_t common;
	u32 reserved_2B_28;
	u32 reserved_2F_2C;
	u32 reserved_33_30;
	u32 reserved_37_34;
	u32 reserved_3B_38;
	u32 reserved_3F_3C;
	u16 interfaceVersionWdi;
	u16 reserved_43_42;
	u8  wdi_tx_ch_0_state;
	u8  wdi_rx_ch_0_state;
	u16 reserved_47_46;
} __packed;

/**
 * struct IpaHwWdiTxSetUpCmdData_t - Structure holding the parameters for
 * IPA_CPU_2_HW_CMD_WDI_TX_SET_UP command.
 * @comp_ring_base_pa : This is the physical address of the base of the Tx
 * completion ring
 * @comp_ring_size : This is the size of the Tx completion ring
 * @reserved_comp_ring : Reserved field for expansion of Completion ring params
 * @ce_ring_base_pa : This is the physical address of the base of the Copy
 * Engine Source Ring
 * @ce_ring_size : Copy Engine Ring size
 * @reserved_ce_ring : Reserved field for expansion of CE ring params
 * @ce_ring_doorbell_pa : This is the physical address of the doorbell that the
 * IPA uC has to write into to trigger the copy engine
 * @num_tx_buffers : Number of pkt buffers allocated. The size of the CE ring
 * and the Tx completion ring has to be atleast ( num_tx_buffers + 1)
 * @ipa_pipe_number : This is the IPA pipe number that has to be used for the
 * Tx path
 * @reserved : Reserved field
 *
 * Parameters are sent as pointer thus should be reside in address accessible
 * to HW
 */
struct IpaHwWdiTxSetUpCmdData_t {
	u32 comp_ring_base_pa;
	u16 comp_ring_size;
	u16 reserved_comp_ring;
	u32 ce_ring_base_pa;
	u16 ce_ring_size;
	u16 reserved_ce_ring;
	u32 ce_ring_doorbell_pa;
	u16 num_tx_buffers;
	u8  ipa_pipe_number;
	u8  reserved;
} __packed;

/**
 * struct IpaHwWdiRxSetUpCmdData_t -  Structure holding the parameters for
 * IPA_CPU_2_HW_CMD_WDI_RX_SET_UP command.
 * @rx_ring_base_pa : This is the physical address of the base of the Rx ring
 * (containing Rx buffers)
 * @rx_ring_size : This is the size of the Rx ring
 * @rx_ring_rp_pa : This is the physical address of the location through which
 * IPA uc is expected to communicate about the Read pointer into the Rx Ring
 * @ipa_pipe_number : This is the IPA pipe number that has to be used for the
 * Rx path
 *
 * Parameters are sent as pointer thus should be reside in address accessible
 * to HW
*/
struct IpaHwWdiRxSetUpCmdData_t {
	u32 rx_ring_base_pa;
	u32 rx_ring_size;
	u32 rx_ring_rp_pa;
	u8  ipa_pipe_number;
} __packed;

/**
 * union IpaHwWdiRxExtCfgCmdData_t - Structure holding the parameters for
 * IPA_CPU_2_HW_CMD_WDI_RX_EXT_CFG command.
 * @ipa_pipe_number : The IPA pipe number for which this config is passed
 * @qmap_id : QMAP ID to be set in the metadata register
 * @reserved : Reserved
 *
 * The parameters are passed as immediate params in the shared memory
*/
union IpaHwWdiRxExtCfgCmdData_t {
	struct IpaHwWdiRxExtCfgCmdParams_t {
		u32 ipa_pipe_number:8;
		u32 qmap_id:8;
		u32 reserved:16;
	} __packed params;
	u32 raw32b;
} __packed;

/**
 * union IpaHwWdiCommonChCmdData_t -  Structure holding the parameters for
 * IPA_CPU_2_HW_CMD_WDI_TEAR_DOWN,
 * IPA_CPU_2_HW_CMD_WDI_CH_ENABLE,
 * IPA_CPU_2_HW_CMD_WDI_CH_DISABLE,
 * IPA_CPU_2_HW_CMD_WDI_CH_SUSPEND,
 * IPA_CPU_2_HW_CMD_WDI_CH_RESUME command.
 * @ipa_pipe_number :  The IPA pipe number. This could be Tx or an Rx pipe
 * @reserved : Reserved
 *
 * The parameters are passed as immediate params in the shared memory
 */
union IpaHwWdiCommonChCmdData_t {
	struct IpaHwWdiCommonChCmdParams_t {
		u32 ipa_pipe_number:8;
		u32 reserved:24;
	} __packed params;
	u32 raw32b;
} __packed;

/**
 * union IpaHwWdiErrorEventData_t - parameters for IPA_HW_2_CPU_EVENT_WDI_ERROR
 * event.
 * @wdi_error_type : The IPA pipe number to be torn down. This could be Tx or
 * an Rx pipe
 * @reserved : Reserved
 * @ipa_pipe_number : IPA pipe number on which error has happened. Applicable
 * only if error type indicates channel error
 * @wdi_ch_err_type : Information about the channel error (if available)
 *
 * The parameters are passed as immediate params in the shared memory
 */
union IpaHwWdiErrorEventData_t {
	struct IpaHwWdiErrorEventParams_t {
		u32 wdi_error_type:8;
		u32 reserved:8;
		u32 ipa_pipe_number:8;
		u32 wdi_ch_err_type:8;
	} __packed params;
	u32 raw32b;
} __packed;

static void ipa_uc_wdi_event_log_info_handler(
struct IpaHwEventLogInfoData_t *uc_event_top_mmio)

{
	if ((uc_event_top_mmio->featureMask & (1 << IPA_HW_FEATURE_WDI)) == 0) {
		IPAERR("WDI feature missing 0x%x\n",
			uc_event_top_mmio->featureMask);
		return;
	}

	if (uc_event_top_mmio->statsInfo.featureInfo[IPA_HW_FEATURE_WDI].
		params.size != sizeof(struct IpaHwStatsWDIInfoData_t)) {
			IPAERR("wdi stats sz invalid exp=%zu is=%u\n",
				sizeof(struct IpaHwStatsWDIInfoData_t),
				uc_event_top_mmio->statsInfo.
				featureInfo[IPA_HW_FEATURE_WDI].params.size);
			return;
	}

	ipa_ctx->uc_wdi_ctx.wdi_uc_stats_ofst = uc_event_top_mmio->
		statsInfo.baseAddrOffset + uc_event_top_mmio->statsInfo.
		featureInfo[IPA_HW_FEATURE_WDI].params.offset;
	IPAERR("WDI stats ofst=0x%x\n", ipa_ctx->uc_wdi_ctx.wdi_uc_stats_ofst);
	if (ipa_ctx->uc_wdi_ctx.wdi_uc_stats_ofst +
		sizeof(struct IpaHwStatsWDIInfoData_t) >=
		ipa_ctx->ctrl->ipa_reg_base_ofst +
		IPA_SRAM_DIRECT_ACCESS_N_OFST_v2_0(0) +
		ipa_ctx->smem_sz) {
			IPAERR("uc_wdi_stats 0x%x outside SRAM\n",
				ipa_ctx->uc_wdi_ctx.wdi_uc_stats_ofst);
			return;
	}

	ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio =
		ioremap(ipa_ctx->ipa_wrapper_base +
		ipa_ctx->uc_wdi_ctx.wdi_uc_stats_ofst,
		sizeof(struct IpaHwStatsWDIInfoData_t));
	if (!ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio) {
		IPAERR("fail to ioremap uc wdi stats\n");
		return;
	}

	return;
}

static void ipa_uc_wdi_event_handler(struct IpaHwSharedMemCommonMapping_t
				     *uc_sram_mmio)

{
	union IpaHwWdiErrorEventData_t wdi_evt;
	struct IpaHwSharedMemWdiMapping_t *wdi_sram_mmio_ext;

	if (uc_sram_mmio->eventOp ==
		IPA_HW_2_CPU_EVENT_WDI_ERROR) {
			wdi_evt.raw32b = uc_sram_mmio->eventParams;
			IPADBG("uC WDI evt errType=%u pipe=%d cherrType=%u\n",
				wdi_evt.params.wdi_error_type,
				wdi_evt.params.ipa_pipe_number,
				wdi_evt.params.wdi_ch_err_type);
			wdi_sram_mmio_ext =
				(struct IpaHwSharedMemWdiMapping_t *)
				uc_sram_mmio;
			IPADBG("tx_ch_state=%u rx_ch_state=%u\n",
				wdi_sram_mmio_ext->wdi_tx_ch_0_state,
				wdi_sram_mmio_ext->wdi_rx_ch_0_state);
	}
}

/**
 * ipa_get_wdi_stats() - Query WDI statistics from uc
 * @stats:	[inout] stats blob from client populated by driver
 *
 * Returns:	0 on success, negative on failure
 *
 * @note Cannot be called from atomic context
 *
 */
int ipa_get_wdi_stats(struct IpaHwStatsWDIInfoData_t *stats)
{
#define TX_STATS(y) stats->tx_ch_stats.y = \
	ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio->tx_ch_stats.y
#define RX_STATS(y) stats->rx_ch_stats.y = \
	ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio->rx_ch_stats.y

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (!stats || !ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio) {
		IPAERR("bad parms stats=%p wdi_stats=%p\n",
			stats,
			ipa_ctx->uc_wdi_ctx.wdi_uc_stats_mmio);
		return -EINVAL;
	}

	ipa_inc_client_enable_clks();

	TX_STATS(num_pkts_processed);
	TX_STATS(copy_engine_doorbell_value);
	TX_STATS(num_db_fired);
	TX_STATS(tx_comp_ring_stats.ringFull);
	TX_STATS(tx_comp_ring_stats.ringEmpty);
	TX_STATS(tx_comp_ring_stats.ringUsageHigh);
	TX_STATS(tx_comp_ring_stats.ringUsageLow);
	TX_STATS(tx_comp_ring_stats.RingUtilCount);
	TX_STATS(bam_stats.bamFifoFull);
	TX_STATS(bam_stats.bamFifoEmpty);
	TX_STATS(bam_stats.bamFifoUsageHigh);
	TX_STATS(bam_stats.bamFifoUsageLow);
	TX_STATS(bam_stats.bamUtilCount);
	TX_STATS(num_db);
	TX_STATS(num_unexpected_db);
	TX_STATS(num_bam_int_handled);
	TX_STATS(num_bam_int_in_non_runnning_state);
	TX_STATS(num_qmb_int_handled);
	TX_STATS(num_bam_int_handled_while_wait_for_bam);

	RX_STATS(max_outstanding_pkts);
	RX_STATS(num_pkts_processed);
	RX_STATS(rx_ring_rp_value);
	RX_STATS(rx_ind_ring_stats.ringFull);
	RX_STATS(rx_ind_ring_stats.ringEmpty);
	RX_STATS(rx_ind_ring_stats.ringUsageHigh);
	RX_STATS(rx_ind_ring_stats.ringUsageLow);
	RX_STATS(rx_ind_ring_stats.RingUtilCount);
	RX_STATS(bam_stats.bamFifoFull);
	RX_STATS(bam_stats.bamFifoEmpty);
	RX_STATS(bam_stats.bamFifoUsageHigh);
	RX_STATS(bam_stats.bamFifoUsageLow);
	RX_STATS(bam_stats.bamUtilCount);
	RX_STATS(num_bam_int_handled);
	RX_STATS(num_db);
	RX_STATS(num_unexpected_db);
	RX_STATS(num_pkts_in_dis_uninit_state);
	RX_STATS(reserved1);
	RX_STATS(reserved2);

	ipa_dec_client_disable_clks();

	return 0;
}
EXPORT_SYMBOL(ipa_get_wdi_stats);

int ipa_wdi_init(void)
{
	struct ipa_uc_hdlrs uc_wdi_cbs = { 0 };

	uc_wdi_cbs.ipa_uc_event_hdlr = ipa_uc_wdi_event_handler;
	uc_wdi_cbs.ipa_uc_event_log_info_hdlr =
		ipa_uc_wdi_event_log_info_handler;
	uc_wdi_cbs.ipa_uc_loaded_hdlr =
		ipa_uc_wdi_loaded_handler;

	ipa_uc_register_handlers(IPA_HW_FEATURE_WDI, &uc_wdi_cbs);

	return 0;
}

static int ipa_create_uc_smmu_mapping_pa(phys_addr_t pa, size_t len,
		bool device, unsigned long *iova)
{
	struct ipa_smmu_cb_ctx *cb = ipa_get_uc_smmu_ctx();
	unsigned long va = roundup(cb->next_addr, PAGE_SIZE);
	int prot = IOMMU_READ | IOMMU_WRITE;
	size_t true_len = roundup(len + pa - rounddown(pa, PAGE_SIZE),
			PAGE_SIZE);
	int ret;

	if (!cb->valid) {
		IPAERR("No SMMU CB setup\n");
		return -EINVAL;
	}

	ret = iommu_map(cb->mapping->domain, va, rounddown(pa, PAGE_SIZE),
			true_len,
			device ? (prot | IOMMU_DEVICE) : prot);
	if (ret) {
		IPAERR("iommu map failed for pa=%pa len=%lu\n", &pa, true_len);
		return -EINVAL;
	}

	ipa_ctx->wdi_map_cnt++;
	cb->next_addr = va + true_len;
	*iova = va + pa - rounddown(pa, PAGE_SIZE);
	return 0;
}

static int ipa_create_uc_smmu_mapping_sgt(struct sg_table *sgt,
		unsigned long *iova)
{
	struct ipa_smmu_cb_ctx *cb = ipa_get_uc_smmu_ctx();
	unsigned long va = roundup(cb->next_addr, PAGE_SIZE);
	int prot = IOMMU_READ | IOMMU_WRITE;
	int ret;
	int i;
	struct scatterlist *sg;
	unsigned long start_iova = va;
	phys_addr_t phys;
	size_t len;
	int count = 0;

	if (!cb->valid) {
		IPAERR("No SMMU CB setup\n");
		return -EINVAL;
	}

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		phys = page_to_phys(sg_page(sg));
		len = PAGE_ALIGN(sg->offset + sg->length);

		ret = iommu_map(cb->mapping->domain, va, phys, len, prot);
		if (ret) {
			IPAERR("iommu map failed for pa=%pa len=%lu\n",
					&phys, len);
			goto bad_mapping;
		}
		va += len;
		ipa_ctx->wdi_map_cnt++;
		count++;
	}
	cb->next_addr = va;
	*iova = start_iova;

	return 0;

bad_mapping:
	for_each_sg(sgt->sgl, sg, count, i)
		iommu_unmap(cb->mapping->domain, sg_dma_address(sg),
				sg_dma_len(sg));
	return -EINVAL;
}

static void ipa_release_uc_smmu_mappings(enum ipa_client_type client)
{
	struct ipa_smmu_cb_ctx *cb = ipa_get_uc_smmu_ctx();
	int i;
	int j;
	int start;
	int end;

	if (IPA_CLIENT_IS_CONS(client)) {
		start = IPA_WDI_TX_RING_RES;
		end = IPA_WDI_CE_DB_RES;
	} else {
		start = IPA_WDI_RX_RING_RES;
		end = IPA_WDI_RX_RING_RP_RES;
	}

	for (i = start; i <= end; i++) {
		if (wdi_res[i].valid) {
			for (j = 0; j < wdi_res[i].nents; j++) {
				iommu_unmap(cb->mapping->domain,
					wdi_res[i].res[j].iova,
					wdi_res[i].res[j].size);
				ipa_ctx->wdi_map_cnt--;
			}
			kfree(wdi_res[i].res);
			wdi_res[i].valid = false;
		}
	}

	if (ipa_ctx->wdi_map_cnt == 0)
		cb->next_addr = IPA_SMMU_UC_VA_END;

}

static void ipa_save_uc_smmu_mapping_pa(int res_idx, phys_addr_t pa,
		unsigned long iova, size_t len)
{
	IPADBG("--res_idx=%d pa=0x%pa iova=0x%lx sz=0x%zx\n", res_idx,
			&pa, iova, len);
	wdi_res[res_idx].res = kzalloc(sizeof(struct ipa_wdi_res), GFP_KERNEL);
	if (!wdi_res[res_idx].res)
		BUG();
	wdi_res[res_idx].nents = 1;
	wdi_res[res_idx].valid = true;
	wdi_res[res_idx].res->pa = rounddown(pa, PAGE_SIZE);
	wdi_res[res_idx].res->iova = rounddown(iova, PAGE_SIZE);
	wdi_res[res_idx].res->size = roundup(len + pa - rounddown(pa,
				PAGE_SIZE), PAGE_SIZE);
	IPADBG("res_idx=%d pa=0x%pa iova=0x%lx sz=0x%zx\n", res_idx,
			&wdi_res[res_idx].res->pa, wdi_res[res_idx].res->iova,
			wdi_res[res_idx].res->size);
}

static void ipa_save_uc_smmu_mapping_sgt(int res_idx, struct sg_table *sgt,
		unsigned long iova)
{
	int i;
	struct scatterlist *sg;
	unsigned long curr_iova = iova;

	wdi_res[res_idx].res = kcalloc(sgt->nents, sizeof(struct ipa_wdi_res),
			GFP_KERNEL);
	if (!wdi_res[res_idx].res)
		BUG();
	wdi_res[res_idx].nents = sgt->nents;
	wdi_res[res_idx].valid = true;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		wdi_res[res_idx].res[i].pa = page_to_phys(sg_page(sg));
		wdi_res[res_idx].res[i].iova = curr_iova;
		wdi_res[res_idx].res[i].size = PAGE_ALIGN(sg->offset +
				sg->length);
		IPADBG("res_idx=%d pa=0x%pa iova=0x%lx sz=0x%zx\n", res_idx,
			&wdi_res[res_idx].res[i].pa,
			wdi_res[res_idx].res[i].iova,
			wdi_res[res_idx].res[i].size);
		curr_iova += wdi_res[res_idx].res[i].size;
	}
}

static int ipa_create_uc_smmu_mapping(int res_idx, bool wlan_smmu_en,
		phys_addr_t pa, struct sg_table *sgt, size_t len, bool device,
		unsigned long *iova)
{
	/* support for SMMU on WLAN but no SMMU on IPA */
	if (wlan_smmu_en && !ipa_ctx->smmu_present) {
		IPAERR("Unsupported SMMU pairing\n");
		return -EINVAL;
	}

	/* legacy: no SMMUs on either end */
	if (!wlan_smmu_en && !ipa_ctx->smmu_present) {
		*iova = pa;
		return 0;
	}

	/* no SMMU on WLAN but SMMU on IPA */
	if (!wlan_smmu_en && ipa_ctx->smmu_present) {
		if (ipa_create_uc_smmu_mapping_pa(pa, len,
			(res_idx == IPA_WDI_CE_DB_RES) ? true : false, iova)) {
			IPAERR("Fail to create mapping res %d\n", res_idx);
			return -EFAULT;
		}
		ipa_save_uc_smmu_mapping_pa(res_idx, pa, *iova, len);
		return 0;
	}

	/* SMMU on WLAN and SMMU on IPA */
	if (wlan_smmu_en && ipa_ctx->smmu_present) {
		switch (res_idx) {
		case IPA_WDI_RX_RING_RP_RES:
		case IPA_WDI_CE_DB_RES:
			if (ipa_create_uc_smmu_mapping_pa(pa, len,
				(res_idx == IPA_WDI_CE_DB_RES) ? true : false,
				iova)) {
				IPAERR("Fail to create mapping res %d\n",
						res_idx);
				return -EFAULT;
			}
			ipa_save_uc_smmu_mapping_pa(res_idx, pa, *iova, len);
			break;
		case IPA_WDI_RX_RING_RES:
		case IPA_WDI_TX_RING_RES:
		case IPA_WDI_CE_RING_RES:
			if (ipa_create_uc_smmu_mapping_sgt(sgt, iova)) {
				IPAERR("Fail to create mapping res %d\n",
						res_idx);
				return -EFAULT;
			}
			ipa_save_uc_smmu_mapping_sgt(res_idx, sgt, *iova);
			break;
		default:
			BUG();
		}
	}

	return 0;
}

/**
 * ipa_connect_wdi_pipe() - WDI client connect
 * @in:	[in] input parameters from client
 * @out: [out] output params to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_connect_wdi_pipe(struct ipa_wdi_in_params *in,
		struct ipa_wdi_out_params *out)
{
	int ipa_ep_idx;
	int result = -EFAULT;
	struct ipa_ep_context *ep;
	struct ipa_mem_buffer cmd;
	struct IpaHwWdiTxSetUpCmdData_t *tx;
	struct IpaHwWdiRxSetUpCmdData_t *rx;
	struct ipa_ep_cfg_ctrl ep_cfg_ctrl;
	unsigned long va;
	phys_addr_t pa;
	u32 len;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (in == NULL || out == NULL || in->sys.client >= IPA_CLIENT_MAX) {
		IPAERR("bad parm. in=%p out=%p\n", in, out);
		if (in)
			IPAERR("client = %d\n", in->sys.client);
		return -EINVAL;
	}

	if (IPA_CLIENT_IS_CONS(in->sys.client)) {
		if (in->u.dl.comp_ring_base_pa % IPA_WDI_RING_ALIGNMENT ||
			in->u.dl.ce_ring_base_pa % IPA_WDI_RING_ALIGNMENT) {
			IPAERR("alignment failure on TX\n");
			return -EINVAL;
		}
	} else {
		if (in->u.ul.rdy_ring_base_pa % IPA_WDI_RING_ALIGNMENT) {
			IPAERR("alignment failure on RX\n");
			return -EINVAL;
		}
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	ipa_ep_idx = ipa_get_ep_mapping(in->sys.client);
	if (ipa_ep_idx == -1) {
		IPAERR("fail to alloc EP.\n");
		goto fail;
	}

	ep = &ipa_ctx->ep[ipa_ep_idx];

	if (ep->valid) {
		IPAERR("EP already allocated.\n");
		goto fail;
	}

	memset(&ipa_ctx->ep[ipa_ep_idx], 0, sizeof(struct ipa_ep_context));
	ipa_inc_client_enable_clks();

	IPADBG("client=%d ep=%d\n", in->sys.client, ipa_ep_idx);
	if (IPA_CLIENT_IS_CONS(in->sys.client)) {
		cmd.size = sizeof(*tx);
		IPADBG("comp_ring_base_pa=0x%pa\n",
				&in->u.dl.comp_ring_base_pa);
		IPADBG("comp_ring_size=%d\n", in->u.dl.comp_ring_size);
		IPADBG("ce_ring_base_pa=0x%pa\n", &in->u.dl.ce_ring_base_pa);
		IPADBG("ce_ring_size=%d\n", in->u.dl.ce_ring_size);
		IPADBG("ce_ring_doorbell_pa=0x%pa\n",
				&in->u.dl.ce_door_bell_pa);
		IPADBG("num_tx_buffers=%d\n", in->u.dl.num_tx_buffers);
	} else {
		cmd.size = sizeof(*rx);
		IPADBG("rx_ring_base_pa=0x%pa\n", &in->u.ul.rdy_ring_base_pa);
		IPADBG("rx_ring_size=%d\n", in->u.ul.rdy_ring_size);
		IPADBG("rx_ring_rp_pa=0x%pa\n", &in->u.ul.rdy_ring_rp_pa);
	}

	cmd.base = dma_alloc_coherent(ipa_ctx->uc_pdev, cmd.size,
			&cmd.phys_base, GFP_KERNEL);
	if (cmd.base == NULL) {
		IPAERR("fail to get DMA memory.\n");
		result = -ENOMEM;
		goto dma_alloc_fail;
	}

	if (IPA_CLIENT_IS_CONS(in->sys.client)) {
		tx = (struct IpaHwWdiTxSetUpCmdData_t *)cmd.base;

		len = in->smmu_enabled ? in->u.dl_smmu.comp_ring_size :
			in->u.dl.comp_ring_size;
		IPADBG("TX ring smmu_en=%d ring_size=%d %d\n", in->smmu_enabled,
				in->u.dl_smmu.comp_ring_size,
				in->u.dl.comp_ring_size);
		if (ipa_create_uc_smmu_mapping(IPA_WDI_TX_RING_RES,
					in->smmu_enabled,
					in->u.dl.comp_ring_base_pa,
					&in->u.dl_smmu.comp_ring,
					len,
					false,
					&va)) {
				IPAERR("fail to create uc mapping TX ring.\n");
				result = -ENOMEM;
				goto uc_timeout;
		}
		tx->comp_ring_base_pa = va;
		tx->comp_ring_size = len;

		len = in->smmu_enabled ? in->u.dl_smmu.ce_ring_size :
			in->u.dl.ce_ring_size;
		IPADBG("TX CE ring smmu_en=%d ring_size=%d %d\n",
				in->smmu_enabled,
				in->u.dl_smmu.ce_ring_size,
				in->u.dl.ce_ring_size);
		if (ipa_create_uc_smmu_mapping(IPA_WDI_CE_RING_RES,
					in->smmu_enabled,
					in->u.dl.ce_ring_base_pa,
					&in->u.dl_smmu.ce_ring,
					len,
					false,
					&va)) {
				IPAERR("fail to create uc mapping CE ring.\n");
				result = -ENOMEM;
				goto uc_timeout;
		}
		tx->ce_ring_base_pa = va;
		tx->ce_ring_size = len;

		pa = in->smmu_enabled ? in->u.dl_smmu.ce_door_bell_pa :
			in->u.dl.ce_door_bell_pa;
		if (ipa_create_uc_smmu_mapping(IPA_WDI_CE_DB_RES,
					in->smmu_enabled,
					pa,
					NULL,
					4,
					true,
					&va)) {
				IPAERR("fail to create uc mapping CE DB.\n");
				result = -ENOMEM;
				goto uc_timeout;
		}
		tx->ce_ring_doorbell_pa = va;

		tx->num_tx_buffers = in->u.dl.num_tx_buffers;
		tx->ipa_pipe_number = ipa_ep_idx;
		if (ipa_ctx->ipa_hw_type >= IPA_HW_v2_5) {
				out->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
				   IPA_REG_BASE_OFST_v2_5 +
				   IPA_UC_MAILBOX_m_n_OFFS_v2_5(
				    IPA_HW_WDI_TX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_TX_MBOX_START_INDEX % 32);
		} else {
				out->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
				   IPA_REG_BASE_OFST_v2_0 +
				   IPA_UC_MAILBOX_m_n_OFFS(
				    IPA_HW_WDI_TX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_TX_MBOX_START_INDEX % 32);
		}
	} else {
		rx = (struct IpaHwWdiRxSetUpCmdData_t *)cmd.base;

		len = in->smmu_enabled ? in->u.ul_smmu.rdy_ring_size :
			in->u.ul.rdy_ring_size;
		IPADBG("RX ring smmu_en=%d ring_size=%d %d\n", in->smmu_enabled,
				in->u.ul_smmu.rdy_ring_size,
				in->u.ul.rdy_ring_size);
		if (ipa_create_uc_smmu_mapping(IPA_WDI_RX_RING_RES,
					in->smmu_enabled,
					in->u.ul.rdy_ring_base_pa,
					&in->u.ul_smmu.rdy_ring,
					len,
					false,
					&va)) {
				IPAERR("fail to create uc mapping RX ring.\n");
				result = -ENOMEM;
				goto uc_timeout;
		}
		rx->rx_ring_base_pa = va;
		rx->rx_ring_size = len;

		pa = in->smmu_enabled ? in->u.ul_smmu.rdy_ring_rp_pa :
			in->u.ul.rdy_ring_rp_pa;
		if (ipa_create_uc_smmu_mapping(IPA_WDI_RX_RING_RP_RES,
					in->smmu_enabled,
					pa,
					NULL,
					4,
					false,
					&va)) {
				IPAERR("fail to create uc mapping RX rng RP\n");
				result = -ENOMEM;
				goto uc_timeout;
		}
		rx->rx_ring_rp_pa = va;

		rx->ipa_pipe_number = ipa_ep_idx;
		if (ipa_ctx->ipa_hw_type >= IPA_HW_v2_5) {
				out->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
				   IPA_REG_BASE_OFST_v2_5 +
				   IPA_UC_MAILBOX_m_n_OFFS_v2_5(
				    IPA_HW_WDI_RX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_RX_MBOX_START_INDEX % 32);
		} else {
				out->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
				   IPA_REG_BASE_OFST_v2_0 +
				   IPA_UC_MAILBOX_m_n_OFFS(
				    IPA_HW_WDI_RX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_RX_MBOX_START_INDEX % 32);
		}
	}

	ep->valid = 1;
	ep->client = in->sys.client;
	ep->keep_ipa_awake = in->sys.keep_ipa_awake;
	result = ipa_disable_data_path(ipa_ep_idx);
	if (result) {
		IPAERR("disable data path failed res=%d clnt=%d.\n", result,
			ipa_ep_idx);
		goto uc_timeout;
	}
	if (IPA_CLIENT_IS_PROD(in->sys.client)) {
		memset(&ep_cfg_ctrl, 0 , sizeof(struct ipa_ep_cfg_ctrl));
		ep_cfg_ctrl.ipa_ep_delay = true;
		ipa_cfg_ep_ctrl(ipa_ep_idx, &ep_cfg_ctrl);
	}

	result = ipa_uc_send_cmd((u32)(cmd.phys_base),
				IPA_CLIENT_IS_CONS(in->sys.client) ?
				IPA_CPU_2_HW_CMD_WDI_TX_SET_UP :
				IPA_CPU_2_HW_CMD_WDI_RX_SET_UP,
				IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
				false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	ep->skip_ep_cfg = in->sys.skip_ep_cfg;
	ep->client_notify = in->sys.notify;
	ep->priv = in->sys.priv;

	if (!ep->skip_ep_cfg) {
		if (ipa_cfg_ep(ipa_ep_idx, &in->sys.ipa_ep_cfg)) {
			IPAERR("fail to configure EP.\n");
			goto ipa_cfg_ep_fail;
		}
		IPADBG("ep configuration successful\n");
	} else {
		IPADBG("Skipping endpoint configuration.\n");
	}

	out->clnt_hdl = ipa_ep_idx;

	if (!ep->skip_ep_cfg && IPA_CLIENT_IS_PROD(in->sys.client))
		ipa_install_dflt_flt_rules(ipa_ep_idx);

	if (!ep->keep_ipa_awake)
		ipa_dec_client_disable_clks();

	dma_free_coherent(ipa_ctx->uc_pdev, cmd.size, cmd.base, cmd.phys_base);
	ep->wdi_state |= IPA_WDI_CONNECTED;
	IPADBG("client %d (ep: %d) connected\n", in->sys.client, ipa_ep_idx);

	return 0;

ipa_cfg_ep_fail:
	memset(&ipa_ctx->ep[ipa_ep_idx], 0, sizeof(struct ipa_ep_context));
uc_timeout:
	ipa_release_uc_smmu_mappings(in->sys.client);
	dma_free_coherent(ipa_ctx->uc_pdev, cmd.size, cmd.base, cmd.phys_base);
dma_alloc_fail:
	ipa_dec_client_disable_clks();
fail:
	return result;
}
EXPORT_SYMBOL(ipa_connect_wdi_pipe);


/**
 * ipa_disconnect_wdi_pipe() - WDI client disconnect
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_disconnect_wdi_pipe(u32 clnt_hdl)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiCommonChCmdData_t tear;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (ep->wdi_state != IPA_WDI_CONNECTED) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	if (!ep->keep_ipa_awake)
		ipa_inc_client_enable_clks();

	tear.params.ipa_pipe_number = clnt_hdl;

	result = ipa_uc_send_cmd(tear.raw32b,
				IPA_CPU_2_HW_CMD_WDI_TEAR_DOWN,
				IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
				false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	ipa_delete_dflt_flt_rules(clnt_hdl);
	ipa_release_uc_smmu_mappings(ep->client);

	memset(&ipa_ctx->ep[clnt_hdl], 0, sizeof(struct ipa_ep_context));
	ipa_dec_client_disable_clks();

	IPADBG("client (ep: %d) disconnected\n", clnt_hdl);

uc_timeout:
	return result;
}
EXPORT_SYMBOL(ipa_disconnect_wdi_pipe);

/**
 * ipa_enable_wdi_pipe() - WDI client enable
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_enable_wdi_pipe(u32 clnt_hdl)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiCommonChCmdData_t enable;
	struct ipa_ep_cfg_holb holb_cfg;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (ep->wdi_state != IPA_WDI_CONNECTED) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	ipa_inc_client_enable_clks();
	enable.params.ipa_pipe_number = clnt_hdl;

	result = ipa_uc_send_cmd(enable.raw32b,
		IPA_CPU_2_HW_CMD_WDI_CH_ENABLE,
		IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
		false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	if (IPA_CLIENT_IS_CONS(ep->client)) {
		memset(&holb_cfg, 0 , sizeof(holb_cfg));
		holb_cfg.en = IPA_HOLB_TMR_DIS;
		holb_cfg.tmr_val = 0;
		result = ipa_cfg_ep_holb(clnt_hdl, &holb_cfg);
	}

	ipa_dec_client_disable_clks();
	ep->wdi_state |= IPA_WDI_ENABLED;
	IPADBG("client (ep: %d) enabled\n", clnt_hdl);

uc_timeout:
	return result;
}
EXPORT_SYMBOL(ipa_enable_wdi_pipe);

/**
 * ipa_disable_wdi_pipe() - WDI client disable
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_disable_wdi_pipe(u32 clnt_hdl)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiCommonChCmdData_t disable;
	struct ipa_ep_cfg_ctrl ep_cfg_ctrl;
	u32 prod_hdl;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (ep->wdi_state != (IPA_WDI_CONNECTED | IPA_WDI_ENABLED)) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	ipa_inc_client_enable_clks();

	result = ipa_disable_data_path(clnt_hdl);
	if (result) {
		IPAERR("disable data path failed res=%d clnt=%d.\n", result,
			clnt_hdl);
		result = -EPERM;
		goto uc_timeout;
	}

	/**
	 * To avoid data stall during continuous SAP on/off before
	 * setting delay to IPA Consumer pipe, remove delay and enable
	 * holb on IPA Producer pipe
	 */
	if (IPA_CLIENT_IS_PROD(ep->client)) {
		memset(&ep_cfg_ctrl, 0 , sizeof(struct ipa_ep_cfg_ctrl));
		ipa_cfg_ep_ctrl(clnt_hdl, &ep_cfg_ctrl);

		prod_hdl = ipa_get_ep_mapping(IPA_CLIENT_WLAN1_CONS);
		if (ipa_ctx->ep[prod_hdl].valid == 1) {
			result = ipa_disable_data_path(prod_hdl);
			if (result) {
				IPAERR("disable data path failed\n");
				IPAERR("res=%d clnt=%d\n",
					result, prod_hdl);
				result = -EPERM;
				goto uc_timeout;
			}
		}
		usleep_range(IPA_UC_POLL_SLEEP_USEC * IPA_UC_POLL_SLEEP_USEC,
			IPA_UC_POLL_SLEEP_USEC * IPA_UC_POLL_SLEEP_USEC);
	}

	disable.params.ipa_pipe_number = clnt_hdl;

	result = ipa_uc_send_cmd(disable.raw32b,
		IPA_CPU_2_HW_CMD_WDI_CH_DISABLE,
		IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
		false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	/* Set the delay after disabling IPA Producer pipe */
	if (IPA_CLIENT_IS_PROD(ep->client)) {
		memset(&ep_cfg_ctrl, 0, sizeof(struct ipa_ep_cfg_ctrl));
		ep_cfg_ctrl.ipa_ep_delay = true;
		ipa_cfg_ep_ctrl(clnt_hdl, &ep_cfg_ctrl);
	}

	ipa_dec_client_disable_clks();
	ep->wdi_state &= ~IPA_WDI_ENABLED;
	IPADBG("client (ep: %d) disabled\n", clnt_hdl);

uc_timeout:
	return result;
}
EXPORT_SYMBOL(ipa_disable_wdi_pipe);

/**
 * ipa_resume_wdi_pipe() - WDI client resume
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_resume_wdi_pipe(u32 clnt_hdl)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiCommonChCmdData_t resume;
	struct ipa_ep_cfg_ctrl ep_cfg_ctrl;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (ep->wdi_state != (IPA_WDI_CONNECTED | IPA_WDI_ENABLED)) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	ipa_inc_client_enable_clks();
	resume.params.ipa_pipe_number = clnt_hdl;

	result = ipa_uc_send_cmd(resume.raw32b,
		IPA_CPU_2_HW_CMD_WDI_CH_RESUME,
		IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
		false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	memset(&ep_cfg_ctrl, 0 , sizeof(struct ipa_ep_cfg_ctrl));
	result = ipa_cfg_ep_ctrl(clnt_hdl, &ep_cfg_ctrl);
	if (result)
		IPAERR("client (ep: %d) fail un-susp/delay result=%d\n",
				clnt_hdl, result);
	else
		IPADBG("client (ep: %d) un-susp/delay\n", clnt_hdl);

	ep->wdi_state |= IPA_WDI_RESUMED;
	IPADBG("client (ep: %d) resumed\n", clnt_hdl);

uc_timeout:
	return result;
}
EXPORT_SYMBOL(ipa_resume_wdi_pipe);

/**
 * ipa_suspend_wdi_pipe() - WDI client suspend
 * @clnt_hdl:	[in] opaque client handle assigned by IPA to client
 *
 * Returns:	0 on success, negative on failure
 *
 * Note:	Should not be called from atomic context
 */
int ipa_suspend_wdi_pipe(u32 clnt_hdl)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiCommonChCmdData_t suspend;
	struct ipa_ep_cfg_ctrl ep_cfg_ctrl;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (ep->wdi_state != (IPA_WDI_CONNECTED | IPA_WDI_ENABLED |
				IPA_WDI_RESUMED)) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	suspend.params.ipa_pipe_number = clnt_hdl;

	if (IPA_CLIENT_IS_PROD(ep->client)) {
		IPADBG("Post suspend event first for IPA Producer\n");
		IPADBG("Client: %d clnt_hdl: %d\n", ep->client, clnt_hdl);
		result = ipa_uc_send_cmd(suspend.raw32b,
			IPA_CPU_2_HW_CMD_WDI_CH_SUSPEND,
			IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
			false, 10*HZ);

		if (result) {
			result = -EFAULT;
			goto uc_timeout;
		}
	}

	memset(&ep_cfg_ctrl, 0 , sizeof(struct ipa_ep_cfg_ctrl));
	if (IPA_CLIENT_IS_CONS(ep->client)) {
		ep_cfg_ctrl.ipa_ep_suspend = true;
		result = ipa_cfg_ep_ctrl(clnt_hdl, &ep_cfg_ctrl);
		if (result)
			IPAERR("client (ep: %d) failed to suspend result=%d\n",
					clnt_hdl, result);
		else
			IPADBG("client (ep: %d) suspended\n", clnt_hdl);
	} else {
		ep_cfg_ctrl.ipa_ep_delay = true;
		result = ipa_cfg_ep_ctrl(clnt_hdl, &ep_cfg_ctrl);
		if (result)
			IPAERR("client (ep: %d) failed to delay result=%d\n",
					clnt_hdl, result);
		else
			IPADBG("client (ep: %d) delayed\n", clnt_hdl);
	}

	if (IPA_CLIENT_IS_CONS(ep->client)) {
		result = ipa_uc_send_cmd(suspend.raw32b,
			IPA_CPU_2_HW_CMD_WDI_CH_SUSPEND,
			IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
			false, 10*HZ);

		if (result) {
			result = -EFAULT;
			goto uc_timeout;
		}
	}

	ipa_ctx->tag_process_before_gating = true;
	ipa_dec_client_disable_clks();
	ep->wdi_state &= ~IPA_WDI_RESUMED;
	IPADBG("client (ep: %d) suspended\n", clnt_hdl);

uc_timeout:
	return result;
}
EXPORT_SYMBOL(ipa_suspend_wdi_pipe);

int ipa_write_qmapid_wdi_pipe(u32 clnt_hdl, u8 qmap_id)
{
	int result = 0;
	struct ipa_ep_context *ep;
	union IpaHwWdiRxExtCfgCmdData_t qmap;

	if (clnt_hdl >= ipa_ctx->ipa_num_pipes ||
	    ipa_ctx->ep[clnt_hdl].valid == 0) {
		IPAERR("bad parm.\n");
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result)
		return result;

	IPADBG("ep=%d\n", clnt_hdl);

	ep = &ipa_ctx->ep[clnt_hdl];

	if (!(ep->wdi_state & IPA_WDI_CONNECTED)) {
		IPAERR("WDI channel bad state %d\n", ep->wdi_state);
		return -EFAULT;
	}

	ipa_inc_client_enable_clks();
	qmap.params.ipa_pipe_number = clnt_hdl;
	qmap.params.qmap_id = qmap_id;

	result = ipa_uc_send_cmd(qmap.raw32b,
		IPA_CPU_2_HW_CMD_WDI_RX_EXT_CFG,
		IPA_HW_2_CPU_WDI_CMD_STATUS_SUCCESS,
		false, 10*HZ);

	if (result) {
		result = -EFAULT;
		goto uc_timeout;
	}

	ipa_dec_client_disable_clks();

	IPADBG("client (ep: %d) qmap_id %d updated\n", clnt_hdl, qmap_id);

uc_timeout:
	return result;
}

/**
 * ipa_uc_reg_rdyCB() - To register uC
 * ready CB if uC not ready
 * @inout:	[in/out] input/ouput parameters
 * from/to client
 *
 * Returns:	0 on success, negative on failure
 *
 */
int ipa_uc_reg_rdyCB(
	struct ipa_wdi_uc_ready_params *inout)
{
	int result = 0;

	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (inout == NULL) {
		IPAERR("bad parm. inout=%p ", inout);
		return -EINVAL;
	}

	result = ipa_uc_state_check();
	if (result) {
		inout->is_uC_ready = false;
		ipa_ctx->uc_wdi_ctx.uc_ready_cb = inout->notify;
		ipa_ctx->uc_wdi_ctx.priv = inout->priv;
	} else {
		inout->is_uC_ready = true;
	}

	return 0;
}
EXPORT_SYMBOL(ipa_uc_reg_rdyCB);


/**
 * ipa_uc_wdi_get_dbpa() - To retrieve
 * doorbell physical address of wlan pipes
 * @param:  [in/out] input/ouput parameters
 *          from/to client
 *
 * Returns:	0 on success, negative on failure
 *
 */
int ipa_uc_wdi_get_dbpa(
	struct ipa_wdi_db_params *param)
{
	if (unlikely(!ipa_ctx)) {
		IPAERR("IPA driver was not initialized\n");
		return -EINVAL;
	}

	if (param == NULL || param->client >= IPA_CLIENT_MAX) {
		IPAERR("bad parm. param=%p ", param);
		if (param)
			IPAERR("client = %d\n", param->client);
		return -EINVAL;
	}

	if (IPA_CLIENT_IS_CONS(param->client)) {
		if (ipa_ctx->ipa_hw_type >= IPA_HW_v2_5) {
				param->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
					IPA_REG_BASE_OFST_v2_5 +
				   IPA_UC_MAILBOX_m_n_OFFS_v2_5(
				    IPA_HW_WDI_TX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_TX_MBOX_START_INDEX % 32);
		} else {
				param->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
					IPA_REG_BASE_OFST_v2_0 +
				   IPA_UC_MAILBOX_m_n_OFFS(
				    IPA_HW_WDI_TX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_TX_MBOX_START_INDEX % 32);
		}
	} else {
		if (ipa_ctx->ipa_hw_type >= IPA_HW_v2_5) {
				param->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
					IPA_REG_BASE_OFST_v2_5 +
				   IPA_UC_MAILBOX_m_n_OFFS_v2_5(
				    IPA_HW_WDI_RX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_RX_MBOX_START_INDEX % 32);
		} else {
				param->uc_door_bell_pa =
				 ipa_ctx->ipa_wrapper_base +
					IPA_REG_BASE_OFST_v2_0 +
				   IPA_UC_MAILBOX_m_n_OFFS(
				    IPA_HW_WDI_RX_MBOX_START_INDEX/32,
				    IPA_HW_WDI_RX_MBOX_START_INDEX % 32);
		}
	}

	return 0;
}
EXPORT_SYMBOL(ipa_uc_wdi_get_dbpa);

static void ipa_uc_wdi_loaded_handler(void)
{
	if (!ipa_ctx) {
		IPAERR("IPA ctx is null\n");
		return;
	}

	if (ipa_ctx->uc_wdi_ctx.uc_ready_cb)
		ipa_ctx->uc_wdi_ctx.uc_ready_cb(
			ipa_ctx->uc_wdi_ctx.priv);

	return;
}

int ipa_create_wdi_mapping(u32 num_buffers, struct ipa_wdi_buffer_info *info)
{
	struct ipa_smmu_cb_ctx *cb = ipa_get_wlan_smmu_ctx();
	int i;
	int ret = 0;
	int prot = IOMMU_READ | IOMMU_WRITE;

	if (!info) {
		IPAERR("info = %p\n", info);
		return -EINVAL;
	}

	if (!cb->valid) {
		IPAERR("No SMMU CB setup\n");
		return -EINVAL;
	}

	for (i = 0; i < num_buffers; i++) {
		IPADBG("i=%d pa=0x%pa iova=0x%lx sz=0x%zx\n", i,
			&info[i].pa, info[i].iova, info[i].size);
		info[i].result = iommu_map(cb->iommu,
			rounddown(info[i].iova, PAGE_SIZE),
			rounddown(info[i].pa, PAGE_SIZE),
			roundup(info[i].size + info[i].pa -
				rounddown(info[i].pa, PAGE_SIZE), PAGE_SIZE),
			prot);
	}

	return ret;
}
EXPORT_SYMBOL(ipa_create_wdi_mapping);

int ipa_release_wdi_mapping(u32 num_buffers, struct ipa_wdi_buffer_info *info)
{
	struct ipa_smmu_cb_ctx *cb = ipa_get_wlan_smmu_ctx();
	int i;
	int ret = 0;

	if (!info) {
		IPAERR("info = %p\n", info);
		return -EINVAL;
	}

	if (!cb->valid) {
		IPAERR("No SMMU CB setup\n");
		return -EINVAL;
	}

	for (i = 0; i < num_buffers; i++) {
		IPADBG("i=%d pa=0x%pa iova=0x%lx sz=0x%zx\n", i,
			&info[i].pa, info[i].iova, info[i].size);
		info[i].result = iommu_unmap(cb->iommu,
			rounddown(info[i].iova, PAGE_SIZE),
			roundup(info[i].size + info[i].pa -
				rounddown(info[i].pa, PAGE_SIZE), PAGE_SIZE));
	}

	return ret;
}
EXPORT_SYMBOL(ipa_release_wdi_mapping);
