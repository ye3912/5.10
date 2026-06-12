// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_screenlog.c — Render oplus_logbuf ring buffer to DRM display
 *
 * Uses the DRM client API to draw the last N lines of the printk ring
 * buffer directly on the device display.  No dependency on fbdev,
 * userspace, or filesystem — works as soon as the DPU/DRM driver
 * has probed and the display pipeline is active.
 *
 * A public helper oplus_screenlog_refresh() is exported for the
 * hung_task detector to force an immediate screen update.
 *
 * Requires: CONFIG_FONT_8x16=y (for font_vga_8x16)
 *
 * Copyright (c) 2025, OPLUS SM8250 5.10 port
 */

#include <drm/drm_client.h>
#include <drm/drm_fourcc.h>
#include <linux/delay.h>
#include <linux/font.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/oplus_logbuf.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SCREENLOG_LINES   35
#define SCREENLOG_DRM_SCAN_REPORT_INTERVAL	10
#define SCREENLOG_DRM_CANDIDATE_LIMIT		5

static struct drm_client_dev screenlog_client;
static struct drm_client_buffer *screenlog_buffer;
static struct task_struct *screenlog_thread;
static u32 screen_width, screen_height;

struct screenlog_drm_scan_report {
	unsigned int count;
	bool limit_reached;
};

/* ── find MSM / DPU drm_device via platform bus ─────────── */

/**
 * screenlog_first_compatible() - Return a device node's first compatible
 * @dev: device to inspect
 *
 * The DT compatible property is a NUL-separated string list.  For debug
 * output the first entry is enough to identify which display node was found.
 *
 * Return: first compatible string, or "<none>" when absent.
 */
static const char *screenlog_first_compatible(struct device *dev)
{
	const char *compat;

	if (!dev->of_node)
		return "<none>";

	compat = of_get_property(dev->of_node, "compatible", NULL);
	if (!compat)
		return "<none>";

	return compat;
}

/**
 * screenlog_is_drm_candidate() - Check whether an OF node may host MSM DRM
 * @dev: device to inspect
 *
 * Keep this list intentionally narrow: it mirrors the compatibles that the
 * 5.10 MSM DRM path can use, plus Kona downstream's qcom,sde-kms node.
 *
 * Return: true if the platform device is display-related.
 */
static bool screenlog_is_drm_candidate(struct device *dev)
{
	if (!dev->of_node)
		return false;

	return of_device_is_compatible(dev->of_node, "qcom,sde-kms") ||
	       of_device_is_compatible(dev->of_node, "qcom,sdm845-mdss") ||
	       of_device_is_compatible(dev->of_node, "qcom,sc7180-mdss");
}

/**
 * screenlog_of_match() - bus_find_device() matcher for MSM display nodes
 * @dev: platform device visited by the bus iterator
 * @data: unused matcher data
 *
 * Return: non-zero when @dev is a supported DRM platform candidate.
 */
static int screenlog_of_match(struct device *dev, const void *data)
{
	return screenlog_is_drm_candidate(dev);
}

/**
 * screenlog_report_drm_candidate() - Log one display candidate device
 * @dev: platform device visited by bus_for_each_dev()
 * @data: struct screenlog_drm_scan_report accumulator
 *
 * Return: 1 when the candidate log limit is reached, otherwise 0.
 */
static int screenlog_report_drm_candidate(struct device *dev, void *data)
{
	struct screenlog_drm_scan_report *report = data;

	if (!screenlog_is_drm_candidate(dev))
		return 0;

	pr_info("oplus_screenlog: drm candidate %u: %s compatible=%s drvdata=%s\n",
		++report->count, dev_name(dev), screenlog_first_compatible(dev),
		dev_get_drvdata(dev) ? "set" : "unset");

	if (report->count >= SCREENLOG_DRM_CANDIDATE_LIMIT) {
		report->limit_reached = true;
		return 1;
	}

	return 0;
}

