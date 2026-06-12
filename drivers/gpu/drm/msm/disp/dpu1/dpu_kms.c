// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__

#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#include <linux/of_irq.h>
#include <linux/pm_opp.h>

#include <drm/drm_crtc.h>
#include <drm/drm_file.h>

#include "msm_drv.h"
#include "msm_mmu.h"
#include "msm_gem.h"

#include "dpu_kms.h"
#include "dpu_core_irq.h"
#include "dpu_formats.h"
#include "dpu_hw_vbif.h"
#include "dpu_vbif.h"
#include "dpu_encoder.h"
#include "dpu_plane.h"
#include "dpu_crtc.h"

#define CREATE_TRACE_POINTS
#include "dpu_trace.h"

/*
 * To enable overall DRM driver logging
 * # echo 0x2 > /sys/module/drm/parameters/debug
 *
 * To enable DRM driver h/w logging
 * # echo <mask> > /sys/kernel/debug/dri/0/debug/hw_log_mask
 *
 * See dpu_hw_mdss.h for h/w logging mask definitions (search for DPU_DBG_MASK_)
 */
#define DPU_DEBUGFS_DIR "msm_dpu"
#define DPU_DEBUGFS_HWMASKNAME "hw_log_mask"

#define MIN_IB_BW	400000000ULL /* Min ib vote 400MB */

static int dpu_kms_hw_init(struct msm_kms *kms);
static int dpu_sde_kms_hw_init(struct msm_kms *kms);
static void _dpu_kms_mmu_destroy(struct dpu_kms *dpu_kms);

#ifdef CONFIG_DEBUG_FS
static int _dpu_danger_signal_status(struct seq_file *s,
		bool danger_status)
{
	struct dpu_kms *kms = (struct dpu_kms *)s->private;
	struct dpu_danger_safe_status status;
	int i;

	if (!kms->hw_mdp) {
		DPU_ERROR("invalid arg(s)\n");
		return 0;
	}

	memset(&status, 0, sizeof(struct dpu_danger_safe_status));

	pm_runtime_get_sync(&kms->pdev->dev);
	if (danger_status) {
		seq_puts(s, "\nDanger signal status:\n");
		if (kms->hw_mdp->ops.get_danger_status)
			kms->hw_mdp->ops.get_danger_status(kms->hw_mdp,
					&status);
	} else {
		seq_puts(s, "\nSafe signal status:\n");
		if (kms->hw_mdp->ops.get_safe_status)
			kms->hw_mdp->ops.get_safe_status(kms->hw_mdp,
					&status);
	}
	pm_runtime_put_sync(&kms->pdev->dev);

	seq_printf(s, "MDP     :  0x%x\n", status.mdp);

	for (i = SSPP_VIG0; i < SSPP_MAX; i++)
		seq_printf(s, "SSPP%d   :  0x%x  \t", i - SSPP_VIG0,
				status.sspp[i]);
	seq_puts(s, "\n");

	return 0;
}

static int dpu_debugfs_danger_stats_show(struct seq_file *s, void *v)
{
	return _dpu_danger_signal_status(s, true);
}
DEFINE_SHOW_ATTRIBUTE(dpu_debugfs_danger_stats);

static int dpu_debugfs_safe_stats_show(struct seq_file *s, void *v)
{
	return _dpu_danger_signal_status(s, false);
}
DEFINE_SHOW_ATTRIBUTE(dpu_debugfs_safe_stats);

static void dpu_debugfs_danger_init(struct dpu_kms *dpu_kms,
		struct dentry *parent)
{
	struct dentry *entry = debugfs_create_dir("danger", parent);

	debugfs_create_file("danger_status", 0600, entry,
			dpu_kms, &dpu_debugfs_danger_stats_fops);
	debugfs_create_file("safe_status", 0600, entry,
			dpu_kms, &dpu_debugfs_safe_stats_fops);
}

static int _dpu_debugfs_show_regset32(struct seq_file *s, void *data)
{
	struct dpu_debugfs_regset32 *regset = s->private;
	struct dpu_kms *dpu_kms = regset->dpu_kms;
	void __iomem *base;
	uint32_t i, addr;

	if (!dpu_kms->mmio)
		return 0;

	base = dpu_kms->mmio + regset->offset;

	/* insert padding spaces, if needed */
	if (regset->offset & 0xF) {
		seq_printf(s, "[%x]", regset->offset & ~0xF);
		for (i = 0; i < (regset->offset & 0xF); i += 4)
			seq_puts(s, "         ");
	}

	pm_runtime_get_sync(&dpu_kms->pdev->dev);

	/* main register output */
	for (i = 0; i < regset->blk_len; i += 4) {
		addr = regset->offset + i;
		if ((addr & 0xF) == 0x0)
			seq_printf(s, i ? "\n[%x]" : "[%x]", addr);
		seq_printf(s, " %08x", readl_relaxed(base + i));
	}
	seq_puts(s, "\n");
	pm_runtime_put_sync(&dpu_kms->pdev->dev);

	return 0;
}

static int dpu_debugfs_open_regset32(struct inode *inode,
		struct file *file)
{
	return single_open(file, _dpu_debugfs_show_regset32, inode->i_private);
}

static const struct file_operations dpu_fops_regset32 = {
	.open =		dpu_debugfs_open_regset32,
	.read =		seq_read,
	.llseek =	seq_lseek,
	.release =	single_release,
};

void dpu_debugfs_setup_regset32(struct dpu_debugfs_regset32 *regset,
		uint32_t offset, uint32_t length, struct dpu_kms *dpu_kms)
{
	if (regset) {
		regset->offset = offset;
		regset->blk_len = length;
		regset->dpu_kms = dpu_kms;
	}
}

void dpu_debugfs_create_regset32(const char *name, umode_t mode,
		void *parent, struct dpu_debugfs_regset32 *regset)
{
	if (!name || !regset || !regset->dpu_kms || !regset->blk_len)
		return;

	/* make sure offset is a multiple of 4 */
	regset->offset = round_down(regset->offset, 4);

	debugfs_create_file(name, mode, parent, regset, &dpu_fops_regset32);
}

static int dpu_kms_debugfs_init(struct msm_kms *kms, struct drm_minor *minor)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	void *p = dpu_hw_util_get_log_mask_ptr();
	struct dentry *entry;
	struct drm_device *dev;
	struct msm_drm_private *priv;

	if (!p)
		return -EINVAL;

	dev = dpu_kms->dev;
	priv = dev->dev_private;

	entry = debugfs_create_dir("debug", minor->debugfs_root);

	debugfs_create_x32(DPU_DEBUGFS_HWMASKNAME, 0600, entry, p);

	dpu_debugfs_danger_init(dpu_kms, entry);
	dpu_debugfs_vbif_init(dpu_kms, entry);
	dpu_debugfs_core_irq_init(dpu_kms, entry);

	if (priv->dp)
		msm_dp_debugfs_init(priv->dp, minor);

	return dpu_core_perf_debugfs_init(dpu_kms, entry);
}
#endif

/* Global/shared object state funcs */

/*
 * This is a helper that returns the private state currently in operation.
 * Note that this would return the "old_state" if called in the atomic check
 * path, and the "new_state" after the atomic swap has been done.
 */
