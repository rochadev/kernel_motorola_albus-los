/*
 * Abilis Systems Single DVB-T Receiver
 * Copyright (C) 2008 Pierrick Hascoet <pierrick.hascoet@abilis.com>
 * Copyright (C) 2010 Devin Heitmueller <dheitmueller@kernellabs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#if defined(LINUX) && defined(__KERNEL__) /* linux kernel implementation */
#include <linux/kernel.h>
#include "as102_drv.h"
#elif defined(WIN32)
   #if defined(__BUILDMACHINE__) && (__BUILDMACHINE__ == WinDDK)
      /* win32 ddk implementation */
      #include "wdm.h"
      #include "Device.h"
      #include "endian_mgmt.h" /* FIXME */
   #else /* win32 sdk implementation */
      #include <windows.h>
      #include "types.h"
      #include "util.h"
      #include "as10x_handle.h"
      #include "endian_mgmt.h"
   #endif
#else /* all other cases */
   #include <string.h>
   #include "types.h"
   #include "util.h"
   #include "as10x_handle.h"
   #include "endian_mgmt.h" /* FIXME */
#endif /* __KERNEL__ */

#include "as10x_types.h"
#include "as10x_cmd.h"

/**
   \brief  send turn on command to AS10x
   \param  phandle:   pointer to AS10x handle
   \return 0 when no error, < 0 in case of error.
  \callgraph
*/
int as10x_cmd_turn_on(as10x_handle_t *phandle)
{
	int error;
	struct as10x_cmd_t *pcmd, *prsp;

	ENTER();

	pcmd = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(pcmd, (++phandle->cmd_xid),
			sizeof(pcmd->body.turn_on.req));

	/* fill command */
	pcmd->body.turn_on.req.proc_id = cpu_to_le16(CONTROL_PROC_TURNON);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(phandle, (uint8_t *) pcmd,
					       sizeof(pcmd->body.turn_on.req) +
					       HEADER_SIZE,
					       (uint8_t *) prsp,
					       sizeof(prsp->body.turn_on.rsp) +
					       HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_TURNON_RSP);

out:
	LEAVE();
	return error;
}

/**
   \brief  send turn off command to AS10x
   \param  phandle:   pointer to AS10x handle
   \return 0 when no error, < 0 in case of error.
   \callgraph
*/
int as10x_cmd_turn_off(as10x_handle_t *phandle)
{
	int error;
	struct as10x_cmd_t *pcmd, *prsp;

	ENTER();

	pcmd = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(pcmd, (++phandle->cmd_xid),
			sizeof(pcmd->body.turn_off.req));

	/* fill command */
	pcmd->body.turn_off.req.proc_id = cpu_to_le16(CONTROL_PROC_TURNOFF);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(
			phandle, (uint8_t *) pcmd,
			sizeof(pcmd->body.turn_off.req) + HEADER_SIZE,
			(uint8_t *) prsp,
			sizeof(prsp->body.turn_off.rsp) + HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_TURNOFF_RSP);

out:
	LEAVE();
	return error;
}

/**
   \brief  send set tune command to AS10x
   \param  phandle: pointer to AS10x handle
   \param  ptune:   tune parameters
   \return 0 when no error, < 0 in case of error.
   \callgraph
 */
int as10x_cmd_set_tune(as10x_handle_t *phandle, struct as10x_tune_args *ptune)
{
	int error;
	struct as10x_cmd_t *preq, *prsp;

	ENTER();

	preq = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(preq, (++phandle->cmd_xid),
			sizeof(preq->body.set_tune.req));

	/* fill command */
	preq->body.set_tune.req.proc_id = cpu_to_le16(CONTROL_PROC_SETTUNE);
	preq->body.set_tune.req.args.freq = cpu_to_le32(ptune->freq);
	preq->body.set_tune.req.args.bandwidth = ptune->bandwidth;
	preq->body.set_tune.req.args.hier_select = ptune->hier_select;
	preq->body.set_tune.req.args.constellation = ptune->constellation;
	preq->body.set_tune.req.args.hierarchy = ptune->hierarchy;
	preq->body.set_tune.req.args.interleaving_mode  =
		ptune->interleaving_mode;
	preq->body.set_tune.req.args.code_rate  = ptune->code_rate;
	preq->body.set_tune.req.args.guard_interval = ptune->guard_interval;
	preq->body.set_tune.req.args.transmission_mode  =
		ptune->transmission_mode;

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(phandle,
					       (uint8_t *) preq,
					       sizeof(preq->body.set_tune.req)
					       + HEADER_SIZE,
					       (uint8_t *) prsp,
					       sizeof(prsp->body.set_tune.rsp)
					       + HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_SETTUNE_RSP);

out:
	LEAVE();
	return error;
}