/**
 * screenlog_dump_drm_candidates() - Dump display platform candidates
 *
 * This diagnostic is deliberately limited to avoid flooding early boot logs
 * while still proving whether the Kona qcom,sde-kms node exists as a platform
 * device and whether any driver has attached private data to it.
 */
static void screenlog_dump_drm_candidates(void)
{
	struct screenlog_drm_scan_report report = { 0 };

	bus_for_each_dev(&platform_bus_type, NULL, &report,
			 screenlog_report_drm_candidate);

	if (!report.count)
		pr_info("oplus_screenlog: no DRM platform candidates found\n");
	else if (report.limit_reached)
		pr_info("oplus_screenlog: DRM candidate list truncated at %u\n",
			SCREENLOG_DRM_CANDIDATE_LIMIT);
}

/**
 * screenlog_find_drm() - Find a registered DRM device for screen logging
 *
 * The screen logger is started before userspace, so it cannot depend on fbdev
 * or Android services.  It scans platform devices for the display controller
 * and uses driver data only after the DRM device has been registered.
 *
 * Return: registered DRM device, or NULL when display DRM is not ready.
 */
static struct drm_device *screenlog_find_drm(void)
{
	struct device *dev;
	struct drm_device *ddev;

	dev = bus_find_device(&platform_bus_type, NULL, NULL,
			      screenlog_of_match);
	if (!dev)
		return NULL;

	ddev = dev_get_drvdata(dev);
	if (!ddev) {
		pr_info_ratelimited("oplus_screenlog: %s compatible=%s has no DRM drvdata\n",
				    dev_name(dev), screenlog_first_compatible(dev));
		put_device(dev);
		return NULL;
	}

	if (!ddev->registered) {
		pr_info_ratelimited("oplus_screenlog: %s compatible=%s DRM not registered\n",
				    dev_name(dev), screenlog_first_compatible(dev));
		put_device(dev);
		return NULL;
	}

	put_device(dev);

	return ddev;
}

/* ── framebuffer rendering ────────────────────────────── */

static void screenlog_clear(void)
{
	u32 *fb = screenlog_buffer->vaddr;
	u32 pitch_words = screenlog_buffer->pitch / 4;
	u32 row;

	for (row = 0; row < screen_height; row++)
		memset32(fb + row * pitch_words, 0x00000000, screen_width);
}

static void screenlog_draw_char(u32 *fb, u32 pitch_words,
				u32 x, u32 y, char c)
{
	const struct font_desc *font = &font_vga_8x16;
	const u8 *src;
	u32 *dst;
	int row, col;

	if ((signed char)c < 0)
		c = '?';
	src = font->data + ((unsigned char)c * font->height);

	for (row = 0; row < font->height; row++) {
		u8 bits = *src++;
		dst = fb + (y + row) * pitch_words + x;
		for (col = 0; col < font->width; col++)
			dst[col] = (bits & (1 << (7 - col))) ?
				   0xFF00FF00 :  /* green */
				   0xFF000000;   /* black */
	}
}

/*
 * Build a string containing the last SCREENLOG_LINES lines from the
 * ring buffer.  Caller must kfree() the returned pointer.
 */
static char *screenlog_build_text(u32 *out_len)
{
	struct oplus_logbuf_hdr *hdr = oplus_logbuf;
	u32 total, start, i, lines;
	char *buf;

	if (!hdr || !hdr->size) {
		buf = kmalloc(64, GFP_KERNEL);
		if (buf)
			strcpy(buf, "(logbuf not ready)");
		*out_len = buf ? strlen(buf) : 0;
		return buf;
	}

	total = hdr->size;

	buf = kmalloc(total + 1, GFP_KERNEL);
	if (!buf) {
		*out_len = 0;
		return NULL;
	}

	/* Linearise: oldest byte → newest */
	start = (hdr->head + 1) % total;
	for (i = 0; i < total; i++) {
		buf[i] = hdr->data[start];
		start = (start + 1) % total;
	}
	buf[total] = '\0';

	/* Walk backwards to find line starts */
	lines = 0;
	start = 0;
	for (i = total; i > 0; i--) {
		if (buf[i - 1] == '\n') {
			lines++;
			if (lines >= SCREENLOG_LINES) {
				start = i;
				break;
			}
		}
	}

	*out_len = total - start;
	if (start > 0)
		memmove(buf, buf + start, *out_len);
	buf[*out_len] = '\0';

	return buf;
}