struct dpu_global_state *
dpu_kms_get_existing_global_state(struct dpu_kms *dpu_kms)
{
	return to_dpu_global_state(dpu_kms->global_state.state);
}

/*
 * This acquires the modeset lock set aside for global state, creates
 * a new duplicated private object state.
 */
struct dpu_global_state *dpu_kms_get_global_state(struct drm_atomic_state *s)
{
	struct msm_drm_private *priv = s->dev->dev_private;
	struct dpu_kms *dpu_kms = to_dpu_kms(priv->kms);
	struct drm_private_state *priv_state;
	int ret;

	ret = drm_modeset_lock(&dpu_kms->global_state_lock, s->acquire_ctx);
	if (ret)
		return ERR_PTR(ret);

	priv_state = drm_atomic_get_private_obj_state(s,
						&dpu_kms->global_state);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_dpu_global_state(priv_state);
}

static struct drm_private_state *
dpu_kms_global_duplicate_state(struct drm_private_obj *obj)
{
	struct dpu_global_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void dpu_kms_global_destroy_state(struct drm_private_obj *obj,
				      struct drm_private_state *state)
{
	struct dpu_global_state *dpu_state = to_dpu_global_state(state);

	kfree(dpu_state);
}

static const struct drm_private_state_funcs dpu_kms_global_state_funcs = {
	.atomic_duplicate_state = dpu_kms_global_duplicate_state,
	.atomic_destroy_state = dpu_kms_global_destroy_state,
};

static int dpu_kms_global_obj_init(struct dpu_kms *dpu_kms)
{
	struct dpu_global_state *state;

	drm_modeset_lock_init(&dpu_kms->global_state_lock);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	drm_atomic_private_obj_init(dpu_kms->dev, &dpu_kms->global_state,
				    &state->base,
				    &dpu_kms_global_state_funcs);
	return 0;
}

static int dpu_kms_parse_data_bus_icc_path(struct dpu_kms *dpu_kms)
{
	struct icc_path *path0;
	struct icc_path *path1;
	struct drm_device *dev = dpu_kms->dev;

	path0 = of_icc_get(dev->dev, "mdp0-mem");
	path1 = of_icc_get(dev->dev, "mdp1-mem");

	if (IS_ERR_OR_NULL(path0))
		return PTR_ERR_OR_ZERO(path0);

	dpu_kms->path[0] = path0;
	dpu_kms->num_paths = 1;

	if (!IS_ERR_OR_NULL(path1)) {
		dpu_kms->path[1] = path1;
		dpu_kms->num_paths++;
	}
	return 0;
}

static int dpu_kms_enable_vblank(struct msm_kms *kms, struct drm_crtc *crtc)
{
	return dpu_crtc_vblank(crtc, true);
}

static void dpu_kms_disable_vblank(struct msm_kms *kms, struct drm_crtc *crtc)
{
	dpu_crtc_vblank(crtc, false);
}

static void dpu_kms_enable_commit(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	pm_runtime_get_sync(&dpu_kms->pdev->dev);
}

static void dpu_kms_disable_commit(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	pm_runtime_put_sync(&dpu_kms->pdev->dev);
}

static ktime_t dpu_kms_vsync_time(struct msm_kms *kms, struct drm_crtc *crtc)
{
	struct drm_encoder *encoder;

	drm_for_each_encoder_mask(encoder, crtc->dev, crtc->state->encoder_mask) {
		ktime_t vsync_time;

		if (dpu_encoder_vsync_time(encoder, &vsync_time) == 0)
			return vsync_time;
	}

	return ktime_get();
}

static void dpu_kms_prepare_commit(struct msm_kms *kms,
		struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_encoder *encoder;
	int i;

	if (!kms)
		return;

	/* Call prepare_commit for all affected encoders */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		drm_for_each_encoder_mask(encoder, crtc->dev,
					  crtc_state->encoder_mask) {
			dpu_encoder_prepare_commit(encoder);
		}
	}
}

static void dpu_kms_flush_commit(struct msm_kms *kms, unsigned crtc_mask)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	struct drm_crtc *crtc;

	for_each_crtc_mask(dpu_kms->dev, crtc, crtc_mask) {
		if (!crtc->state->active)
			continue;

		trace_dpu_kms_commit(DRMID(crtc));
		dpu_crtc_commit_kickoff(crtc);
	}
}

/*
 * Override the encoder enable since we need to setup the inline rotator and do
 * some crtc magic before enabling any bridge that might be present.
 */
void dpu_kms_encoder_enable(struct drm_encoder *encoder)
{
	const struct drm_encoder_helper_funcs *funcs = encoder->helper_private;
	struct drm_device *dev = encoder->dev;
	struct drm_crtc *crtc;

	/* Forward this enable call to the commit hook */
	if (funcs && funcs->commit)
		funcs->commit(encoder);

	drm_for_each_crtc(crtc, dev) {
		if (!(crtc->state->encoder_mask & drm_encoder_mask(encoder)))
			continue;

		trace_dpu_kms_enc_enable(DRMID(crtc));
	}
}

static void dpu_kms_complete_commit(struct msm_kms *kms, unsigned crtc_mask)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	struct drm_crtc *crtc;

	DPU_ATRACE_BEGIN("kms_complete_commit");

	for_each_crtc_mask(dpu_kms->dev, crtc, crtc_mask)
		dpu_crtc_complete_commit(crtc);

	DPU_ATRACE_END("kms_complete_commit");
}

static void dpu_kms_wait_for_commit_done(struct msm_kms *kms,
		struct drm_crtc *crtc)
{
	struct drm_encoder *encoder;
	struct drm_device *dev;
	int ret;

	if (!kms || !crtc || !crtc->state) {
		DPU_ERROR("invalid params\n");
		return;
	}

	dev = crtc->dev;

	if (!crtc->state->enable) {
		DPU_DEBUG("[crtc:%d] not enable\n", crtc->base.id);
		return;
	}

	if (!crtc->state->active) {
		DPU_DEBUG("[crtc:%d] not active\n", crtc->base.id);
		return;
	}

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		if (encoder->crtc != crtc)
			continue;
		/*
		 * Wait for post-flush if necessary to delay before
		 * plane_cleanup. For example, wait for vsync in case of video
		 * mode panels. This may be a no-op for command mode panels.
		 */
		trace_dpu_kms_wait_for_commit_done(DRMID(crtc));
		ret = dpu_encoder_wait_for_event(encoder, MSM_ENC_COMMIT_DONE);
		if (ret && ret != -EWOULDBLOCK) {
			DPU_ERROR("wait for commit done returned %d\n", ret);
			break;
		}
	}
}

static void dpu_kms_wait_flush(struct msm_kms *kms, unsigned crtc_mask)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);
	struct drm_crtc *crtc;

	for_each_crtc_mask(dpu_kms->dev, crtc, crtc_mask)
		dpu_kms_wait_for_commit_done(kms, crtc);
}

static int _dpu_kms_initialize_dsi(struct drm_device *dev,
				    struct msm_drm_private *priv,
				    struct dpu_kms *dpu_kms)
{
	struct drm_encoder *encoder = NULL;
	int i, rc = 0;

