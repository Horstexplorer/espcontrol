#include "ov02c10_esphome.h"

#include <algorithm>

#include "ov02c10.h"

namespace esphome::mipi_csi_camera::ov02c10 {

static void *const detect_keep_alive_ __attribute__((used)) = reinterpret_cast<void *>(&ov02c10_detect);

void force_link() {
  // Nothing to do at runtime - `detect_keep_alive_` above is what actually keeps the linker from
  // dropping ov02c10.c. This function exists only so the generic mipi_csi_camera.cpp has a
  // normal, named call site instead of needing to know about the keep-alive trick itself.
}

void apply_rgb565_color_correction(uint16_t *pixels, size_t num_pixels) {
  for (size_t i = 0; i < num_pixels; i++) {
    uint16_t px = pixels[i];
    uint32_t r = (px >> 11) & 0x1F;
    uint32_t g = (px >> 5) & 0x3F;
    uint32_t b = px & 0x1F;
    r = std::min<uint32_t>((r * 166) >> 7, 31);  // * 1.30
    g = std::min<uint32_t>((g * 115) >> 7, 63);  // * 0.90
    b = std::min<uint32_t>((b * 166) >> 7, 31);  // * 1.30
    pixels[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }
}

}  // namespace esphome::mipi_csi_camera::ov02c10
