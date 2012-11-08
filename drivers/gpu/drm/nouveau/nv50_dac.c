/*
 * Copyright (C) 2008 Maarten Maathuis.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>

#define NOUVEAU_DMA_DEBUG (nouveau_reg_debug & NOUVEAU_REG_DEBUG_EVO)
#include "nouveau_reg.h"
#include "nouveau_drm.h"
#include "nouveau_dma.h"
#include "nouveau_encoder.h"
#include "nouveau_connector.h"
#include "nouveau_crtc.h"
#include "nv50_display.h"

#include <core/class.h>

#include <subdev/timer.h>

static void
nv50_dac_disconnect(struct drm_encoder *encoder)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct drm_device *dev = encoder->dev;
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nouveau_channel *evo = nv50_display(dev)->master;
	int ret;

	if (!nv_encoder->crtc)
		return;
	nv50_crtc_blank(nouveau_crtc(nv_encoder->crtc), true);

	NV_DEBUG(drm, "Disconnecting DAC %d\n", nv_encoder->or);

	ret = RING_SPACE(evo, 4);
	if (ret) {
		NV_ERROR(drm, "no space while disconnecting DAC\n");
		return;
	}
	BEGIN_NV04(evo, 0, NV50_EVO_DAC(nv_encoder->or, MODE_CTRL), 1);
	OUT_RING  (evo, 0);
	BEGIN_NV04(evo, 0, NV50_EVO_UPDATE, 1);
	OUT_RING  (evo, 0);

	nv_encoder->crtc = NULL;
}

static enum drm_connector_status
nv50_dac_detect(struct drm_encoder *encoder, struct drm_connector *connector)
{
	struct nv50_display *priv = nv50_display(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	int or = nv_encoder->or, ret;
	u32 load;

	if (drm->vbios.dactestval)
		load = drm->vbios.dactestval;
	else
		load = 340;

	ret = nv_exec(priv->core, NV50_DISP_DAC_LOAD + or, &load, sizeof(load));
	if (ret || load != 7)
		return connector_status_disconnected;

	return connector_status_connected;
}

static void
nv50_dac_dpms(struct drm_encoder *encoder, int mode)
{
	struct nv50_display *priv = nv50_display(encoder->dev);
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	uint32_t val;
	int or = nv_encoder->or;

	NV_DEBUG(drm, "or %d mode %d\n", or, mode);

	if (mode != DRM_MODE_DPMS_ON)
		val = NV50_PDISPLAY_DAC_DPMS_CTRL_BLANKED;
	else
		val = 0;

	switch (mode) {
	case DRM_MODE_DPMS_STANDBY:
		val |= NV50_PDISPLAY_DAC_DPMS_CTRL_HSYNC_OFF;
		break;
	case DRM_MODE_DPMS_SUSPEND:
		val |= NV50_PDISPLAY_DAC_DPMS_CTRL_VSYNC_OFF;
		break;
	case DRM_MODE_DPMS_OFF:
		val |= NV50_PDISPLAY_DAC_DPMS_CTRL_OFF;
		val |= NV50_PDISPLAY_DAC_DPMS_CTRL_HSYNC_OFF;
		val |= NV50_PDISPLAY_DAC_DPMS_CTRL_VSYNC_OFF;
		break;
	default:
		break;
	}

	nv_call(priv->core, NV50_DISP_DAC_PWR + or, val);
}

static void
nv50_dac_save(struct drm_encoder *encoder)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	NV_ERROR(drm, "!!\n");
}

static void
nv50_dac_restore(struct drm_encoder *encoder)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	NV_ERROR(drm, "!!\n");
}

static bool
nv50_dac_mode_fixup(struct drm_encoder *encoder,
		    const struct drm_display_mode *mode,
		    struct drm_display_mode *adjusted_mode)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_connector *connector;

	NV_DEBUG(drm, "or %d\n", nv_encoder->or);

	connector = nouveau_encoder_connector_get(nv_encoder);
	if (!connector) {
		NV_ERROR(drm, "Encoder has no connector\n");
		return false;
	}

	if (connector->scaling_mode != DRM_MODE_SCALE_NONE &&
	     connector->native_mode)
		drm_mode_copy(adjusted_mode, connector->native_mode);

	return true;
}

static void
nv50_dac_commit(struct drm_encoder *encoder)
{
}

static void
nv50_dac_mode_set(struct drm_encoder *encoder, struct drm_display_mode *mode,
		  struct drm_display_mode *adjusted_mode)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct drm_device *dev = encoder->dev;
	struct nouveau_channel *evo = nv50_display(dev)->master;
	struct nouveau_crtc *crtc = nouveau_crtc(encoder->crtc);
	uint32_t mode_ctl = 0, mode_ctl2 = 0;
	int ret;

	NV_DEBUG(drm, "or %d type %d crtc %d\n",
		     nv_encoder->or, nv_encoder->dcb->type, crtc->index);

	nv50_dac_dpms(encoder, DRM_MODE_DPMS_ON);

	if (crtc->index == 1)
		mode_ctl |= NV50_EVO_DAC_MODE_CTRL_CRTC1;
	else
		mode_ctl |= NV50_EVO_DAC_MODE_CTRL_CRTC0;

	/* Lacking a working tv-out, this is not a 100% sure. */
	if (nv_encoder->dcb->type == DCB_OUTPUT_ANALOG)
		mode_ctl |= 0x40;
	else
	if (nv_encoder->dcb->type == DCB_OUTPUT_TV)
		mode_ctl |= 0x100;

	if (adjusted_mode->flags & DRM_MODE_FLAG_NHSYNC)
		mode_ctl2 |= NV50_EVO_DAC_MODE_CTRL2_NHSYNC;

	if (adjusted_mode->flags & DRM_MODE_FLAG_NVSYNC)
		mode_ctl2 |= NV50_EVO_DAC_MODE_CTRL2_NVSYNC;

	ret = RING_SPACE(evo, 3);
	if (ret) {
		NV_ERROR(drm, "no space while connecting DAC\n");
		return;
	}
	BEGIN_NV04(evo, 0, NV50_EVO_DAC(nv_encoder->or, MODE_CTRL), 2);
	OUT_RING(evo, mode_ctl);
	OUT_RING(evo, mode_ctl2);

	nv_encoder->crtc = encoder->crtc;
}