	if (!(priv->dsi[0] || priv->dsi[1]))
		return rc;

	/*TODO: Support two independent DSI connectors */
	encoder = dpu_encoder_init(dev, DRM_MODE_ENCODER_DSI);
	if (IS_ERR(encoder)) {
		DPU_ERROR("encoder init failed for dsi display\n");
		return PTR_ERR(encoder);
	}

	priv->encoders[priv->num_encoders++] = encoder;

	for (i = 0; i < ARRAY_SIZE(priv->dsi); i++) {
		if (!priv->dsi[i])
			continue;

		rc = msm_dsi_modeset_init(priv->dsi[i], dev, encoder);
		if (rc) {
			DPU_ERROR("modeset_init failed for dsi[%d], rc = %d\n",
				i, rc);
			break;
		}
	}

	return rc;
}

static int _dpu_kms_initialize_displayport(struct drm_device *dev,
					    struct msm_drm_private *priv,
					    struct dpu_kms *dpu_kms)
{
	struct drm_encoder *encoder = NULL;
	int rc = 0;

	if (!priv->dp)
		return rc;

	encoder = dpu_encoder_init(dev, DRM_MODE_ENCODER_TMDS);
	if (IS_ERR(encoder)) {
		DPU_ERROR("encoder init failed for dsi display\n");
		return PTR_ERR(encoder);
	}

	rc = msm_dp_modeset_init(priv->dp, dev, encoder);
	if (rc) {
		DPU_ERROR("modeset_init failed for DP, rc = %d\n", rc);
		drm_encoder_cleanup(encoder);
		return rc;
	}

	priv->encoders[priv->num_encoders++] = encoder;
	return rc;
}

/**
 * _dpu_kms_setup_displays - create encoders, bridges and connectors
 *                           for underlying displays
 * @dev:        Pointer to drm device structure
 * @priv:       Pointer to private drm device data
 * @dpu_kms:    Pointer to dpu kms structure
 * Returns:     Zero on success
 */
static int _dpu_kms_setup_displays(struct drm_device *dev,
				    struct msm_drm_private *priv,
				    struct dpu_kms *dpu_kms)
{
	int rc = 0;

	rc = _dpu_kms_initialize_dsi(dev, priv, dpu_kms);
	if (rc) {
		DPU_ERROR("initialize_dsi failed, rc = %d\n", rc);
		return rc;
	}

	rc = _dpu_kms_initialize_displayport(dev, priv, dpu_kms);
	if (rc) {
		DPU_ERROR("initialize_DP failed, rc = %d\n", rc);
		return rc;
	}

	return rc;
}

static void _dpu_kms_drm_obj_destroy(struct dpu_kms *dpu_kms)
{
	struct msm_drm_private *priv;
	int i;

	priv = dpu_kms->dev->dev_private;

	for (i = 0; i < priv->num_crtcs; i++)
		priv->crtcs[i]->funcs->destroy(priv->crtcs[i]);
	priv->num_crtcs = 0;

	for (i = 0; i < priv->num_planes; i++)
		priv->planes[i]->funcs->destroy(priv->planes[i]);
	priv->num_planes = 0;

	for (i = 0; i < priv->num_connectors; i++)
		priv->connectors[i]->funcs->destroy(priv->connectors[i]);
	priv->num_connectors = 0;

	for (i = 0; i < priv->num_encoders; i++)
		priv->encoders[i]->funcs->destroy(priv->encoders[i]);
	priv->num_encoders = 0;
}

static int _dpu_kms_drm_obj_init(struct dpu_kms *dpu_kms)
{
	struct drm_device *dev;
	struct drm_plane *primary_planes[MAX_PLANES], *plane;
	struct drm_plane *cursor_planes[MAX_PLANES] = { NULL };
	struct drm_crtc *crtc;

	struct msm_drm_private *priv;
	struct dpu_mdss_cfg *catalog;

	int primary_planes_idx = 0, cursor_planes_idx = 0, i, ret;
	int max_crtc_count;
	dev = dpu_kms->dev;
	priv = dev->dev_private;
	catalog = dpu_kms->catalog;

	/*
	 * Create encoder and query display drivers to create
	 * bridges and connectors.  Skip if encoders already exist
	 * (SDE flat-DT path pre-initializes displays before
	 * drm_obj_init).
	 */
	if (!priv->num_encoders) {
		ret = _dpu_kms_setup_displays(dev, priv, dpu_kms);
		if (ret)
			goto fail;
	}

	max_crtc_count = min(catalog->mixer_count, priv->num_encoders);

	/* Create the planes, keeping track of one primary/cursor per crtc */
	for (i = 0; i < catalog->sspp_count; i++) {
		enum drm_plane_type type;

		if ((catalog->sspp[i].features & BIT(DPU_SSPP_CURSOR))
			&& cursor_planes_idx < max_crtc_count)
			type = DRM_PLANE_TYPE_CURSOR;
		else if (primary_planes_idx < max_crtc_count)
			type = DRM_PLANE_TYPE_PRIMARY;
		else
			type = DRM_PLANE_TYPE_OVERLAY;

		DPU_DEBUG("Create plane type %d with features %lx (cur %lx)\n",
			  type, catalog->sspp[i].features,
			  catalog->sspp[i].features & BIT(DPU_SSPP_CURSOR));

		plane = dpu_plane_init(dev, catalog->sspp[i].id, type,
				       (1UL << max_crtc_count) - 1, 0);
		if (IS_ERR(plane)) {
			DPU_ERROR("dpu_plane_init failed\n");
			ret = PTR_ERR(plane);
			goto fail;
		}
		priv->planes[priv->num_planes++] = plane;

		if (type == DRM_PLANE_TYPE_CURSOR)
			cursor_planes[cursor_planes_idx++] = plane;
		else if (type == DRM_PLANE_TYPE_PRIMARY)
			primary_planes[primary_planes_idx++] = plane;
	}

	max_crtc_count = min(max_crtc_count, primary_planes_idx);

	/* Create one CRTC per encoder */
	for (i = 0; i < max_crtc_count; i++) {
		crtc = dpu_crtc_init(dev, primary_planes[i], cursor_planes[i]);
		if (IS_ERR(crtc)) {
			ret = PTR_ERR(crtc);
			goto fail;
		}
		priv->crtcs[priv->num_crtcs++] = crtc;
	}

	/* All CRTCs are compatible with all encoders */
	for (i = 0; i < priv->num_encoders; i++)
		priv->encoders[i]->possible_crtcs = (1 << priv->num_crtcs) - 1;

	return 0;
fail:
	_dpu_kms_drm_obj_destroy(dpu_kms);
	return ret;
}

static long dpu_kms_round_pixclk(struct msm_kms *kms, unsigned long rate,
		struct drm_encoder *encoder)
{
	return rate;
}