/**
   \brief  send get tune status command to AS10x
   \param  phandle:   pointer to AS10x handle
   \param  pstatus:   pointer to updated status structure of the current tune
   \return 0 when no error, < 0 in case of error.
   \callgraph
 */
int as10x_cmd_get_tune_status(as10x_handle_t *phandle,
			      struct as10x_tune_status *pstatus)
{
	int error;
	struct as10x_cmd_t  *preq, *prsp;

	ENTER();

	preq = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(preq, (++phandle->cmd_xid),
			sizeof(preq->body.get_tune_status.req));

	/* fill command */
	preq->body.get_tune_status.req.proc_id =
		cpu_to_le16(CONTROL_PROC_GETTUNESTAT);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(
			phandle,
			(uint8_t *) preq,
			sizeof(preq->body.get_tune_status.req) + HEADER_SIZE,
			(uint8_t *) prsp,
			sizeof(prsp->body.get_tune_status.rsp) + HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_GETTUNESTAT_RSP);
	if (error < 0)
		goto out;

	/* Response OK -> get response data */
	pstatus->tune_state = prsp->body.get_tune_status.rsp.sts.tune_state;
	pstatus->signal_strength  =
		le16_to_cpu(prsp->body.get_tune_status.rsp.sts.signal_strength);
	pstatus->PER = le16_to_cpu(prsp->body.get_tune_status.rsp.sts.PER);
	pstatus->BER = le16_to_cpu(prsp->body.get_tune_status.rsp.sts.BER);

out:
	LEAVE();
	return error;
}

/**
   \brief  send get TPS command to AS10x
   \param  phandle:   pointer to AS10x handle
   \param  ptps:      pointer to TPS parameters structure
   \return 0 when no error, < 0 in case of error.
   \callgraph
 */
int as10x_cmd_get_tps(as10x_handle_t *phandle, struct as10x_tps *ptps)
{
	int error;
	struct as10x_cmd_t *pcmd, *prsp;

	ENTER();

	pcmd = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(pcmd, (++phandle->cmd_xid),
			sizeof(pcmd->body.get_tps.req));

	/* fill command */
	pcmd->body.get_tune_status.req.proc_id =
		cpu_to_le16(CONTROL_PROC_GETTPS);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(phandle,
					       (uint8_t *) pcmd,
					       sizeof(pcmd->body.get_tps.req) +
					       HEADER_SIZE,
					       (uint8_t *) prsp,
					       sizeof(prsp->body.get_tps.rsp) +
					       HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_GETTPS_RSP);
	if (error < 0)
		goto out;

	/* Response OK -> get response data */
	ptps->constellation = prsp->body.get_tps.rsp.tps.constellation;
	ptps->hierarchy = prsp->body.get_tps.rsp.tps.hierarchy;
	ptps->interleaving_mode = prsp->body.get_tps.rsp.tps.interleaving_mode;
	ptps->code_rate_HP = prsp->body.get_tps.rsp.tps.code_rate_HP;
	ptps->code_rate_LP = prsp->body.get_tps.rsp.tps.code_rate_LP;
	ptps->guard_interval = prsp->body.get_tps.rsp.tps.guard_interval;
	ptps->transmission_mode  = prsp->body.get_tps.rsp.tps.transmission_mode;
	ptps->DVBH_mask_HP = prsp->body.get_tps.rsp.tps.DVBH_mask_HP;
	ptps->DVBH_mask_LP = prsp->body.get_tps.rsp.tps.DVBH_mask_LP;
	ptps->cell_ID = le16_to_cpu(prsp->body.get_tps.rsp.tps.cell_ID);

out:
	LEAVE();
	return error;
}

/**
   \brief  send get demod stats command to AS10x
   \param  phandle:       pointer to AS10x handle
   \param  pdemod_stats:  pointer to demod stats parameters structure
   \return 0 when no error, < 0 in case of error.
   \callgraph
*/
int as10x_cmd_get_demod_stats(as10x_handle_t  *phandle,
			      struct as10x_demod_stats *pdemod_stats)
{
	int error;
	struct as10x_cmd_t *pcmd, *prsp;

	ENTER();

