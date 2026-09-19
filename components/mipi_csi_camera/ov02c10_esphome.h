#pragma once

// C++ adapter around the vendored, non-`esp_cam_sensor`-managed OV02C10 driver
// (`ov02c10.c`/`ov02c10*.h`). Implements the generic SensorExtension interface (see
// mipi_csi_camera_sensor_extension.h) so every OV02C10-specific detail - the linker keep-alive
// workaround and the fixed-gain color correction the ISP's uncorrected demosaic needs for this
// sensor - stays out of the generic mipi_csi_camera.cpp/.h entirely.
//
// NOTE: all files for a vendored sensor have to live flat in this component's top-level
// directory (prefixed `ov02c10_*`), not in a subfolder - ESPHome's external-component loader
// only picks up source files that sit directly inside the component's own directory, it does not
// recurse into subdirectories (confirmed by a failed scratch compile: files placed under
// `sensors/ov02c10/` were silently never copied into the build tree at all).

#include <cstddef>
#include <cstdint>

#include "mipi_csi_camera_sensor_extension.h"

namespace esphome::mipi_csi_camera::ov02c10 {

class Ov02c10Extension final : public SensorExtension {
 public:
  /// Forces the linker to keep `ov02c10.c`'s object file (and with it its
  /// `ESP_CAM_SENSOR_DETECT_FN` auto-registration entry), which would otherwise be dropped as
  /// unused since nothing else in the firmware references any symbol from it - unlike
  /// `esp_cam_sensor`-managed sensors (SC2336/OV5647), which get a `-u <sensor>_detect` linker
  /// force-reference from their own build scripts. See:
  /// https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/include/esp_cam_sensor_detect.h
  void force_link() override;

  /// Applies a fixed per-channel RGB565 gain correction (in place) to counteract the ESP32-P4
  /// ISP's uncorrected-demosaic green bias when running without the `esp_ipa`-driven auto
  /// white-balance pipeline (which needs per-sensor calibration data that doesn't exist for
  /// OV02C10 - see dev-docs/mipi-csi-camera-plan.md for why the automatic pipeline was tried and
  /// reverted).
  void apply_rgb565_color_correction(uint16_t *pixels, size_t num_pixels) override;
};

}  // namespace esphome::mipi_csi_camera::ov02c10