static void _dpu_kms_hw_destroy(struct dpu_kms *dpu_kms)
{
	int i;

	if (dpu_kms->hw_intr)
		dpu_hw_intr_destroy(dpu_kms->hw_intr);
	dpu_kms->hw_intr = NULL;

	/* safe to call these more than once during shutdown */
	_dpu_kms_mmu_destroy(dpu_kms);

	if (dpu_kms->catalog) {
		for (i = 0; i < ARRAY_SIZE(dpu_kms->hw_vbif); i++) {
			if (dpu_kms->hw_vbif[i]) {
				dpu_hw_vbif_destroy(dpu_kms->hw_vbif[i]);
				dpu_kms->hw_vbif[i] = NULL;
			}
		}
	}

	if (dpu_kms->rm_init)
		dpu_rm_destroy(&dpu_kms->rm);
	dpu_kms->rm_init = false;

	if (dpu_kms->catalog)
		dpu_hw_catalog_deinit(dpu_kms->catalog);
	dpu_kms->catalog = NULL;

	if (dpu_kms->vbif[VBIF_NRT])
		devm_iounmap(&dpu_kms->pdev->dev, dpu_kms->vbif[VBIF_NRT]);
	dpu_kms->vbif[VBIF_NRT] = NULL;

	if (dpu_kms->vbif[VBIF_RT])
		devm_iounmap(&dpu_kms->pdev->dev, dpu_kms->vbif[VBIF_RT]);
	dpu_kms->vbif[VBIF_RT] = NULL;

	if (dpu_kms->hw_mdp)
		dpu_hw_mdp_destroy(dpu_kms->hw_mdp);
	dpu_kms->hw_mdp = NULL;

	if (dpu_kms->mmio)
		devm_iounmap(&dpu_kms->pdev->dev, dpu_kms->mmio);
	dpu_kms->mmio = NULL;
}

static void dpu_kms_destroy(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms;

	if (!kms) {
		DPU_ERROR("invalid kms\n");
		return;
	}

	dpu_kms = to_dpu_kms(kms);

	_dpu_kms_hw_destroy(dpu_kms);
}

static void _dpu_kms_set_encoder_mode(struct msm_kms *kms,
				 struct drm_encoder *encoder,
				 bool cmd_mode)
{
	struct msm_display_info info;
	struct msm_drm_private *priv = encoder->dev->dev_private;
	int i, rc = 0;

	memset(&info, 0, sizeof(info));

	info.intf_type = encoder->encoder_type;
	info.capabilities = cmd_mode ? MSM_DISPLAY_CAP_CMD_MODE :
			MSM_DISPLAY_CAP_VID_MODE;

	switch (info.intf_type) {
	case DRM_MODE_ENCODER_DSI:
		/* TODO: No support for DSI swap */
		for (i = 0; i < ARRAY_SIZE(priv->dsi); i++) {
			if (priv->dsi[i]) {
				info.h_tile_instance[info.num_of_h_tiles] = i;
				info.num_of_h_tiles++;
			}
		}
		break;
	case DRM_MODE_ENCODER_TMDS:
		info.num_of_h_tiles = 1;
		break;
	};

	rc = dpu_encoder_setup(encoder->dev, encoder, &info);
	if (rc)
		DPU_ERROR("failed to setup DPU encoder %d: rc:%d\n",
			encoder->base.id, rc);
}

static irqreturn_t dpu_irq(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);

	return dpu_core_irq(dpu_kms);
}

static void dpu_irq_preinstall(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);

	dpu_core_irq_preinstall(dpu_kms);
}

static int dpu_irq_postinstall(struct msm_kms *kms)
{
	struct msm_drm_private *priv;
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);

	if (!dpu_kms || !dpu_kms->dev)
		return -EINVAL;

	priv = dpu_kms->dev->dev_private;
	if (!priv)
		return -EINVAL;

	msm_dp_irq_postinstall(priv->dp);

	return 0;
}

static void dpu_irq_uninstall(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms = to_dpu_kms(kms);

	dpu_core_irq_uninstall(dpu_kms);
}

static const struct msm_kms_funcs kms_funcs = {
	.hw_init         = dpu_kms_hw_init,
	.irq_preinstall  = dpu_irq_preinstall,
	.irq_postinstall = dpu_irq_postinstall,
	.irq_uninstall   = dpu_irq_uninstall,
	.irq             = dpu_irq,
	.enable_commit   = dpu_kms_enable_commit,
	.disable_commit  = dpu_kms_disable_commit,
	.vsync_time      = dpu_kms_vsync_time,
	.prepare_commit  = dpu_kms_prepare_commit,
	.flush_commit    = dpu_kms_flush_commit,
	.wait_flush      = dpu_kms_wait_flush,
	.complete_commit = dpu_kms_complete_commit,
	.enable_vblank   = dpu_kms_enable_vblank,
	.disable_vblank  = dpu_kms_disable_vblank,
	.check_modified_format = dpu_format_check_modified_format,
	.get_format      = dpu_get_msm_format,
	.round_pixclk    = dpu_kms_round_pixclk,
	.destroy         = dpu_kms_destroy,
	.set_encoder_mode = _dpu_kms_set_encoder_mode,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init    = dpu_kms_debugfs_init,
#endif
};

static void _dpu_kms_mmu_destroy(struct dpu_kms *dpu_kms)
{
	struct msm_mmu *mmu;

	if (!dpu_kms->base.aspace)
		return;

	mmu = dpu_kms->base.aspace->mmu;

	mmu->funcs->detach(mmu);
	msm_gem_address_space_put(dpu_kms->base.aspace);

	dpu_kms->base.aspace = NULL;
}

static int _dpu_kms_mmu_init(struct dpu_kms *dpu_kms)
{
	struct iommu_domain *domain;
	struct msm_gem_address_space *aspace;
	struct msm_mmu *mmu;

	domain = iommu_domain_alloc(&platform_bus_type);
	if (!domain)
		return 0;

	mmu = msm_iommu_new(dpu_kms->dev->dev, domain);
	if (IS_ERR(mmu)) {
		iommu_domain_free(domain);
		return PTR_ERR(mmu);
	}
	aspace = msm_gem_address_space_create(mmu, "dpu1",
		0x1000, 0x100000000 - 0x1000);

	if (IS_ERR(aspace)) {
		mmu->funcs->destroy(mmu);
		return PTR_ERR(aspace);
	}

	dpu_kms->base.aspace = aspace;
	return 0;
}

static struct dss_clk *_dpu_kms_get_clk(struct dpu_kms *dpu_kms,
		char *clock_name)
{
	struct dss_module_power *mp = &dpu_kms->mp;
	int i;

	for (i = 0; i < mp->num_clk; i++) {
		if (!strcmp(mp->clk_config[i].clk_name, clock_name))
			return &mp->clk_config[i];
	}

	return NULL;
}

u64 dpu_kms_get_clk_rate(struct dpu_kms *dpu_kms, char *clock_name)
{
	struct dss_clk *clk;

	clk = _dpu_kms_get_clk(dpu_kms, clock_name);
	if (!clk)
		return -EINVAL;

	return clk_get_rate(clk->clk);
}

