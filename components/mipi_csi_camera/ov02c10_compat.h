/*
 * Compatibility shim for vendoring Espressif's OV02C10 esp_cam_sensor driver
 * outside of its usual Kconfig/menuconfig-driven build (the sensor isn't
 * published in the `espressif/esp_cam_sensor` managed component registry
 * this component otherwise depends on for SC2336/OV5647 support, so it can't
 * pick up its Kconfig options the normal way).
 *
 * Fixed knobs the upstream Kconfig would otherwise offer as tunables get a
 * sensible constant default here. The three "which capture mode is enabled"
 * options (CONFIG_CAMERA_OV02C10_MIPI_RAW10_...) and
 * CONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DEFAULT are intentionally NOT
 * defined here: mipi_csi_camera's Python codegen supplies exactly one of them
 * (matching the user's configured `resolution`/`data_lanes`) as a build flag,
 * so ov02c10_format_info_mipi[] ends up with exactly one entry.
 */
#pragma once

// Referenced by the driver but not actually used elsewhere in it; any small
// positive value is fine.
#define CONFIG_CAMERA_OV02C10_MAX_SUPPORT 1

// Matches upstream Kconfig's own default (see Kconfig.ov02c10:
// CAMERA_OV02C10_ABSOLUTE_GAIN_LIMIT, default 66016 = 66.016x).
#define CONFIG_CAMERA_OV02C10_ABSOLUTE_GAIN_LIMIT 66016
