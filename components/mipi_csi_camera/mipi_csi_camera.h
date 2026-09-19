#pragma once

// MIPI-CSI is only available on the ESP32-P4.
#ifdef USE_ESP32_VARIANT_ESP32P4

#include <atomic>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/camera/camera.h"
#include "esphome/components/i2c/i2c_bus.h"

#include "driver/ppa.h"
#include "driver/jpeg_encode.h"

namespace esphome::mipi_csi_camera {

/// Camera sensors that the vendored `esp_cam_sensor` drivers support and
/// that this component is validated against.
enum MipiCsiSensorModel : uint8_t {
  MIPI_CSI_SENSOR_SC2336 = 0,
  MIPI_CSI_SENSOR_OV5647,
  MIPI_CSI_SENSOR_OV02C10,
};

/// Requested output pixel format. RAW8/RAW10 are the sensor's native Bayer
/// output; the rest are produced by the ESP32-P4's hardware ISP.
enum MipiCsiPixelFormat : uint8_t {
  MIPI_CSI_PIXEL_FORMAT_RAW8 = 0,
  MIPI_CSI_PIXEL_FORMAT_RAW10,
  MIPI_CSI_PIXEL_FORMAT_GRAYSCALE,
  MIPI_CSI_PIXEL_FORMAT_RGB565,
  MIPI_CSI_PIXEL_FORMAT_RGB888,
  MIPI_CSI_PIXEL_FORMAT_YUV422,
  MIPI_CSI_PIXEL_FORMAT_YUV420,
};

/// Data delivered through the `on_image` automation trigger.
struct CameraImageData {
  uint8_t *data;
  size_t length;
};

/// A single captured frame, backed by one of the V4L2 capture buffers that
/// were queued with the video device. Frames are handed to the Camera base
/// class as shared_ptrs so multiple listeners/readers can consume the same
/// buffer without copying, exactly like ESP32Camera does with camera_fb_t.
class MipiCsiCameraImage : public camera::CameraImage {
 public:
  MipiCsiCameraImage(uint8_t *data, size_t length, uint16_t width, uint16_t height, uint8_t requesters)
      : data_(data), length_(length), width_(width), height_(height), requesters_(requesters) {}

  uint8_t *get_data_buffer() override { return this->data_; }
  size_t get_data_length() override { return this->length_; }
  bool was_requested_by(camera::CameraRequester requester) const override {
    return (this->requesters_ & (1 << requester)) != 0;
  }
  uint16_t get_width() const { return this->width_; }
  uint16_t get_height() const { return this->height_; }

 protected:
  uint8_t *data_;
  size_t length_;
  uint16_t width_;
  uint16_t height_;
  uint8_t requesters_;
};

class MipiCsiCameraImageReader : public camera::CameraImageReader {
 public:
  void set_image(std::shared_ptr<camera::CameraImage> image) override {
    this->image_ = std::move(image);
    this->offset_ = 0;
  }
  size_t available() const override {
    if (!this->image_)
      return 0;
    return this->image_->get_data_length() - this->offset_;
  }
  uint8_t *peek_data_buffer() override { return this->image_->get_data_buffer() + this->offset_; }
  void consume_data(size_t consumed) override { this->offset_ += consumed; }
  void return_image() override { this->image_.reset(); }