static int dpu_kms_hw_init(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms;
	struct drm_device *dev;
	int i, rc = -EINVAL;

	if (!kms) {
		DPU_ERROR("invalid kms\n");
		return rc;
	}

	dpu_kms = to_dpu_kms(kms);
	dev = dpu_kms->dev;

	rc = dpu_kms_global_obj_init(dpu_kms);
	if (rc)
		return rc;

	atomic_set(&dpu_kms->bandwidth_ref, 0);

	dpu_kms->mmio = msm_ioremap(dpu_kms->pdev, "mdp", "mdp");
	if (IS_ERR(dpu_kms->mmio)) {
		rc = PTR_ERR(dpu_kms->mmio);
		DPU_ERROR("mdp register memory map failed: %d\n", rc);
		dpu_kms->mmio = NULL;
		goto error;
	}
	DRM_DEBUG("mapped dpu address space @%pK\n", dpu_kms->mmio);

	dpu_kms->vbif[VBIF_RT] = msm_ioremap(dpu_kms->pdev, "vbif", "vbif");
	if (IS_ERR(dpu_kms->vbif[VBIF_RT])) {
		rc = PTR_ERR(dpu_kms->vbif[VBIF_RT]);
		DPU_ERROR("vbif register memory map failed: %d\n", rc);
		dpu_kms->vbif[VBIF_RT] = NULL;
		goto error;
	}
	dpu_kms->vbif[VBIF_NRT] = msm_ioremap_quiet(dpu_kms->pdev, "vbif_nrt", "vbif_nrt");
	if (IS_ERR(dpu_kms->vbif[VBIF_NRT])) {
		dpu_kms->vbif[VBIF_NRT] = NULL;
		DPU_DEBUG("VBIF NRT is not defined");
	}

	dpu_kms->reg_dma = msm_ioremap_quiet(dpu_kms->pdev, "regdma", "regdma");
	if (IS_ERR(dpu_kms->reg_dma)) {
		dpu_kms->reg_dma = NULL;
		DPU_DEBUG("REG_DMA is not defined");
	}

	dpu_kms_parse_data_bus_icc_path(dpu_kms);

	rc = pm_runtime_resume_and_get(&dpu_kms->pdev->dev);
	if (rc < 0)
		goto error;

	dpu_kms->core_rev = readl_relaxed(dpu_kms->mmio + 0x0);

	pr_info("dpu hardware revision:0x%x\n", dpu_kms->core_rev);

	dpu_kms->catalog = dpu_hw_catalog_init(dpu_kms->core_rev);
	if (IS_ERR_OR_NULL(dpu_kms->catalog)) {
		rc = PTR_ERR(dpu_kms->catalog);
		if (!dpu_kms->catalog)
			rc = -EINVAL;
		DPU_ERROR("catalog init failed: %d\n", rc);
		dpu_kms->catalog = NULL;
		goto power_error;
	}

	/*
	 * Now we need to read the HW catalog and initialize resources such as
	 * clocks, regulators, GDSC/MMAGIC, ioremap the register ranges etc
	 */
	rc = _dpu_kms_mmu_init(dpu_kms);
	if (rc) {
		DPU_ERROR("dpu_kms_mmu_init failed: %d\n", rc);
		goto power_error;
	}

	rc = dpu_rm_init(&dpu_kms->rm, dpu_kms->catalog, dpu_kms->mmio);
	if (rc) {
		DPU_ERROR("rm init failed: %d\n", rc);
		goto power_error;
	}

	dpu_kms->rm_init = true;

	dpu_kms->hw_mdp = dpu_hw_mdptop_init(MDP_TOP, dpu_kms->mmio,
					     dpu_kms->catalog);
	if (IS_ERR(dpu_kms->hw_mdp)) {
		rc = PTR_ERR(dpu_kms->hw_mdp);
		DPU_ERROR("failed to get hw_mdp: %d\n", rc);
		dpu_kms->hw_mdp = NULL;
		goto power_error;
	}

	for (i = 0; i < dpu_kms->catalog->vbif_count; i++) {
		u32 vbif_idx = dpu_kms->catalog->vbif[i].id;

		dpu_kms->hw_vbif[vbif_idx] = dpu_hw_vbif_init(vbif_idx,
				dpu_kms->vbif[vbif_idx], dpu_kms->catalog);
		if (IS_ERR_OR_NULL(dpu_kms->hw_vbif[vbif_idx])) {
			rc = PTR_ERR(dpu_kms->hw_vbif[vbif_idx]);
			if (!dpu_kms->hw_vbif[vbif_idx])
				rc = -EINVAL;
			DPU_ERROR("failed to init vbif %d: %d\n", vbif_idx, rc);
			dpu_kms->hw_vbif[vbif_idx] = NULL;
			goto power_error;
		}
	}

	rc = dpu_core_perf_init(&dpu_kms->perf, dev, dpu_kms->catalog,
			_dpu_kms_get_clk(dpu_kms, "core"));
	if (rc) {
		DPU_ERROR("failed to init perf %d\n", rc);
		goto perf_err;
	}

	dpu_kms->hw_intr = dpu_hw_intr_init(dpu_kms->mmio, dpu_kms->catalog);
	if (IS_ERR_OR_NULL(dpu_kms->hw_intr)) {
		rc = PTR_ERR(dpu_kms->hw_intr);
		DPU_ERROR("hw_intr init failed: %d\n", rc);
		dpu_kms->hw_intr = NULL;
		goto hw_intr_init_err;
	}

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	/*
	 * max crtc width is equal to the max mixer width * 2 and max height is
	 * is 4K
	 */
	dev->mode_config.max_width =
			dpu_kms->catalog->caps->max_mixer_width * 2;
	dev->mode_config.max_height = 4096;

	/*
	 * Support format modifiers for compression etc.
	 */
	dev->mode_config.allow_fb_modifiers = true;

	/*
	 * _dpu_kms_drm_obj_init should create the DRM related objects
	 * i.e. CRTCs, planes, encoders, connectors and so forth
	 */
	rc = _dpu_kms_drm_obj_init(dpu_kms);
	if (rc) {
		DPU_ERROR("modeset init failed: %d\n", rc);
		goto drm_obj_init_err;
	}

	dpu_vbif_init_memtypes(dpu_kms);

	pm_runtime_put_sync(&dpu_kms->pdev->dev);

	return 0;

drm_obj_init_err:
	_dpu_kms_drm_obj_destroy(dpu_kms);
	dpu_core_perf_destroy(&dpu_kms->perf);
hw_intr_init_err:
perf_err:
power_error:
	pm_runtime_put_sync(&dpu_kms->pdev->dev);
error:
	_dpu_kms_hw_destroy(dpu_kms);

	return rc;
}

struct msm_kms *dpu_kms_init(struct drm_device *dev)
{
	struct msm_drm_private *priv;
	struct dpu_kms *dpu_kms;
	int irq;

	if (!dev) {
		DPU_ERROR("drm device node invalid\n");
		return ERR_PTR(-EINVAL);
	}

	priv = dev->dev_private;
	dpu_kms = to_dpu_kms(priv->kms);

	irq = irq_of_parse_and_map(dpu_kms->pdev->dev.of_node, 0);
	if (irq < 0) {
		DPU_ERROR("failed to get irq: %d\n", irq);
		return ERR_PTR(irq);
	}
	dpu_kms->base.irq = irq;

	return &dpu_kms->base;
}

