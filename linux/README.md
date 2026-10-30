# PocketGo S30 mainline Linux bits

Files here drop into a mainline kernel tree (written against 6.15+ APIs:
`devm_drm_panel_alloc`, `mipi_dsi_*_multi`).

## Panel

ILI9488, 320x480, MIPI-DSI 1 lane, video mode, RGB888, 12 MHz pixel clock.
Recovered from the stock kernel (`firmware-decoded/ili9488_init.txt`).

Add to `drivers/gpu/drm/panel/Kconfig`:

```
config DRM_PANEL_ILITEK_ILI9488_DSI
	tristate "Ilitek ILI9488 MIPI-DSI panel"
	depends on OF
	depends on DRM_MIPI_DSI
	depends on BACKLIGHT_CLASS_DEVICE
	help
	  Say Y here to support the Ilitek ILI9488 320x480 MIPI-DSI panel
	  used in the PocketGo S30.
```

Add to `drivers/gpu/drm/panel/Makefile`:

```
obj-$(CONFIG_DRM_PANEL_ILITEK_ILI9488_DSI) += panel-ilitek-ili9488-dsi.o
```

Add `sun8i-a33-pocketgo-s30.dtb` to `arch/arm/boot/dts/allwinner/Makefile`
under `dtb-$(CONFIG_MACH_SUN8I)`.

Kernel config needs at least: `CONFIG_DRM_SUN4I`, `CONFIG_DRM_SUN6I_DSI`,
`CONFIG_PHY_SUN6I_MIPI_DPHY`, `CONFIG_DRM_PANEL_ILITEK_ILI9488_DSI`,
`CONFIG_PWM_SUN4I`, `CONFIG_BACKLIGHT_PWM`, `CONFIG_SUNXI_RSB`,
`CONFIG_MFD_AXP20X_RSB`, `CONFIG_REGULATOR_AXP20X`, `CONFIG_KEYBOARD_GPIO_POLLED`.

## Open items

- Panel `rotation` (90 vs 270) and backlight PWM polarity are unverified.
- Button-to-pin mapping is unknown; keys are `BTN_TRIGGER_HAPPY1..21` in fex order.
- Physical size 49x74 mm is the 3.5" nominal; the fex value (150x94) is a template leftover.
