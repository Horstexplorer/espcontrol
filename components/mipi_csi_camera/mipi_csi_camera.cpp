#ifdef USE_ESP32_VARIANT_ESP32P4

#include "mipi_csi_camera.h"

#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "driver/i2c_master.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

namespace esphome::mipi_csi_camera {

static const char *const TAG = "mipi_csi_camera";
static constexpr size_t CAPTURE_TASK_STACK_SIZE = 4096;
static constexpr UBaseType_t CAPTURE_TASK_PRIORITY = 4;

static const char *sensor_model_to_str(MipiCsiSensorModel model) {
  switch (model) {
    case MIPI_CSI_SENSOR_SC2336:
      return "SC2336";
    case MIPI_CSI_SENSOR_OV5647:
      return "OV5647";
    case MIPI_CSI_SENSOR_OV02C10:
      return "OV02C10";
  }
  return "UNKNOWN";
}

static const char *pixel_format_to_str(MipiCsiPixelFormat format) {
  switch (format) {
    case MIPI_CSI_PIXEL_FORMAT_RAW8:
      return "RAW8";
    case MIPI_CSI_PIXEL_FORMAT_RAW10:
      return "RAW10";
    case MIPI_CSI_PIXEL_FORMAT_GRAYSCALE:
      return "GRAYSCALE";
    case MIPI_CSI_PIXEL_FORMAT_RGB565:
      return "RGB565";
    case MIPI_CSI_PIXEL_FORMAT_RGB888:
      return "RGB888";
    case MIPI_CSI_PIXEL_FORMAT_YUV422:
      return "YUV422";
    case MIPI_CSI_PIXEL_FORMAT_YUV420:
      return "YUV420";
  }
  return "UNKNOWN";
}

/// Maps our public pixel format enum to the V4L2 fourcc the ISP/sensor pipeline
/// understands. RAW8/RAW10 map to the sensor's native Bayer layout (SBGGR);
/// everything else is produced by the hardware ISP from that raw data.
static uint32_t pixel_format_to_v4l2(MipiCsiPixelFormat format) {
  switch (format) {
    case MIPI_CSI_PIXEL_FORMAT_RAW8:
      return V4L2_PIX_FMT_SBGGR8;
    case MIPI_CSI_PIXEL_FORMAT_RAW10:
      return V4L2_PIX_FMT_SBGGR10;
    case MIPI_CSI_PIXEL_FORMAT_GRAYSCALE:
      return V4L2_PIX_FMT_GREY;
    case MIPI_CSI_PIXEL_FORMAT_RGB565:
      return V4L2_PIX_FMT_RGB565;
    case MIPI_CSI_PIXEL_FORMAT_RGB888:
      return V4L2_PIX_FMT_RGB24;
    case MIPI_CSI_PIXEL_FORMAT_YUV422:
      return V4L2_PIX_FMT_YUV422P;
    case MIPI_CSI_PIXEL_FORMAT_YUV420:
      return V4L2_PIX_FMT_YUV420;
  }
  return V4L2_PIX_FMT_RGB565;
}

size_t MipiCsiCamera::bytes_per_pixel_() const {
  switch (this->pixel_format_) {
    case MIPI_CSI_PIXEL_FORMAT_RAW8:
    case MIPI_CSI_PIXEL_FORMAT_GRAYSCALE:
      return 1;
    case MIPI_CSI_PIXEL_FORMAT_RAW10:
    case MIPI_CSI_PIXEL_FORMAT_RGB565:
      return 2;
    case MIPI_CSI_PIXEL_FORMAT_RGB888:
      return 3;
    case MIPI_CSI_PIXEL_FORMAT_YUV422:
      return 2;
    case MIPI_CSI_PIXEL_FORMAT_YUV420:
      return 3;  // approximate upper bound for buffer sizing (2 planes averaged)
  }
  return 2;
}

/* ---------------- setup ---------------- */

void MipiCsiCamera::setup() {
  // SCCB always reuses the already-configured ESPHome `i2c:` bus (see
  // set_i2c_bus()) rather than starting a second, independent I2C driver;
  // the config schema guarantees external_i2c_bus_ is set before setup().
  auto *internal_bus = static_cast<i2c::InternalI2CBus *>(this->external_i2c_bus_);
  i2c_master_bus_handle_t bus_handle{};
  esp_err_t err = i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(internal_bus->get_port()), &bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get i2c bus handle for SCCB: %s", esp_err_to_name(err));
    this->init_error_ = err;
    this->mark_failed();
    return;
  }

