#ifdef USE_ESP32_VARIANT_ESP32P4

#include "mipi_csi_camera.h"

#include <cinttypes>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "driver/i2c_master.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "mipi_csi_camera_sensor_extension.h"

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
  this->sensor_extension_ = get_sensor_extension(this->sensor_model_);
  if (this->sensor_extension_ != nullptr) {
    // Sensors vendored directly into this component (not part of Espressif's `esp_cam_sensor`
    // managed registry) need an explicit linker keep-alive; see
    // mipi_csi_camera_sensor_extension.h.
    this->sensor_extension_->force_link();
  }

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
  // SCCB register access runs as its own I2C device on the shared bus and needs an explicit
  // clock speed (0 is rejected by the IDF I2C driver as "invalid scl frequency"); 100kHz is the
  // standard SCCB speed used by every sensor/board reference example.
  sccb_config.freq = 100000;

  esp_video_init_csi_config_t csi_config[] = {{
      .sccb_config = sccb_config,
      .reset_pin = static_cast<gpio_num_t>(this->reset_pin_),
      .pwdn_pin = static_cast<gpio_num_t>(this->power_down_pin_),
      // The MIPI-CSI PHY and MIPI-DSI display PHY share LDO channel 3 on the ESP32-P4. If the
      // display already claimed it via ESPHome's esp_ldo component, don't try to acquire it again
      // here - esp_ldo_acquire_channel() rejects a second exclusive acquire on the same channel.
      .dont_init_ldo = !this->init_ldo_,
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
    this->setup_failure_reason_ = "opening/configuring the video device failed (see error above)";
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

  // Home Assistant (and the ESPHome API in general) always treats camera image bytes as JPEG, so
  // any non-JPEG pixel format needs re-encoding before it's usable there. jpeg_quality == 0 means
  // the user explicitly wants raw frames only (e.g. for a custom on_image consumer downstream).
  if (this->jpeg_quality_ > 0) {
    jpeg_encode_engine_cfg_t enc_eng_cfg = {};
    enc_eng_cfg.timeout_ms = 1000 / std::max<uint8_t>(this->framerate_, 1) + 100;
    esp_err_t jpeg_err = jpeg_new_encoder_engine(&enc_eng_cfg, &this->jpeg_encoder_);
    if (jpeg_err != ESP_OK) {
      ESP_LOGW(TAG, "JPEG encoder engine creation failed (%s); frames will be sent unencoded",
               esp_err_to_name(jpeg_err));
      this->jpeg_encoder_ = nullptr;
    }
  }

  this->frame_queue_ = xQueueCreate(1, sizeof(int));
  this->start_capture_();

  xTaskCreatePinnedToCore(&MipiCsiCamera::capture_task, "mipi_csi_cam", CAPTURE_TASK_STACK_SIZE, this,
                          CAPTURE_TASK_PRIORITY, &this->capture_task_handle_, 1);
}

bool MipiCsiCamera::open_device_() {
  this->video_fd_ = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (this->video_fd_ < 0) {
    ESP_LOGE(TAG, "Failed to open %s: %s (errno %d)", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, strerror(errno), errno);
    return false;
  }

  struct v4l2_capability capability{};
  if (ioctl(this->video_fd_, VIDIOC_QUERYCAP, &capability) != 0) {
    ESP_LOGE(TAG, "Failed to query camera capabilities: %s (errno %d)", strerror(errno), errno);
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
    ESP_LOGE(TAG,
             "Failed to set %ux%u %s format: %s (errno %d); check that this mode is supported by the %s sensor",
             this->width_, this->height_, pixel_format_to_str(this->pixel_format_), strerror(errno), errno,
             sensor_model_to_str(this->sensor_model_));
    return false;
  }

  // VIDIOC_S_FMT is a read/write ioctl: the driver writes back the *actual* negotiated format,
  // including `bytesperline` (the per-row stride, which the CSI/ISP pipeline may pad to an
  // alignment boundary and which can therefore be larger than width * bytes-per-pixel). Frames
  // are de-strided in destride_frame_() before use, so every other stage (PPA rotation, JPEG
  // encoding, raw pass-through) can keep assuming tightly-packed rows.
  this->capture_stride_ = format.fmt.pix.bytesperline;
  size_t packed_stride = static_cast<size_t>(this->width_) * this->bytes_per_pixel_();
  if (this->capture_stride_ == 0)
    this->capture_stride_ = packed_stride;
  if (this->capture_stride_ != packed_stride) {
    ESP_LOGD(TAG, "Sensor row stride is %u bytes (packed would be %u); frames will be de-strided",
             static_cast<unsigned>(this->capture_stride_), static_cast<unsigned>(packed_stride));
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
    ESP_LOGE(TAG, "Failed to request %u capture buffers: %s (errno %d)", this->frame_buffer_count_, strerror(errno),
             errno);
    return false;
  }

  this->capture_buffers_.resize(req.count, nullptr);
  for (uint32_t i = 0; i < req.count; i++) {
    struct v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (ioctl(this->video_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "Failed to query capture buffer %" PRIu32 ": %s (errno %d)", i, strerror(errno), errno);
      return false;
    }

    auto *mapped = static_cast<uint8_t *>(
        mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, this->video_fd_, buf.m.offset));
    if (mapped == MAP_FAILED) {  // NOLINT(performance-no-int-to-ptr)
      ESP_LOGE(TAG, "Failed to mmap capture buffer %" PRIu32 ": %s (errno %d)", i, strerror(errno), errno);
      return false;
    }
    this->capture_buffers_[i] = mapped;
    this->capture_buffer_size_ = buf.length;

    if (ioctl(this->video_fd_, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "Failed to queue capture buffer %" PRIu32 ": %s (errno %d)", i, strerror(errno), errno);
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
    ESP_LOGE(TAG, "Failed to start MIPI-CSI capture stream: %s (errno %d)", strerror(errno), errno);
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

void MipiCsiCamera::on_shutdown() {
  // A software reset (OTA update, safe-mode reboot, etc) does not power-cycle external
  // peripherals: the CSI PHY, sensor, and SCCB/LDO claims all survive it untouched unless we
  // explicitly tear them down here. Leaving the sensor mid-stream has been observed to disturb
  // the shared I2C bus right at the start of the *next* boot - before this component's own
  // setup() even runs again - corrupting other peripherals (e.g. a touchscreen) that are in the
  // middle of their own timing-sensitive setup at that point.
  if (this->video_fd_ < 0)
    return;  // setup() never completed; nothing to tear down.

  if (this->capture_task_handle_ != nullptr) {
    this->capture_task_stop_requested_.store(true, std::memory_order_relaxed);
    this->stop_capture_();  // STREAMOFF unblocks a pending VIDIOC_DQBUF in the capture task.

    // Wait for the task to notice, requeue/clean up, and self-delete (it always does, see
    // capture_task()); cap the wait so a stuck driver can't hang shutdown indefinitely.
    for (int waited_ms = 0; waited_ms < 200 && !this->capture_task_stopped_.load(std::memory_order_relaxed);
         waited_ms += 5) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    this->capture_task_handle_ = nullptr;
  } else {
    this->stop_capture_();
  }

  if (this->frame_queue_ != nullptr) {
    vQueueDelete(this->frame_queue_);
    this->frame_queue_ = nullptr;
  }

  if (this->ppa_handle_ != nullptr) {
    ppa_unregister_client(this->ppa_handle_);
    this->ppa_handle_ = nullptr;
  }

  if (this->jpeg_encoder_ != nullptr) {
    jpeg_del_encoder_engine(this->jpeg_encoder_);
    this->jpeg_encoder_ = nullptr;
  }

  for (auto *buffer : this->capture_buffers_) {
    if (buffer != nullptr)
      munmap(buffer, this->capture_buffer_size_);
  }
  this->capture_buffers_.clear();

  close(this->video_fd_);
  this->video_fd_ = -1;

  // Releases the CSI PHY, stops/resets the sensor over SCCB, and (unless dont_init_ldo was set,
  // i.e. init_ldo_ == false) releases the shared MIPI PHY LDO channel - so the next boot starts
  // from a clean slate instead of inheriting whatever state the sensor was left streaming in.
  esp_err_t err = esp_video_deinit();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "esp_video_deinit failed during shutdown: %s", esp_err_to_name(err));
  }
}

/* ---------------- capture task ---------------- */

void MipiCsiCamera::capture_task(void *param) {
  auto *self = static_cast<MipiCsiCamera *>(param);

  while (!self->capture_task_stop_requested_.load(std::memory_order_relaxed)) {
    struct v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(self->video_fd_, VIDIOC_DQBUF, &buf) != 0) {
      if (self->capture_task_stop_requested_.load(std::memory_order_relaxed))
        break;
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

  self->capture_task_stopped_.store(true, std::memory_order_relaxed);
  vTaskDelete(nullptr);
}

/* ---------------- de-striding ---------------- */

uint8_t *MipiCsiCamera::destride_frame_(uint8_t *src, uint16_t width, uint16_t height) {
  size_t packed_stride = static_cast<size_t>(width) * this->bytes_per_pixel_();
  if (this->capture_stride_ == 0 || this->capture_stride_ == packed_stride)
    return src;  // common case: driver already produced tightly-packed rows, nothing to do

  size_t packed_size = packed_stride * height;
  auto *dest = static_cast<uint8_t *>(heap_caps_malloc(packed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (dest == nullptr) {
    ESP_LOGW(TAG, "Not enough PSRAM to de-stride frame; image will be corrupted");
    return src;
  }

  for (uint16_t row = 0; row < height; row++) {
    memcpy(dest + static_cast<size_t>(row) * packed_stride, src + static_cast<size_t>(row) * this->capture_stride_,
           packed_stride);
  }
  return dest;
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

  // The PPA hardware DMAs directly into `dest`, so it must satisfy the same
  // cache-line alignment constraints as the JPEG encoder's buffers (both the
  // start address and the allocated size); a plain `heap_caps_malloc()` does
  // not guarantee this and causes `ppa_do_scale_rotate_mirror()` to fail with
  // `ESP_ERR_INVALID_ARG` ("out.buffer addr or out.buffer_size not aligned to
  // cache line size"), silently falling back to the unrotated frame.
  size_t cache_line_size = 64;
  esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line_size);
  size_t aligned_size = (frame_size + cache_line_size - 1) & ~(cache_line_size - 1);
  auto *dest = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(cache_line_size, aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
  srm_config.out.buffer_size = aligned_size;
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

  // The PPA writes to `dest` via DMA, which does not automatically keep the CPU data cache coherent
  // for PSRAM. Invalidate the CPU cache for the written range so subsequent reads (JPEG encoding or
  // sending the raw frame to listeners) see the PPA's output rather than stale/uninitialized cache lines.
  esp_cache_msync(dest, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  return dest;
}

/* ---------------- JPEG re-encoding ---------------- */

uint8_t *MipiCsiCamera::encode_jpeg_(const uint8_t *src, uint16_t width, uint16_t height, size_t src_size,
                                    size_t *out_size) {
  if (this->jpeg_encoder_ == nullptr)
    return nullptr;

  jpeg_enc_input_format_t src_type;
  jpeg_down_sampling_type_t sub_sample;
  switch (this->pixel_format_) {
    case MIPI_CSI_PIXEL_FORMAT_RGB565:
      src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
      sub_sample = JPEG_DOWN_SAMPLING_YUV420;
      break;
    case MIPI_CSI_PIXEL_FORMAT_RGB888:
      src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
      sub_sample = JPEG_DOWN_SAMPLING_YUV420;
      break;
    case MIPI_CSI_PIXEL_FORMAT_YUV422:
      src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
      sub_sample = JPEG_DOWN_SAMPLING_YUV422;
      break;
    case MIPI_CSI_PIXEL_FORMAT_YUV420:
      src_type = JPEG_ENCODE_IN_FORMAT_YUV420;
      sub_sample = JPEG_DOWN_SAMPLING_YUV420;
      break;
    case MIPI_CSI_PIXEL_FORMAT_GRAYSCALE:
      src_type = JPEG_ENCODE_IN_FORMAT_GRAY;
      sub_sample = JPEG_DOWN_SAMPLING_GRAY;
      break;
    default:
      // RAW8/RAW10 sensor Bayer data has no direct JPEG source format; config validation
      // (__init__.py) already rejects jpeg_quality > 0 with these pixel formats.
      return nullptr;
  }

  // The hardware JPEG encoder's DMA2D engine requires *both* its input and output buffers to
  // satisfy specific cache-line/DMA2D alignment constraints (see esp_driver_jpeg's
  // jpeg_encoder_process()); our V4L2/PPA-rotated buffers aren't guaranteed to meet that, so copy
  // the source frame into a freshly `jpeg_alloc_encoder_mem()`-allocated input buffer first.
  jpeg_encode_memory_alloc_cfg_t in_mem_cfg = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
  size_t in_capacity = 0;
  auto *in_buf = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(src_size, &in_mem_cfg, &in_capacity));
  if (in_buf == nullptr) {
    ESP_LOGW(TAG, "Failed to allocate JPEG input buffer (%u bytes)", static_cast<unsigned>(src_size));
    return nullptr;
  }
  memcpy(in_buf, src, src_size);

  // In the worst case (very high-entropy/noisy scenes) the compressed output can approach the
  // raw input size; size the output buffer the same as the input to stay safe.
  jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
  size_t out_capacity = 0;
  auto *out_buf = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(src_size, &out_mem_cfg, &out_capacity));
  if (out_buf == nullptr) {
    ESP_LOGW(TAG, "Failed to allocate JPEG output buffer (%u bytes)", static_cast<unsigned>(src_size));
    heap_caps_free(in_buf);
    return nullptr;
  }

  jpeg_encode_cfg_t encode_cfg = {};
  encode_cfg.height = height;
  encode_cfg.width = width;
  encode_cfg.src_type = src_type;
  encode_cfg.sub_sample = sub_sample;
  // jpeg_quality_ uses the same inverted IJG-style scale as ESP32Camera's frame2jpg() convention
  // (lower value = higher quality); the hardware encoder instead wants 1-100, higher = better.
  encode_cfg.image_quality = 100 - this->jpeg_quality_;

  uint32_t encoded_size = 0;
  esp_err_t err = jpeg_encoder_process(this->jpeg_encoder_, &encode_cfg, in_buf, static_cast<uint32_t>(src_size),
                                       out_buf, static_cast<uint32_t>(out_capacity), &encoded_size);
  heap_caps_free(in_buf);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
    heap_caps_free(out_buf);
    return nullptr;
  }

  *out_size = encoded_size;
  return out_buf;
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

  // The MIPI-CSI/ISP DMA engine wrote this buffer directly into PSRAM; that write is not
  // automatically visible to the CPU data cache. Without invalidating the cache here, reads
  // below can return a mix of the fresh DMA'd bytes and stale previously-cached bytes,
  // producing exactly the kind of torn/shifted frames with per-frame color drift seen on
  // hardware. Invalidate (memory -> cache) before touching the buffer at all.
  esp_err_t cache_err = esp_cache_msync(raw, this->capture_buffer_size_, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  if (cache_err != ESP_OK) {
    ESP_LOGW(TAG, "Cache invalidate of captured frame failed: %s", esp_err_to_name(cache_err));
  }

  uint16_t out_width = this->width_;
  uint16_t out_height = this->height_;

  // Repack the frame into tightly-packed rows first (no-op/zero-copy if the driver's stride
  // already matches), since rotation/JPEG-encoding/raw pass-through all assume that layout.
  uint8_t *destrided = this->destride_frame_(raw, this->width_, this->height_);
  bool destride_owned = destrided != raw;
  size_t packed_size = static_cast<size_t>(this->width_) * this->height_ * this->bytes_per_pixel_();
  size_t frame_size = destride_owned ? packed_size : this->capture_buffer_size_;

  uint8_t *frame_data =
      this->rotate_frame_(destrided, this->width_, this->height_, frame_size, &out_width, &out_height);
  if (destride_owned && frame_data != destrided)
    heap_caps_free(destrided);  // superseded by rotate_frame_'s own output buffer
  bool rotated_copy = frame_data != raw;

  // The ISP's automatic 3A pipeline (auto exposure/gain/white balance) needs per-sensor
  // calibration data that some sensors (e.g. OV02C10) don't have (it isn't in Espressif's
  // official esp_cam_sensor registry), so it isn't enabled here - see
  // dev-docs/mipi-csi-camera-plan.md for why enabling it made the image worse, not better.
  // Without it, the ISP's plain demosaic output can have a visible green cast (Bayer's 2x green
  // sample density); sensor extensions that need a correction for this hook in here via
  // apply_rgb565_color_correction() (see mipi_csi_camera_sensor_extension.h). Sensors that don't
  // need one (get_sensor_extension() returned nullptr, or their extension leaves the default
  // no-op) are unaffected.
  if (this->sensor_extension_ != nullptr && this->pixel_format_ == MIPI_CSI_PIXEL_FORMAT_RGB565) {
    this->sensor_extension_->apply_rgb565_color_correction(reinterpret_cast<uint16_t *>(frame_data),
                                                             frame_size / 2);
  }

  uint8_t requesters = single | stream;
  this->single_requesters_.store(0);

  uint8_t *send_data = frame_data;
  size_t send_length = frame_size;
  bool send_data_owned = rotated_copy;  // the rotated copy needs freeing, unless a JPEG buffer replaces it below

  if (this->jpeg_encoder_ != nullptr) {
    size_t jpeg_length = 0;
    uint8_t *jpeg_data = this->encode_jpeg_(frame_data, out_width, out_height, frame_size, &jpeg_length);
    if (jpeg_data != nullptr) {
      if (rotated_copy)
        heap_caps_free(frame_data);  // fully consumed by the encoder now; no longer needed
      send_data = jpeg_data;
      send_length = jpeg_length;
      send_data_owned = true;
    }
    // On encode failure, fall through and send the raw/rotated buffer instead - still usable by
    // on_image automations even if Home Assistant can't display it as a JPEG.
  }

  auto image = std::make_shared<MipiCsiCameraImage>(send_data, send_length, out_width, out_height, requesters);
  for (auto *listener : this->listeners_)
    listener->on_camera_image(image);
  image.reset();

  if (send_data_owned)
    heap_caps_free(send_data);

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
                "  Frame Buffer Count: %u\n"
                "  Initialize LDO: %s",
                sensor_model_to_str(this->sensor_model_), this->width_, this->height_, this->framerate_,
                pixel_format_to_str(this->pixel_format_), this->data_lanes_, this->rotation_, this->reset_pin_,
                this->power_down_pin_, this->frame_buffer_count_, YESNO(this->init_ldo_));

  ESP_LOGCONFIG(TAG, "  SCCB: shared i2c bus (port %d)",
                static_cast<i2c::InternalI2CBus *>(this->external_i2c_bus_)->get_port());

  if (this->is_failed()) {
    if (this->setup_failure_reason_ != nullptr) {
      ESP_LOGE(TAG, "  Setup Failed: %s", this->setup_failure_reason_);
    } else {
      ESP_LOGE(TAG, "  Setup Failed: %s", esp_err_to_name(this->init_error_));
    }
    return;
  }

  if (this->ppa_handle_ == nullptr && this->rotation_ != 0) {
    ESP_LOGW(TAG, "  Rotation requested but PPA is unavailable; frames will be delivered unrotated");
  }

  if (this->jpeg_quality_ > 0) {
    ESP_LOGCONFIG(TAG, "  JPEG Re-encoding: quality %u (%s)", this->jpeg_quality_,
                 this->jpeg_encoder_ != nullptr ? "enabled" : "engine creation failed, sending raw frames");
  } else {
    ESP_LOGCONFIG(TAG,
                 "  JPEG Re-encoding: disabled (frames sent as raw %s; Home Assistant/the API can't "
                 "display these directly - set jpeg_quality to enable)",
                 pixel_format_to_str(this->pixel_format_));
  }
}

}  // namespace esphome::mipi_csi_camera

#endif  // USE_ESP32_VARIANT_ESP32P4
