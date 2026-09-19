#pragma once

// Generic extension point for sensor-specific behavior that doesn't belong in the
// resolution/format/rotation-agnostic V4L2 capture pipeline (mipi_csi_camera.cpp/.h).
//
// Sensors that are fully supported through Espressif's `esp_cam_sensor` managed component
// (SC2336/OV5647) don't need one of these at all - get_sensor_extension() returns nullptr for
// them, and mipi_csi_camera.cpp simply skips every hook. Sensors that are vendored directly into
// this component instead (because they aren't published in that managed component registry, e.g.
// OV02C10) implement this interface to plug into setup()/loop() without the generic driver code
// needing to know anything sensor-specific.
//
// To add support for another vendored sensor in the future:
//   1. Add its driver files, following the same flat/prefixed layout as the OV02C10 files
//      (`<name>_*.{c,h}`) - see README.md for why they can't live in a subfolder.
//   2. Implement this interface for it (see ov02c10_esphome.h/.cpp for the reference shape).
//   3. Add one case to get_sensor_extension()'s switch in sensor_extensions.cpp.
//   4. Add it to the Python side (SENSOR_MODELS/SUPPORTED_MODES/etc in __init__.py).
// Nothing else in mipi_csi_camera.cpp/.h needs to change.

#include <cstddef>
#include <cstdint>

#include "mipi_csi_camera.h"

namespace esphome::mipi_csi_camera {

class SensorExtension {
 public:
  virtual ~SensorExtension() = default;

  /// Called once from MipiCsiCamera::setup(), before esp_video_init(). `esp_cam_sensor`
  /// discovers cameras by iterating a runtime array of every `ESP_CAM_SENSOR_DETECT_FN`-
  /// registered detect function that the *linker* actually kept in the final binary.
  /// Espressif's own sensors (SC2336, OV5647, ...) get force-linked automatically by their own
  /// component's build scripts (`-u <sensor>_detect`); nothing does this for a sensor vendored
  /// directly into this component, so the linker would otherwise silently drop its driver object
  /// file entirely (no compile/link error - it just never gets called, and the sensor is never
  /// found). Implementations should do whatever's needed to force their driver's object file to
  /// stay linked in; see ov02c10_esphome.cpp for the established pattern. Default: no-op.
  virtual void force_link() {}

  /// Called from MipiCsiCamera::loop() for every captured frame, after de-striding/rotation, only
  /// while pixel_format == RGB565. Lets a sensor extension correct for a known, sensor-specific
  /// color/tone issue in the ESP32-P4 ISP's uncorrected demosaic output (see
  /// dev-docs/mipi-csi-camera-plan.md for why OV02C10 needs this, and why esp_ipa's automatic
  /// pipeline can't be used instead). `num_pixels` is the number of 16-bit RGB565 pixels in
  /// `pixels`, not the byte count. Default: no-op (no correction needed/available).
  virtual void apply_rgb565_color_correction(uint16_t *pixels, size_t num_pixels) {}
};

/// Returns the SensorExtension for `model`, or nullptr if that sensor doesn't need one (i.e. it's
/// fully handled by Espressif's `esp_cam_sensor` managed component). Implemented in
/// sensor_extensions.cpp, the single touch point for registering a new vendored sensor's
/// extension.
SensorExtension *get_sensor_extension(MipiCsiSensorModel model);

}  // namespace esphome::mipi_csi_camera