  esp_video_init_sccb_config_t sccb_config{};
  sccb_config.init_sccb = false;
  sccb_config.i2c_handle = bus_handle;

  esp_video_init_csi_config_t csi_config[] = {{
      .sccb_config = sccb_config,
      .reset_pin = static_cast<gpio_num_t>(this->reset_pin_),
      .pwdn_pin = static_cast<gpio_num_t>(this->power_down_pin_),
  }};

  esp_video_init_config_t init_config = {};
  init_config.csi = csi_config;

  this->init_error_ = esp_video_init(&init_config);
  if (this->init_error_ != ESP_OK) {
    ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(this->init_error_));
    this->mark_failed();
    return;
  }

  if (!this->open_device_() || !this->configure_format_() || !this->allocate_buffers_()) {
    this->mark_failed();
    return;
  }

  ppa_client_config_t ppa_config = {};
  ppa_config.oper_type = PPA_OPERATION_SRM;
  esp_err_t ppa_err = ppa_register_client(&ppa_config, &this->ppa_handle_);
  if (ppa_err != ESP_OK) {
    // Rotation just won't be available; capture still works.
    ESP_LOGW(TAG, "PPA client registration failed (%s); rotation will be disabled", esp_err_to_name(ppa_err));
    this->ppa_handle_ = nullptr;
  }

  this->frame_queue_ = xQueueCreate(1, sizeof(int));
  this->start_capture_();

  xTaskCreatePinnedToCore(&MipiCsiCamera::capture_task, "mipi_csi_cam", CAPTURE_TASK_STACK_SIZE, this,
                          CAPTURE_TASK_PRIORITY, &this->capture_task_handle_, 1);
}

bool MipiCsiCamera::open_device_() {
  this->video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (this->video_fd_ < 0) {
    ESP_LOGE(TAG, "Failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    return false;
  }

  struct v4l2_capability capability{};
  if (ioctl(this->video_fd_, VIDIOC_QUERYCAP, &capability) != 0) {
    ESP_LOGE(TAG, "Failed to query camera capabilities");
    return false;
  }
  ESP_LOGD(TAG, "Camera driver: %s, card: %s", capability.driver, capability.card);
  return true;
}

bool MipiCsiCamera::configure_format_() {
  struct v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = this->width_;
  format.fmt.pix.height = this->height_;
  format.fmt.pix.pixelformat = pixel_format_to_v4l2(this->pixel_format_);

  if (ioctl(this->video_fd_, VIDIOC_S_FMT, &format) != 0) {
    ESP_LOGE(TAG, "Failed to set %ux%u %s format; check that this mode is supported by the %s sensor",
             this->width_, this->height_, pixel_format_to_str(this->pixel_format_),
             sensor_model_to_str(this->sensor_model_));
    return false;
  }

  if (this->horizontal_mirror_ || this->vertical_flip_) {
    struct v4l2_ext_control controls[1]{};
    struct v4l2_ext_controls ext_controls{};
    ext_controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    ext_controls.count = 1;
    ext_controls.controls = controls;

    if (this->horizontal_mirror_) {
      controls[0].id = V4L2_CID_HFLIP;
      controls[0].value = 1;
      if (ioctl(this->video_fd_, VIDIOC_S_EXT_CTRLS, &ext_controls) != 0) {
        ESP_LOGW(TAG, "Sensor does not support horizontal mirroring");
      }
    }
    if (this->vertical_flip_) {
      controls[0].id = V4L2_CID_VFLIP;
      controls[0].value = 1;
      if (ioctl(this->video_fd_, VIDIOC_S_EXT_CTRLS, &ext_controls) != 0) {
        ESP_LOGW(TAG, "Sensor does not support vertical flip");
      }
    }
  }

  return true;
}

bool MipiCsiCamera::allocate_buffers_() {
  struct v4l2_requestbuffers req{};
  req.count = this->frame_buffer_count_;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(this->video_fd_, VIDIOC_REQBUFS, &req) != 0) {
    ESP_LOGE(TAG, "Failed to request %u capture buffers", this->frame_buffer_count_);
    return false;
  }

  this->capture_buffers_.resize(req.count, nullptr);
  for (uint32_t i = 0; i < req.count; i++) {
    struct v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (ioctl(this->video_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "Failed to query capture buffer %" PRIu32, i);
      return false;
    }

    auto *mapped = static_cast<uint8_t *>(
        mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->video_fd_, buf.m.offset));
    if (mapped == MAP_FAILED) {  // NOLINT(performance-no-int-to-ptr)
      ESP_LOGE(TAG, "Failed to mmap capture buffer %" PRIu32, i);
      return false;
    }
    this->capture_buffers_[i] = mapped;
    this->capture_buffer_size_ = buf.length;

    if (ioctl(this->video_fd_, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "Failed to queue capture buffer %" PRIu32, i);
      return false;
    }
  }

  return true;
}