static struct drm_crtc *
nv50_dac_crtc_get(struct drm_encoder *encoder)
{
	return nouveau_encoder(encoder)->crtc;
}

static const struct drm_encoder_helper_funcs nv50_dac_helper_funcs = {
	.dpms = nv50_dac_dpms,
	.save = nv50_dac_save,
	.restore = nv50_dac_restore,
	.mode_fixup = nv50_dac_mode_fixup,
	.prepare = nv50_dac_disconnect,
	.commit = nv50_dac_commit,
	.mode_set = nv50_dac_mode_set,
	.get_crtc = nv50_dac_crtc_get,
	.detect = nv50_dac_detect,
	.disable = nv50_dac_disconnect
};

static void
nv50_dac_destroy(struct drm_encoder *encoder)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);

	if (!encoder)
		return;

	drm_encoder_cleanup(encoder);
	kfree(nv_encoder);
}

static const struct drm_encoder_funcs nv50_dac_encoder_funcs = {
	.destroy = nv50_dac_destroy,
};

int
nv50_dac_create(struct drm_connector *connector, struct dcb_output *entry)
{
	struct nouveau_encoder *nv_encoder;
	struct drm_encoder *encoder;

	nv_encoder = kzalloc(sizeof(*nv_encoder), GFP_KERNEL);
	if (!nv_encoder)
		return -ENOMEM;
	encoder = to_drm_encoder(nv_encoder);

	nv_encoder->dcb = entry;
	nv_encoder->or = ffs(entry->or) - 1;

	drm_encoder_init(connector->dev, encoder, &nv50_dac_encoder_funcs,
			 DRM_MODE_ENCODER_DAC);
	drm_encoder_helper_add(encoder, &nv50_dac_helper_funcs);

	encoder->possible_crtcs = entry->heads;
	encoder->possible_clones = 0;

	drm_mode_connector_attach_encoder(connector, encoder);
	return 0;
}