static void screenlog_render_text(const char *text, u32 len)
{
	u32 *fb = screenlog_buffer->vaddr;
	u32 pitch = screenlog_buffer->pitch / 4;
	u32 fw = font_vga_8x16.width;
	u32 fh = font_vga_8x16.height;
	u32 cols = screen_width / fw;
	u32 rows = screen_height / fh;
	u32 col = 0, row = 0;
	u32 i;

	screenlog_clear();

	for (i = 0; i < len && row < rows; i++) {
		if (text[i] == '\n' || col >= cols) {
			col = 0;
			row++;
			if (text[i] == '\n')
				continue;
			if (row >= rows)
				break;
		}
		screenlog_draw_char(fb, pitch, col * fw, row * fh, text[i]);
		col++;
	}
}

/* ── DRM client callbacks ─────────────────────────────── */

static int screenlog_client_hotplug(struct drm_client_dev *client)
{
	struct drm_mode_set *modeset;
	void *vaddr;
	int ret;

	/* Drop previous buffer if any */
	if (screenlog_buffer) {
		drm_client_buffer_vunmap(screenlog_buffer);
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
	}

	ret = drm_client_modeset_probe(client, 0, 0);
	if (ret) {
		pr_info("oplus_screenlog: probe failed: %d\n", ret);
		return ret;
	}

	/* Snapshot the first modeset's resolution (lock held) */
	mutex_lock(&client->modeset_mutex);
	screen_width = 0;
	screen_height = 0;
	drm_client_for_each_modeset(modeset, client) {
		if (modeset->mode) {
			screen_width  = modeset->mode->hdisplay;
			screen_height = modeset->mode->vdisplay;
			break;
		}
	}
	mutex_unlock(&client->modeset_mutex);

	if (!screen_width || !screen_height)
		return -ENODEV;

	/* Create framebuffer outside the lock */
	screenlog_buffer = drm_client_framebuffer_create(
		client, screen_width, screen_height,
		DRM_FORMAT_XRGB8888);
	if (IS_ERR(screenlog_buffer)) {
		pr_info("oplus_screenlog: fb create: %ld\n",
			PTR_ERR(screenlog_buffer));
		screenlog_buffer = NULL;
		return -ENOMEM;
	}

	/* Map framebuffer into kernel address space for CPU rendering */
	vaddr = drm_client_buffer_vmap(screenlog_buffer);
	if (IS_ERR(vaddr)) {
		pr_info("oplus_screenlog: vmap failed: %ld\n", PTR_ERR(vaddr));
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
		return -ENOMEM;
	}

	/* Assign FB to modeset (lock held) */
	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(modeset, client) {
		if (modeset->mode) {
			modeset->fb = screenlog_buffer->fb;
			break;
		}
	}
	mutex_unlock(&client->modeset_mutex);

	ret = drm_client_modeset_check(client);
	if (ret) {
		pr_info("oplus_screenlog: check: %d\n", ret);
		drm_client_buffer_vunmap(screenlog_buffer);
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
		return ret;
	}

	ret = drm_client_modeset_commit(client);
	if (ret) {
		pr_info("oplus_screenlog: commit: %d\n", ret);
		drm_client_buffer_vunmap(screenlog_buffer);
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
		return ret;
	}

	pr_info("oplus_screenlog: %ux%u buffer ready\n",
		screen_width, screen_height);
	return 0;
}

static int screenlog_client_restore(struct drm_client_dev *client)
{
	return screenlog_client_hotplug(client);
}