/* ==========================================================================
 * SDE flat KMS: downstream DT layout (qcom,sde-kms)
 * --------------------------------------------------------------------------
 * The downstream SDE DT uses flat reg-names with _phys suffix
 * (mdp_phys, vbif_phys, vbif_nrt_phys, regdma_phys) instead of the
 * upstream DPU names (mdp, vbif, vbif_nrt, regdma).  This section
 * provides an alternate KMS init path that maps the flat reg-names
 * and adds a stub DSI connector for initial bring-up.
 * ========================================================================== */

static int sde_stub_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	/* 1080x2400 @ 60Hz — primary mode for kona AMOLED panels */
	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;
	mode->hdisplay = 1080;
	mode->hsync_start = 1080 + 8;
	mode->hsync_end = 1080 + 8 + 8;
	mode->htotal = 1080 + 8 + 8 + 8;
	mode->vdisplay = 2400;
	mode->vsync_start = 2400 + 4;
	mode->vsync_end = 2400 + 4 + 4;
	mode->vtotal = 2400 + 4 + 4 + 4;
	mode->clock = mode->htotal * mode->vtotal * 60 / 1000;
	mode->type = DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	/* 1440x3120 @ 60Hz — high-resolution mode */
	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 1;
	mode->hdisplay = 1440;
	mode->hsync_start = 1440 + 8;
	mode->hsync_end = 1440 + 8 + 8;
	mode->htotal = 1440 + 8 + 8 + 8;
	mode->vdisplay = 3120;
	mode->vsync_start = 3120 + 4;
	mode->vsync_end = 3120 + 4 + 4;
	mode->vtotal = 3120 + 4 + 4 + 4;
	mode->clock = mode->htotal * mode->vtotal * 60 / 1000;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 2;
}

static enum drm_mode_status sde_stub_connector_mode_valid(
		struct drm_connector *connector,
		struct drm_display_mode *mode)
{
	return MODE_OK;
}

static const struct drm_connector_funcs sde_stub_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs sde_stub_connector_helper_funcs = {
	.get_modes = sde_stub_connector_get_modes,
	.mode_valid = sde_stub_connector_mode_valid,
};