void MipiCsiCamera::start_capture_() {
  if (this->streaming_)
    return;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(this->video_fd_, VIDIOC_STREAMON, &type) != 0) {
    ESP_LOGE(TAG, "Failed to start MIPI-CSI capture stream");
    return;
  }
  this->streaming_ = true;
}

void MipiCsiCamera::stop_capture_() {
  if (!this->streaming_)
    return;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(this->video_fd_, VIDIOC_STREAMOFF, &type);
  this->streaming_ = false;
}

/* ---------------- capture task ---------------- */

void MipiCsiCamera::capture_task(void *param) {
  auto *self = static_cast<MipiCsiCamera *>(param);

  while (true) {
    struct v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(self->video_fd_, VIDIOC_DQBUF, &buf) != 0) {
      ESP_LOGW(TAG, "Failed to dequeue camera frame");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Overwrite any previously captured-but-unconsumed frame index; we only
    // ever care about the newest frame, mirroring ESP32Camera's single-slot
    // framebuffer queue behaviour.
    int index = static_cast<int>(buf.index);
    int previous_index;
    if (xQueueReceive(self->frame_queue_, &previous_index, 0) == pdTRUE) {
      struct v4l2_buffer requeue{};
      requeue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      requeue.memory = V4L2_MEMORY_MMAP;
      requeue.index = previous_index;
      ioctl(self->video_fd_, VIDIOC_QBUF, &requeue);
    }
    xQueueSend(self->frame_queue_, &index, 0);
  }
}

/* ---------------- rotation ---------------- */

uint8_t *MipiCsiCamera::rotate_frame_(uint8_t *src, uint16_t width, uint16_t height, size_t frame_size,
                                      uint16_t *out_width, uint16_t *out_height) {
  *out_width = width;
  *out_height = height;

  if (this->rotation_ == 0 || this->ppa_handle_ == nullptr)
    return src;

  // The PPA hardware rotator only supports RGB565/RGB888; other formats are
  // returned untouched (rotation is skipped, a warning is logged once).
  if (this->pixel_format_ != MIPI_CSI_PIXEL_FORMAT_RGB565 && this->pixel_format_ != MIPI_CSI_PIXEL_FORMAT_RGB888) {
    return src;
  }

  bool swap_dimensions = (this->rotation_ == 90 || this->rotation_ == 270);
  *out_width = swap_dimensions ? height : width;
  *out_height = swap_dimensions ? width : height;

  auto *dest = static_cast<uint8_t *>(heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (dest == nullptr) {
    ESP_LOGW(TAG, "Not enough PSRAM to rotate frame; returning unrotated image");
    *out_width = width;
    *out_height = height;
    return src;
  }

  ppa_srm_oper_config_t srm_config = {};
  srm_config.in.buffer = src;
  srm_config.in.pic_w = width;
  srm_config.in.pic_h = height;
  srm_config.in.block_w = width;
  srm_config.in.block_h = height;
  srm_config.in.block_offset_x = 0;
  srm_config.in.block_offset_y = 0;
  srm_config.in.srm_cm =
      this->pixel_format_ == MIPI_CSI_PIXEL_FORMAT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888;
  srm_config.out.buffer = dest;
  srm_config.out.buffer_size = frame_size;
  srm_config.out.pic_w = *out_width;
  srm_config.out.pic_h = *out_height;
  srm_config.out.block_offset_x = 0;
  srm_config.out.block_offset_y = 0;
  srm_config.out.srm_cm = srm_config.in.srm_cm;
  switch (this->rotation_) {
    case 90:
      srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
      break;
    case 180:
      srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
      break;
    case 270:
      srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
      break;
    default:
      srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
      break;
  }
  srm_config.scale_x = 1;
  srm_config.scale_y = 1;
  srm_config.mode = PPA_TRANS_MODE_BLOCKING;

  esp_err_t err = ppa_do_scale_rotate_mirror(this->ppa_handle_, &srm_config);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PPA rotate failed: %s; returning unrotated image", esp_err_to_name(err));
    heap_caps_free(dest);
    *out_width = width;
    *out_height = height;
    return src;
  }

  return dest;
}

