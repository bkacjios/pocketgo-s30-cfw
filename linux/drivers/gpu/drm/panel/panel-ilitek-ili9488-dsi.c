// SPDX-License-Identifier: GPL-2.0
/*
 * Ilitek ILI9488 320x480 MIPI-DSI panel, as found in the PocketGo S30.
 *
 * The init sequence, reset timing and video timings were recovered from the
 * stock Zkswe Linux 3.4 kernel (sunxi disp2 "ili9488" LCD driver).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct ili9488 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset;
	enum drm_panel_orientation orientation;
	bool handed_over; /* first prepare done: U-Boot's state no longer applies */
};

static inline struct ili9488 *panel_to_ili9488(struct drm_panel *panel)
{
	return container_of(panel, struct ili9488, panel);
}

static int ili9488_init_sequence(struct ili9488 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Positive / negative gamma control */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe0, 0x00, 0x10, 0x14, 0x01, 0x0e,
				     0x04, 0x33, 0x56, 0x48, 0x03, 0x0c, 0x0b,
				     0x2b, 0x34, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1, 0x00, 0x12, 0x18, 0x05, 0x12,
				     0x06, 0x40, 0x34, 0x57, 0x06, 0x10, 0x0c,
				     0x3b, 0x3f, 0x0f);
	/* Power control 1/2, VCOM control */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x0f, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x00, 0x26, 0x80);
	/* MADCTL: column address order mirrored, BGR */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_ADDRESS_MODE, 0x48);
	/* COLMOD: 18 bpp on both interfaces */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_PIXEL_FORMAT, 0x66);
	/* Interface mode, frame rate, inversion, display function control */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb1, 0xa0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x02, 0x02);
	/* Set image function: 24-bit data bus off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe9, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_ENTER_INVERT_MODE);
	/* Adjust control 3 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0xa9, 0x51, 0x2c, 0x82);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	return dsi_ctx.accum_err;
}

/*
 * U-Boot initializes the panel for its splash; the boot script then passes
 * panel_ilitek_ili9488_dsi.handoff=1 so the first prepare keeps it running
 * instead of resetting it (a visible blank between the U-Boot and kernel
 * images). DSI reads (DCS get_power_mode) return 0 on this host, so the state
 * can't be read back from the panel.
 */
static bool handoff;
module_param(handoff, bool, 0444);
MODULE_PARM_DESC(handoff, "Panel left initialized by the bootloader");

static bool ili9488_left_on(struct ili9488 *ctx)
{
	if (ctx->handed_over)
		return false;
	ctx->handed_over = true;
	return handoff;
}

static int ili9488_prepare(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return ret;
	if (ili9488_left_on(ctx))
		return 0;
	msleep(30);

	/* Stock firmware: release, 50 ms, assert, 20 ms, release, 100 ms */
	gpiod_direction_output(ctx->reset, 0);
	msleep(50);
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(100);

	ret = ili9488_init_sequence(ctx);
	if (ret) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	}

	return ret;
}

static int ili9488_unprepare(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

/*
 * Stock fex: lcd_x 320, lcd_y 480, lcd_dclk_freq 12, lcd_ht 382, lcd_hbp 40,
 * lcd_hspw 12, lcd_vt 524, lcd_vbp 22, lcd_vspw 4. Allwinner's hbp/vbp include
 * the sync width, so front porch = total - active - hbp.
 */
static const struct drm_display_mode ili9488_mode = {
	/*
	 * Stock uses 12 MHz, but on the A33 that makes the clk framework set
	 * PLL-MIPI to an N/K/M combination (0x90c00d1d for "288 MHz") that
	 * garbles the picture. 12.375 MHz = 297 MHz / 24 bpp / 1 lane, so
	 * PLL-MIPI runs at x1 of the 297 MHz PLL-video, like stock firmware
	 * and U-Boot do. With stock's 524 lines that is 61.8 Hz; 16 more
	 * lines of vertical front porch make it 59.99 Hz, the N64/NTSC
	 * rate the ports pace to (verified on the panel).
	 */
	.clock		= 12375,

	.hdisplay	= 320,
	.hsync_start	= 320 + 22,
	.hsync_end	= 320 + 22 + 12,
	.htotal		= 382,

	.vdisplay	= 480,
	.vsync_start	= 480 + 38,
	.vsync_end	= 480 + 38 + 4,
	.vtotal		= 540,

	.width_mm	= 49,
	.height_mm	= 74,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

/*
 * Other refresh rates, for apps that pace to them: the same clock and lines,
 * and the vertical front porch lengthened or shortened (the back porch and
 * sync stay). 61.8 Hz is the stock timing, 59.77 Hz the GBA's 59.73, 50 Hz
 * PAL.
 */
static const u16 ili9488_rate_vtotals[] = { 524, 542, 648 };

static int ili9488_add_rates(struct drm_connector *connector)
{
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(ili9488_rate_vtotals); i++) {
		struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &ili9488_mode);
		int back = ili9488_mode.vtotal - ili9488_mode.vsync_end;
		int sync = ili9488_mode.vsync_end - ili9488_mode.vsync_start;

		if (!mode)
			break;
		mode->vtotal = ili9488_rate_vtotals[i];
		mode->vsync_end = mode->vtotal - back;
		mode->vsync_start = mode->vsync_end - sync;
		mode->type = DRM_MODE_TYPE_DRIVER;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
		n++;
	}
	return n;
}

static int ili9488_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);

	connector->display_info.width_mm = ili9488_mode.width_mm;
	connector->display_info.height_mm = ili9488_mode.height_mm;
	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return drm_connector_helper_get_modes_fixed(connector, &ili9488_mode) + ili9488_add_rates(connector);
}

static enum drm_panel_orientation ili9488_get_orientation(struct drm_panel *panel)
{
	return panel_to_ili9488(panel)->orientation;
}

static const struct drm_panel_funcs ili9488_funcs = {
	.prepare	= ili9488_prepare,
	.unprepare	= ili9488_unprepare,
	.get_modes	= ili9488_get_modes,
	.get_orientation = ili9488_get_orientation,
};

static int ili9488_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9488 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ili9488, panel, &ili9488_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->supplies[0].supply = "power";
	ctx->supplies[1].supply = "iovcc";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* As is: U-Boot may have left the panel running (see prepare) */
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset),
				     "failed to get reset gpio\n");

	ret = drm_of_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	/* Stock: lcd_dsi_if 0 (video), lcd_dsi_lane 1, lcd_dsi_format 0, eotp 0 */
	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void ili9488_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ili9488 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ili9488_of_match[] = {
	{ .compatible = "ilitek,ili9488-dsi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ili9488_of_match);

static struct mipi_dsi_driver ili9488_dsi_driver = {
	.probe = ili9488_dsi_probe,
	.remove = ili9488_dsi_remove,
	.driver = {
		.name = "panel-ilitek-ili9488-dsi",
		.of_match_table = ili9488_of_match,
	},
};
module_mipi_dsi_driver(ili9488_dsi_driver);

MODULE_DESCRIPTION("Ilitek ILI9488 MIPI-DSI panel driver");
MODULE_LICENSE("GPL");