 protected:
  std::shared_ptr<camera::CameraImage> image_;
  size_t offset_{0};
};

/// Driver for the MIPI-CSI camera connector found on the ESP32-P4 (used by
/// the JC8012P4A1C_I_W_Y "new panel" revision with an SC2336/OV5647 module).
///
/// This wraps Espressif's `esp_video` V4L2-style capture API together with
/// the `esp_cam_sensor` sensor drivers instead of talking to the MIPI-CSI PHY
/// directly: `esp_video_init()` brings up the CSI receiver + ISP pipeline and
/// probes/initializes the sensor over SCCB, then frames are captured with the
/// same open/S_FMT/REQBUFS/QBUF/DQBUF/STREAMON lifecycle used by Espressif's
/// reference examples (see `esp_video_init.h`, `linux/videodev2.h`).
class MipiCsiCamera final : public camera::Camera {
 public:
  /* ---- configuration setters (called from generated code) ---- */
  /// SCCB (the sensor's I2C register/control bus) always shares an
  /// already-configured ESPHome `i2c:` bus rather than having `esp_video`
  /// start a second, independent I2C driver instance. On the
  /// JC8012P4A1C_I_W_Y camera FPC (and most MIPI-CSI camera modules) the
  /// SCCB SDA/SCL lines are ordinary GPIOs, often shared with other
  /// peripherals on the board (touchscreen/RTC/audio codec), so reusing the
  /// bus that's already configured for them is required, not optional.
  void set_i2c_bus(i2c::I2CBus *bus) { this->external_i2c_bus_ = bus; }
  void set_sensor_model(MipiCsiSensorModel model) { this->sensor_model_ = model; }
  void set_resolution(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_framerate(uint8_t framerate) { this->framerate_ = framerate; }
  void set_pixel_format(MipiCsiPixelFormat format) { this->pixel_format_ = format; }
  void set_data_lanes(uint8_t lanes) { this->data_lanes_ = lanes; }
  void set_rotation(uint16_t rotation) { this->rotation_ = rotation; }
  void set_xclk_frequency(uint32_t frequency) { this->xclk_frequency_ = frequency; }
  void set_reset_pin(uint8_t pin) { this->reset_pin_ = pin; }
  void set_power_down_pin(uint8_t pin) { this->power_down_pin_ = pin; }
  void set_horizontal_mirror(bool mirror) { this->horizontal_mirror_ = mirror; }
  void set_vertical_flip(bool flip) { this->vertical_flip_ = flip; }
  void set_contrast(int contrast) { this->contrast_ = contrast; }
  void set_brightness(int brightness) { this->brightness_ = brightness; }
  void set_saturation(int saturation) { this->saturation_ = saturation; }
  void set_jpeg_quality(uint8_t quality) { this->jpeg_quality_ = quality; }
  void set_frame_buffer_count(uint8_t count) { this->frame_buffer_count_ = count; }
  /// On the ESP32-P4, the MIPI-CSI receiver PHY and the MIPI-DSI display PHY
  /// share the same internal LDO regulator channel (channel 3, 2.5V). If the
  /// display is already configured through ESPHome's `esp_ldo:` component,
  /// that channel is already powered and marked non-shareable/adjustable, so
  /// this component must not try to acquire it a second time. Set to
  /// `false` when another component (usually the display) already powers
  /// this rail; defaults to `true` (this component powers it itself).
  void set_init_ldo(bool init_ldo) { this->init_ldo_ = init_ldo; }

  /* ---- Component ---- */
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// Runs after other I2C peripherals on the shared SCCB/i2c bus (touchscreens, etc, which
  /// default to setup_priority::DATA) have finished their own setup(). This component performs a
  /// large one-shot SCCB register burst while configuring the sensor's mode and then starts a
  /// background task that keeps using the bus; sharing the bus with something that is still in
  /// the middle of its own timing-sensitive setup sequence (e.g. a touch controller's firmware
  /// upload) has been observed to corrupt that peripheral's I2C transactions.
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  /// Cleanly stops streaming and releases the CSI PHY/sensor/LDO/I2C-SCCB resources before a
  /// reboot (OTA update, safe-mode reboot, etc). A software reset (SW_CPU_RESET) does not
  /// power-cycle external peripherals, so if the sensor is left mid-stream and the SCCB device
  /// registration/LDO channel are left claimed, the next boot's I2C bus can start out in a state
  /// disturbed by the still-active MIPI-CSI hardware - this was observed to corrupt other I2C
  /// peripherals (e.g. a touchscreen) doing timing-sensitive setup work right after boot, even
  /// though the camera component itself hadn't run its own setup() yet on that new boot.
  void on_shutdown() override;

  /* ---- camera::Camera ---- */
  void add_listener(camera::CameraListener *listener) override { this->listeners_.push_back(listener); }
  camera::CameraImageReader *create_image_reader() override { return new MipiCsiCameraImageReader(); }  // NOLINT
  void request_image(camera::CameraRequester requester) override;
  void start_stream(camera::CameraRequester requester) override;
  void stop_stream(camera::CameraRequester requester) override;

 protected:
  static void capture_task(void *param);
  bool open_device_();
  bool configure_format_();
  bool allocate_buffers_();
  void start_capture_();
  void stop_capture_();
  /// Applies rotation/mirror/flip in hardware via the PPA (Pixel Processing
  /// Accelerator) and returns a heap buffer holding the transformed frame.
  /// Returns the original buffer untouched when rotation_ == 0.
  uint8_t *rotate_frame_(uint8_t *src, uint16_t width, uint16_t height, size_t frame_size, uint16_t *out_width,
                         uint16_t *out_height);
  /// Re-encodes a captured (post-rotation) frame as JPEG using the ESP32-P4's hardware JPEG
  /// encoder, so it can be displayed by Home Assistant/the API (which always treats camera image
  /// bytes as JPEG). Only available for the ISP-produced formats the hardware encoder accepts
  /// (RGB565/RGB888/YUV422/YUV420/GRAYSCALE) - RAW8/RAW10 can't be encoded directly. Returns
  /// nullptr (falling back to sending the raw/rotated buffer as-is) if jpeg_quality is 0, the
  /// pixel format isn't supported, or the encoder failed; the returned buffer (on success) is
  /// heap_caps_free()-owned by the caller.
  uint8_t *encode_jpeg_(const uint8_t *src, uint16_t width, uint16_t height, size_t src_size, size_t *out_size);
  size_t bytes_per_pixel_() const;

  /* configuration */
  i2c::I2CBus *external_i2c_bus_{nullptr};
  MipiCsiSensorModel sensor_model_{MIPI_CSI_SENSOR_SC2336};
  uint16_t width_{1280};
  uint16_t height_{720};
  uint8_t framerate_{30};
  MipiCsiPixelFormat pixel_format_{MIPI_CSI_PIXEL_FORMAT_RGB565};
  uint8_t data_lanes_{2};
  uint16_t rotation_{0};
  uint32_t xclk_frequency_{24000000};
  int8_t reset_pin_{-1};
  int8_t power_down_pin_{-1};
  bool horizontal_mirror_{false};
  bool vertical_flip_{false};
  int contrast_{0};
  int brightness_{0};
  int saturation_{0};
  uint8_t jpeg_quality_{0};
  uint8_t frame_buffer_count_{2};
  bool init_ldo_{true};

  /* runtime state */
  int video_fd_{-1};
  std::vector<uint8_t *> capture_buffers_;
  size_t capture_buffer_size_{0};
  bool streaming_{false};
  ppa_client_handle_t ppa_handle_{nullptr};
  jpeg_encoder_handle_t jpeg_encoder_{nullptr};
  esp_err_t init_error_{0 /* ESP_OK */};
  const char *setup_failure_reason_{nullptr};

  std::shared_ptr<MipiCsiCameraImage> current_image_;
  std::atomic<uint8_t> single_requesters_{0};
  std::atomic<uint8_t> stream_requesters_{0};
  QueueHandle_t frame_queue_{nullptr};
  TaskHandle_t capture_task_handle_{nullptr};
  std::atomic<bool> capture_task_stop_requested_{false};
  std::atomic<bool> capture_task_stopped_{false};
  std::vector<camera::CameraListener *> listeners_;
};

class MipiCsiCameraImageTrigger final : public Trigger<CameraImageData>, public camera::CameraListener {
 public:
  explicit MipiCsiCameraImageTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override {
    CameraImageData data{};
    data.data = image->get_data_buffer();
    data.length = image->get_data_length();
    this->trigger(data);
  }
};

class MipiCsiCameraStreamStartTrigger final : public Trigger<>, public camera::CameraListener {
 public:
  explicit MipiCsiCameraStreamStartTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_stream_start() override { this->trigger(); }
};

class MipiCsiCameraStreamStopTrigger final : public Trigger<>, public camera::CameraListener {
 public:
  explicit MipiCsiCameraStreamStopTrigger(MipiCsiCamera *parent) { parent->add_listener(this); }
  void on_stream_stop() override { this->trigger(); }
};

}  // namespace esphome::mipi_csi_camera

#endif  // USE_ESP32_VARIANT_ESP32P4