/* ---------------- Camera interface ---------------- */

void MipiCsiCamera::request_image(camera::CameraRequester requester) {
  this->single_requesters_.fetch_or(1 << requester);
}

void MipiCsiCamera::start_stream(camera::CameraRequester requester) {
  uint8_t before = this->stream_requesters_.fetch_or(1 << requester);
  if (before == 0) {
    for (auto *listener : this->listeners_)
      listener->on_stream_start();
  }
}

void MipiCsiCamera::stop_stream(camera::CameraRequester requester) {
  uint8_t after = this->stream_requesters_.fetch_and(~(1 << requester));
  if ((after & ~(1 << requester)) == 0) {
    for (auto *listener : this->listeners_)
      listener->on_stream_stop();
  }
}

void MipiCsiCamera::loop() {
  if (this->is_failed())
    return;

  uint8_t single = this->single_requesters_.load();
  uint8_t stream = this->stream_requesters_.load();
  if (single == 0 && stream == 0)
    return;

  int index;
  if (xQueueReceive(this->frame_queue_, &index, 0) != pdTRUE)
    return;

  uint8_t *raw = this->capture_buffers_[index];
  uint16_t out_width = this->width_;
  uint16_t out_height = this->height_;
  size_t frame_size = this->capture_buffer_size_;
  uint8_t *frame_data = this->rotate_frame_(raw, this->width_, this->height_, frame_size, &out_width, &out_height);
  bool rotated_copy = frame_data != raw;

  uint8_t requesters = single | stream;
  this->single_requesters_.store(0);

  auto image = std::make_shared<MipiCsiCameraImage>(frame_data, frame_size, out_width, out_height, requesters);
  for (auto *listener : this->listeners_)
    listener->on_camera_image(image);
  image.reset();

  if (rotated_copy)
    heap_caps_free(frame_data);

  struct v4l2_buffer buf{};
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = index;
  ioctl(this->video_fd_, VIDIOC_QBUF, &buf);
}

void MipiCsiCamera::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MIPI-CSI Camera:\n"
                "  Sensor: %s\n"
                "  Resolution: %ux%u\n"
                "  Framerate: %u fps\n"
                "  Pixel Format: %s\n"
                "  Data Lanes: %u\n"
                "  Rotation: %u\n"
                "  Reset Pin: %d\n"
                "  Power Down Pin: %d\n"
                "  Frame Buffer Count: %u",
                sensor_model_to_str(this->sensor_model_), this->width_, this->height_, this->framerate_,
                pixel_format_to_str(this->pixel_format_), this->data_lanes_, this->rotation_, this->reset_pin_,
                this->power_down_pin_, this->frame_buffer_count_);

  ESP_LOGCONFIG(TAG, "  SCCB: shared i2c bus (port %d)",
                static_cast<i2c::InternalI2CBus *>(this->external_i2c_bus_)->get_port());

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup Failed: %s", esp_err_to_name(this->init_error_));
    return;
  }

  if (this->ppa_handle_ == nullptr && this->rotation_ != 0) {
    ESP_LOGW(TAG, "  Rotation requested but PPA is unavailable; frames will be delivered unrotated");
  }
}

}  // namespace esphome::mipi_csi_camera

#endif  // USE_ESP32_VARIANT_ESP32P4