/**
 * _dpu_sde_kms_setup_stub_connector - create a stub DSI connector
 * @dpu_kms:    Pointer to dpu_kms structure
 *
 * Create one DSI encoder and one stub connector with two fixed modes
 * (1080x2400@60Hz and 1440x3120@60Hz).  This ensures at least one
 * encoder/CRTC pair exists when no real DSI bridge is bound, so that
 * _dpu_kms_drm_obj_init() can create planes and CRTCs.
 *
 * The stub connector should be removed once the real DSI bridge is
 * fully brought up and priv->dsi[] is populated.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int _dpu_sde_kms_setup_stub_connector(struct dpu_kms *dpu_kms)
{
	struct drm_device *dev = dpu_kms->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	encoder = dpu_encoder_init(dev, DRM_MODE_ENCODER_DSI);
	if (IS_ERR(encoder)) {
		DPU_ERROR("stub encoder init failed\n");
		return PTR_ERR(encoder);
	}
	priv->encoders[priv->num_encoders++] = encoder;

	connector = devm_kzalloc(&dpu_kms->pdev->dev, sizeof(*connector),
				 GFP_KERNEL);
	if (!connector)
		return -ENOMEM;

	ret = drm_connector_init(dev, connector,
				 &sde_stub_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;

	drm_connector_helper_add(connector, &sde_stub_connector_helper_funcs);

	connector->status = connector_status_connected;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ret;

	return 0;
}

static const struct msm_kms_funcs sde_kms_funcs = {
	.hw_init         = dpu_sde_kms_hw_init,
	.irq_preinstall  = dpu_irq_preinstall,
	.irq_postinstall = dpu_irq_postinstall,
	.irq_uninstall   = dpu_irq_uninstall,
	.irq             = dpu_irq,
	.enable_commit   = dpu_kms_enable_commit,
	.disable_commit  = dpu_kms_disable_commit,
	.vsync_time      = dpu_kms_vsync_time,
	.prepare_commit  = dpu_kms_prepare_commit,
	.flush_commit    = dpu_kms_flush_commit,
	.wait_flush      = dpu_kms_wait_flush,
	.complete_commit = dpu_kms_complete_commit,
	.enable_vblank   = dpu_kms_enable_vblank,
	.disable_vblank  = dpu_kms_disable_vblank,
	.check_modified_format = dpu_format_check_modified_format,
	.get_format      = dpu_get_msm_format,
	.round_pixclk    = dpu_kms_round_pixclk,
	.destroy         = dpu_kms_destroy,
	.set_encoder_mode = _dpu_kms_set_encoder_mode,
#ifdef CONFIG_DEBUG_FS
	.debugfs_init    = dpu_kms_debugfs_init,
#endif
};

/**
 * dpu_sde_kms_hw_init - SDE flat DT hardware initialization
 * @kms:        Pointer to msm_kms structure
 *
 * Maps the flat SDE reg-names (mdp_phys, vbif_phys, vbif_nrt_phys,
 * regdma_phys) to the same dpu_kms fields used by the upstream DPU
 * hw_init.  Then follows the same initialization sequence as
 * dpu_kms_hw_init(), with one difference: a stub DSI connector is
 * added before _dpu_kms_drm_obj_init() when no real DSI/DP bridge
 * is bound, ensuring at least one encoder/CRTC pair exists.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int dpu_sde_kms_hw_init(struct msm_kms *kms)
{
	struct dpu_kms *dpu_kms;
	struct drm_device *dev;
	struct msm_drm_private *priv;
	int i, rc = -EINVAL;

	if (!kms) {
		DPU_ERROR("invalid kms\n");
		return rc;
	}

	dpu_kms = to_dpu_kms(kms);
	dev = dpu_kms->dev;
	priv = dev->dev_private;

	rc = dpu_kms_global_obj_init(dpu_kms);
	if (rc)
		return rc;

	atomic_set(&dpu_kms->bandwidth_ref, 0);

	/*
	 * Flat SDE DT uses _phys suffix reg-names.  Map them to the
	 * same dpu_kms fields that the upstream DPU DT uses.
	 */
	dpu_kms->mmio = msm_ioremap(dpu_kms->pdev, "mdp_phys", "mdp_phys");
	if (IS_ERR(dpu_kms->mmio)) {
		rc = PTR_ERR(dpu_kms->mmio);
		DPU_ERROR("mdp_phys register memory map failed: %d\n", rc);
		dpu_kms->mmio = NULL;
		goto error;
	}
	DRM_DEBUG("mapped dpu address space @%pK\n", dpu_kms->mmio);

	dpu_kms->vbif[VBIF_RT] = msm_ioremap(dpu_kms->pdev, "vbif_phys",
			"vbif_phys");
	if (IS_ERR(dpu_kms->vbif[VBIF_RT])) {
		rc = PTR_ERR(dpu_kms->vbif[VBIF_RT]);
		DPU_ERROR("vbif_phys register memory map failed: %d\n", rc);
		dpu_kms->vbif[VBIF_RT] = NULL;
		goto error;
	}

	dpu_kms->vbif[VBIF_NRT] = msm_ioremap_quiet(dpu_kms->pdev,
			"vbif_nrt_phys",
			"vbif_nrt_phys");
	if (IS_ERR(dpu_kms->vbif[VBIF_NRT])) {
		dpu_kms->vbif[VBIF_NRT] = NULL;
		DPU_DEBUG("VBIF NRT is not defined");
	}

	dpu_kms->reg_dma = msm_ioremap_quiet(dpu_kms->pdev, "regdma_phys",
			"regdma_phys");
	if (IS_ERR(dpu_kms->reg_dma)) {
		dpu_kms->reg_dma = NULL;
		DPU_DEBUG("REG_DMA is not defined");
	}

	dpu_kms_parse_data_bus_icc_path(dpu_kms);

	rc = pm_runtime_resume_and_get(&dpu_kms->pdev->dev);
	if (rc < 0)
		goto error;

	dpu_kms->core_rev = readl_relaxed(dpu_kms->mmio + 0x0);

	pr_info("dpu hardware revision:0x%x\n", dpu_kms->core_rev);

	dpu_kms->catalog = dpu_hw_catalog_init(dpu_kms->core_rev);
	if (IS_ERR_OR_NULL(dpu_kms->catalog)) {
		rc = PTR_ERR(dpu_kms->catalog);
		if (!dpu_kms->catalog)
			rc = -EINVAL;
		DPU_ERROR("catalog init failed: %d\n", rc);
		dpu_kms->catalog = NULL;
		goto power_error;
	}

	rc = _dpu_kms_mmu_init(dpu_kms);
	if (rc) {
		DPU_ERROR("dpu_kms_mmu_init failed: %d\n", rc);
		goto power_error;
	}

	rc = dpu_rm_init(&dpu_kms->rm, dpu_kms->catalog, dpu_kms->mmio);
	if (rc) {
		DPU_ERROR("rm init failed: %d\n", rc);
		goto power_error;
	}

	dpu_kms->rm_init = true;

	dpu_kms->hw_mdp = dpu_hw_mdptop_init(MDP_TOP, dpu_kms->mmio,
					     dpu_kms->catalog);
	if (IS_ERR(dpu_kms->hw_mdp)) {
		rc = PTR_ERR(dpu_kms->hw_mdp);
		DPU_ERROR("failed to get hw_mdp: %d\n", rc);
		dpu_kms->hw_mdp = NULL;
		goto power_error;
	}

	for (i = 0; i < dpu_kms->catalog->vbif_count; i++) {
		u32 vbif_idx = dpu_kms->catalog->vbif[i].id;

		dpu_kms->hw_vbif[vbif_idx] = dpu_hw_vbif_init(vbif_idx,
				dpu_kms->vbif[vbif_idx], dpu_kms->catalog);
		if (IS_ERR_OR_NULL(dpu_kms->hw_vbif[vbif_idx])) {
			rc = PTR_ERR(dpu_kms->hw_vbif[vbif_idx]);
			if (!dpu_kms->hw_vbif[vbif_idx])
				rc = -EINVAL;
			DPU_ERROR("failed to init vbif %d: %d\n", vbif_idx, rc);
			dpu_kms->hw_vbif[vbif_idx] = NULL;
			goto power_error;
		}
	}

	rc = dpu_core_perf_init(&dpu_kms->perf, dev, dpu_kms->catalog,
			_dpu_kms_get_clk(dpu_kms, "core"));
	if (rc) {
		DPU_ERROR("failed to init perf %d\n", rc);
		goto perf_err;
	}

	dpu_kms->hw_intr = dpu_hw_intr_init(dpu_kms->mmio, dpu_kms->catalog);
	if (IS_ERR_OR_NULL(dpu_kms->hw_intr)) {
		rc = PTR_ERR(dpu_kms->hw_intr);
		DPU_ERROR("hw_intr init failed: %d\n", rc);
		dpu_kms->hw_intr = NULL;
		goto hw_intr_init_err;
	}

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;

	dev->mode_config.max_width =
			dpu_kms->catalog->caps->max_mixer_width * 2;
	dev->mode_config.max_height = 4096;

	dev->mode_config.allow_fb_modifiers = true;

	/*
	 * Initialize real DSI/DP displays.  priv->dsi[] is already
	 * populated by component_bind_all() which ran before this
	 * hw_init.  If no display bridge is bound (headless dev),
	 * fall back to a stub connector so DRM object init can
	 * still create at least one encoder/CRTC pair.
	 */
	rc = _dpu_kms_setup_displays(dev, priv, dpu_kms);
	if (rc) {
		DPU_ERROR("display setup failed: %d\n", rc);
		goto drm_obj_init_err;
	}

	if (!priv->num_encoders) {
		rc = _dpu_sde_kms_setup_stub_connector(dpu_kms);
		if (rc) {
			DPU_ERROR("stub connector setup failed: %d\n", rc);
			goto drm_obj_init_err;
		}
	}

	rc = _dpu_kms_drm_obj_init(dpu_kms);
	if (rc) {
		DPU_ERROR("modeset init failed: %d\n", rc);
		goto drm_obj_init_err;
	}

	dpu_vbif_init_memtypes(dpu_kms);

	pm_runtime_put_sync(&dpu_kms->pdev->dev);

	return 0;

drm_obj_init_err:
	_dpu_kms_drm_obj_destroy(dpu_kms);
	dpu_core_perf_destroy(&dpu_kms->perf);
hw_intr_init_err:
perf_err:
power_error:
	pm_runtime_put_sync(&dpu_kms->pdev->dev);
error:
	_dpu_kms_hw_destroy(dpu_kms);

	return rc;
}

/**
 * dpu_sde_kms_init - Allocate and initialize the SDE flat KMS backend
 * @ddev:        Pointer to DRM device
 * @sde_pdev:    Pointer to SDE platform device (qcom,sde-kms)
 *
 * Allocates a dpu_kms structure, sets up OPP table and clocks,
 * initializes the msm_kms base with the SDE vtable (sde_kms_funcs),
 * enables runtime PM, and maps the SDE interrupt.  The actual
 * hardware initialization is deferred to dpu_sde_kms_hw_init(),
 * which is called by the DRM framework via msm_kms_funcs.hw_init.
 *
 * Return: Pointer to initialized msm_kms, or ERR_PTR on failure.
 */
struct msm_kms *dpu_sde_kms_init(struct drm_device *ddev,
			   struct platform_device *sde_pdev)
{
	struct msm_drm_private *priv;
	struct dpu_kms *dpu_kms;
	struct dss_module_power *mp;
	int irq, ret = 0;

	if (!ddev || !sde_pdev) {
		DPU_ERROR("invalid params\n");
		return ERR_PTR(-EINVAL);
	}

	priv = ddev->dev_private;

	dpu_kms = devm_kzalloc(&sde_pdev->dev, sizeof(*dpu_kms), GFP_KERNEL);
	if (!dpu_kms)
		return ERR_PTR(-ENOMEM);

	dpu_kms->opp_table = dev_pm_opp_set_clkname(&sde_pdev->dev, "core");
	if (IS_ERR(dpu_kms->opp_table))
		return ERR_PTR(PTR_ERR(dpu_kms->opp_table));

