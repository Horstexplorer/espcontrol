// The single touch point for registering a vendored sensor's SensorExtension (see
// mipi_csi_camera_sensor_extension.h). Adding support for another sensor that isn't in
// Espressif's `esp_cam_sensor` managed component registry means adding one `case` here -
// mipi_csi_camera.cpp/.h stay completely generic.

#include "mipi_csi_camera_sensor_extension.h"

#include "ov02c10_esphome.h"

namespace esphome::mipi_csi_camera {

SensorExtension *get_sensor_extension(MipiCsiSensorModel model) {
  switch (model) {
    case MIPI_CSI_SENSOR_OV02C10: {
      static ov02c10::Ov02c10Extension instance;
      return &instance;
    }
    case MIPI_CSI_SENSOR_SC2336:
    case MIPI_CSI_SENSOR_OV5647:
      // Fully handled by Espressif's `esp_cam_sensor` managed component; no extension needed.
      return nullptr;
  }
  return nullptr;
}

}  // namespace esphome::mipi_csi_camera