	pcmd = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(pcmd, (++phandle->cmd_xid),
			sizeof(pcmd->body.get_demod_stats.req));

	/* fill command */
	pcmd->body.get_demod_stats.req.proc_id =
		cpu_to_le16(CONTROL_PROC_GET_DEMOD_STATS);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(phandle,
				(uint8_t *) pcmd,
				sizeof(pcmd->body.get_demod_stats.req)
				+ HEADER_SIZE,
				(uint8_t *) prsp,
				sizeof(prsp->body.get_demod_stats.rsp)
				+ HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_GET_DEMOD_STATS_RSP);
	if (error < 0)
		goto out;

	/* Response OK -> get response data */
	pdemod_stats->frame_count =
		le32_to_cpu(prsp->body.get_demod_stats.rsp.stats.frame_count);
	pdemod_stats->bad_frame_count =
		le32_to_cpu(prsp->body.get_demod_stats.rsp.stats.bad_frame_count);
	pdemod_stats->bytes_fixed_by_rs =
		le32_to_cpu(prsp->body.get_demod_stats.rsp.stats.bytes_fixed_by_rs);
	pdemod_stats->mer =
		le16_to_cpu(prsp->body.get_demod_stats.rsp.stats.mer);
	pdemod_stats->has_started =
		prsp->body.get_demod_stats.rsp.stats.has_started;

out:
	LEAVE();
	return error;
}

/**
   \brief  send get impulse response command to AS10x
   \param  phandle:        pointer to AS10x handle
   \param  is_ready:       pointer to value indicating when impulse
			   response data is ready
   \return 0 when no error, < 0 in case of error.
   \callgraph
*/
int as10x_cmd_get_impulse_resp(as10x_handle_t     *phandle,
			       uint8_t *is_ready)
{
	int error;
	struct as10x_cmd_t *pcmd, *prsp;

	ENTER();

	pcmd = phandle->cmd;
	prsp = phandle->rsp;

	/* prepare command */
	as10x_cmd_build(pcmd, (++phandle->cmd_xid),
			sizeof(pcmd->body.get_impulse_rsp.req));

	/* fill command */
	pcmd->body.get_impulse_rsp.req.proc_id =
		cpu_to_le16(CONTROL_PROC_GET_IMPULSE_RESP);

	/* send command */
	if (phandle->ops->xfer_cmd) {
		error = phandle->ops->xfer_cmd(phandle,
					(uint8_t *) pcmd,
					sizeof(pcmd->body.get_impulse_rsp.req)
					+ HEADER_SIZE,
					(uint8_t *) prsp,
					sizeof(prsp->body.get_impulse_rsp.rsp)
					+ HEADER_SIZE);
	} else {
		error = AS10X_CMD_ERROR;
	}

	if (error < 0)
		goto out;

	/* parse response */
	error = as10x_rsp_parse(prsp, CONTROL_PROC_GET_IMPULSE_RESP_RSP);
	if (error < 0)
		goto out;

	/* Response OK -> get response data */
	*is_ready = prsp->body.get_impulse_rsp.rsp.is_ready;

out:
	LEAVE();
	return error;
}



/**
   \brief  build AS10x command header
   \param  pcmd:     pointer to AS10x command buffer
   \param  xid:      sequence id of the command
   \param  cmd_len:  lenght of the command
   \return -
   \callgraph
*/
void as10x_cmd_build(struct as10x_cmd_t *pcmd,
		     uint16_t xid, uint16_t cmd_len)
{
	pcmd->header.req_id = cpu_to_le16(xid);
	pcmd->header.prog = cpu_to_le16(SERVICE_PROG_ID);
	pcmd->header.version = cpu_to_le16(SERVICE_PROG_VERSION);
	pcmd->header.data_len = cpu_to_le16(cmd_len);
}

/**
   \brief  Parse command response
   \param  pcmd:       pointer to AS10x command buffer
   \param  cmd_seqid:  sequence id of the command
   \param  cmd_len:    lenght of the command
   \return 0 when no error, < 0 in case of error
   \callgraph
*/
int as10x_rsp_parse(struct as10x_cmd_t *prsp, uint16_t proc_id)
{
	int error;

	/* extract command error code */
	error = prsp->body.common.rsp.error;

	if ((error == 0) &&
	    (le16_to_cpu(prsp->body.common.rsp.proc_id) == proc_id)) {
		return 0;
	}

	return AS10X_CMD_ERROR;
}