	/* OPP table is optional */
	ret = dev_pm_opp_of_add_table(&sde_pdev->dev);
	if (!ret) {
		dpu_kms->has_opp_table = true;
	} else if (ret != -ENODEV) {
		dev_err(&sde_pdev->dev, "invalid OPP table in device tree\n");
		dev_pm_opp_put_clkname(dpu_kms->opp_table);
		return ERR_PTR(ret);
	}

	mp = &dpu_kms->mp;
	ret = msm_dss_parse_clock(sde_pdev, mp);
	if (ret) {
		DPU_ERROR("failed to parse clocks, ret=%d\n", ret);
		goto err_opp;
	}

	platform_set_drvdata(sde_pdev, dpu_kms);

	msm_kms_init(&dpu_kms->base, &sde_kms_funcs);
	dpu_kms->dev = ddev;
	dpu_kms->pdev = sde_pdev;

	pm_runtime_enable(&sde_pdev->dev);
	dpu_kms->rpm_enabled = true;

	irq = irq_of_parse_and_map(sde_pdev->dev.of_node, 0);
	if (irq < 0) {
		DPU_ERROR("failed to get irq: %d\n", irq);
		ret = irq;
		goto err_pm_runtime;
	}
	dpu_kms->base.irq = irq;

	priv->kms = &dpu_kms->base;

	return &dpu_kms->base;

err_pm_runtime:
	pm_runtime_disable(&sde_pdev->dev);
	dpu_kms->rpm_enabled = false;
	msm_dss_put_clk(mp->clk_config, mp->num_clk);
	devm_kfree(&sde_pdev->dev, mp->clk_config);
	mp->num_clk = 0;
err_opp:
	if (dpu_kms->has_opp_table)
		dev_pm_opp_of_remove_table(&sde_pdev->dev);
	dev_pm_opp_put_clkname(dpu_kms->opp_table);
	return ERR_PTR(ret);
}

static int dpu_bind(struct device *dev, struct device *master, void *data)
{
	struct drm_device *ddev = dev_get_drvdata(master);
	struct platform_device *pdev = to_platform_device(dev);
	struct msm_drm_private *priv = ddev->dev_private;
	struct dpu_kms *dpu_kms;
	struct dss_module_power *mp;
	int ret = 0;

	dpu_kms = devm_kzalloc(&pdev->dev, sizeof(*dpu_kms), GFP_KERNEL);
	if (!dpu_kms)
		return -ENOMEM;

	dpu_kms->opp_table = dev_pm_opp_set_clkname(dev, "core");
	if (IS_ERR(dpu_kms->opp_table))
		return PTR_ERR(dpu_kms->opp_table);
	/* OPP table is optional */
	ret = dev_pm_opp_of_add_table(dev);
	if (!ret) {
		dpu_kms->has_opp_table = true;
	} else if (ret != -ENODEV) {
		dev_err(dev, "invalid OPP table in device tree\n");
		dev_pm_opp_put_clkname(dpu_kms->opp_table);
		return ret;
	}

	mp = &dpu_kms->mp;
	ret = msm_dss_parse_clock(pdev, mp);
	if (ret) {
		DPU_ERROR("failed to parse clocks, ret=%d\n", ret);
		goto err;
	}

	platform_set_drvdata(pdev, dpu_kms);

	msm_kms_init(&dpu_kms->base, &kms_funcs);
	dpu_kms->dev = ddev;
	dpu_kms->pdev = pdev;

	pm_runtime_enable(&pdev->dev);
	dpu_kms->rpm_enabled = true;

	priv->kms = &dpu_kms->base;
	return ret;
err:
	if (dpu_kms->has_opp_table)
		dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_put_clkname(dpu_kms->opp_table);
	return ret;
}

static void dpu_unbind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dpu_kms *dpu_kms = platform_get_drvdata(pdev);
	struct dss_module_power *mp = &dpu_kms->mp;

	msm_dss_put_clk(mp->clk_config, mp->num_clk);
	devm_kfree(&pdev->dev, mp->clk_config);
	mp->num_clk = 0;

	if (dpu_kms->rpm_enabled)
		pm_runtime_disable(&pdev->dev);

	if (dpu_kms->has_opp_table)
		dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_put_clkname(dpu_kms->opp_table);
}

static const struct component_ops dpu_ops = {
	.bind   = dpu_bind,
	.unbind = dpu_unbind,
};

static int dpu_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &dpu_ops);
}

static int dpu_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dpu_ops);
	return 0;
}

static int __maybe_unused dpu_runtime_suspend(struct device *dev)
{
	int i, rc = -1;
	struct platform_device *pdev = to_platform_device(dev);
	struct dpu_kms *dpu_kms = platform_get_drvdata(pdev);
	struct dss_module_power *mp = &dpu_kms->mp;

	/* Drop the performance state vote */
	dev_pm_opp_set_rate(dev, 0);
	rc = msm_dss_enable_clk(mp->clk_config, mp->num_clk, false);
	if (rc)
		DPU_ERROR("clock disable failed rc:%d\n", rc);

	for (i = 0; i < dpu_kms->num_paths; i++)
		icc_set_bw(dpu_kms->path[i], 0, 0);

	return rc;
}

static int __maybe_unused dpu_runtime_resume(struct device *dev)
{
	int rc = -1;
	struct platform_device *pdev = to_platform_device(dev);
	struct dpu_kms *dpu_kms = platform_get_drvdata(pdev);
	struct drm_encoder *encoder;
	struct drm_device *ddev;
	struct dss_module_power *mp = &dpu_kms->mp;
	int i;

	ddev = dpu_kms->dev;

	WARN_ON(!(dpu_kms->num_paths));
	/* Min vote of BW is required before turning on AXI clk */
	for (i = 0; i < dpu_kms->num_paths; i++)
		icc_set_bw(dpu_kms->path[i], 0, Bps_to_icc(MIN_IB_BW));

	rc = msm_dss_enable_clk(mp->clk_config, mp->num_clk, true);
	if (rc) {
		DPU_ERROR("clock enable failed rc:%d\n", rc);
		return rc;
	}

	dpu_vbif_init_memtypes(dpu_kms);

	drm_for_each_encoder(encoder, ddev)
		dpu_encoder_virt_runtime_resume(encoder);

	return rc;
}

static const struct dev_pm_ops dpu_pm_ops = {
	SET_RUNTIME_PM_OPS(dpu_runtime_suspend, dpu_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static const struct of_device_id dpu_dt_match[] = {
	{ .compatible = "qcom,sdm845-dpu", },
	{ .compatible = "qcom,sc7180-dpu", },
	{}
};
MODULE_DEVICE_TABLE(of, dpu_dt_match);

static struct platform_driver dpu_driver = {
	.probe = dpu_dev_probe,
	.remove = dpu_dev_remove,
	.driver = {
		.name = "msm_dpu",
		.of_match_table = dpu_dt_match,
		.pm = &dpu_pm_ops,
	},
};

void __init msm_dpu_register(void)
{
	platform_driver_register(&dpu_driver);
}

void __exit msm_dpu_unregister(void)
{
	platform_driver_unregister(&dpu_driver);
}