static void screenlog_client_unregister(struct drm_client_dev *client)
{
	if (screenlog_buffer) {
		drm_client_buffer_vunmap(screenlog_buffer);
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
	}
}

static const struct drm_client_funcs screenlog_client_funcs = {
	.owner		= THIS_MODULE,
	.hotplug	= screenlog_client_hotplug,
	.restore	= screenlog_client_restore,
	.unregister	= screenlog_client_unregister,
};

/* ── public helper ────────────────────────────────────── */

/**
 * oplus_screenlog_refresh() — Force an immediate screen refresh
 *
 * Reads the log buffer and draws it to the DRM framebuffer.
 * Called from hung_task detector.  NOP if the display is not ready.
 */
void oplus_screenlog_refresh(void)
{
	char *text;
	u32 len;

	if (!screenlog_buffer || !screenlog_buffer->vaddr)
		return;

	text = screenlog_build_text(&len);
	if (text) {
		screenlog_render_text(text, len);
		drm_client_framebuffer_flush(screenlog_buffer, NULL);
		kfree(text);
	}
}
EXPORT_SYMBOL_GPL(oplus_screenlog_refresh);

/* ── kernel thread ────────────────────────────────────── */

static int screenlog_thread_fn(void *data)
{
	struct drm_device *ddev;
	unsigned int scan_count = 0;
	int ret;

	/* Phase 1: wait for DRM device */
	while (!kthread_should_stop()) {
		ddev = screenlog_find_drm();
		if (ddev)
			break;

		if (!(++scan_count % SCREENLOG_DRM_SCAN_REPORT_INTERVAL)) {
			pr_info("oplus_screenlog: waiting for DRM device, scans=%u\n",
				scan_count);
			screenlog_dump_drm_candidates();
		}

		msleep_interruptible(1000);
	}

	if (kthread_should_stop() || !ddev)
		return 0;

	ret = drm_client_init(ddev, &screenlog_client,
			     "oplus_screenlog", &screenlog_client_funcs);
	if (ret) {
		pr_err("oplus_screenlog: client init: %d\n", ret);
		return ret;
	}

	ret = drm_client_modeset_create(&screenlog_client);
	if (ret) {
		pr_err("oplus_screenlog: modeset create: %d\n", ret);
		drm_client_release(&screenlog_client);
		return ret;
	}

	drm_client_register(&screenlog_client);

	/* Phase 2: wait for hotplug to create the buffer */
	while (!kthread_should_stop() && !screenlog_buffer)
		msleep_interruptible(500);

	/* Phase 3: render loop */
	while (!kthread_should_stop()) {
		char *text;
		u32 len;

		text = screenlog_build_text(&len);
		if (text) {
			screenlog_render_text(text, len);
			drm_client_framebuffer_flush(screenlog_buffer, NULL);
			kfree(text);
		}

		msleep_interruptible(2000);
	}

	return 0;
}

/* ── module init ──────────────────────────────────────── */

static int __init oplus_screenlog_init(void)
{
	screenlog_thread = kthread_run(screenlog_thread_fn, NULL,
				       "oplus_slog");
	if (IS_ERR(screenlog_thread)) {
		int ret = PTR_ERR(screenlog_thread);

		pr_err("oplus_screenlog: kthread: %d\n", ret);
		screenlog_thread = NULL;
		return ret;
	}

	pr_info("oplus_screenlog: thread started\n");
	return 0;
}

static void __exit oplus_screenlog_exit(void)
{
	if (screenlog_thread)
		kthread_stop(screenlog_thread);

	if (screenlog_buffer) {
		drm_client_buffer_vunmap(screenlog_buffer);
		drm_client_framebuffer_delete(screenlog_buffer);
		screenlog_buffer = NULL;
	}

	drm_client_release(&screenlog_client);
}

module_init(oplus_screenlog_init);
module_exit(oplus_screenlog_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OPLUS SM8250 5.10 port");
MODULE_DESCRIPTION("DRM screen log renderer for oplus_logbuf");
