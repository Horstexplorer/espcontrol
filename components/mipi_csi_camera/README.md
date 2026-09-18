# mipi_csi_camera

ESPHome external component that drives the MIPI-CSI camera connector on the
ESP32-P4 (as used by the JC8012P4A1C_I_W_Y "new panel" revision). It
implements ESPHome's standard `esphome::camera::Camera` interface, so it works
with the built-in Home Assistant camera entity/API snapshot & stream requests
and can be consumed by other espcontrol components through the same
listener/image-reader interface as `esp32_camera`.

## Supported hardware

- **ESP32-P4 only** — MIPI-CSI is not available on other ESP32 variants.
- Camera sensors: **SC2336** (2MP) and **OV5647** (5MP), the two sensors
  supported by Espressif's ESP32-P4-Function-EV-Board BSP that the
  JC8012P4A1C_I_W_Y New Panel reuses.

## How it works

Rather than talking to the MIPI-CSI PHY registers directly, this component
wraps Espressif's own camera stack:

- [`esp_video`](https://components.espressif.com/components/espressif/esp_video)
  brings up the CSI receiver and hardware ISP and exposes a
  Linux-`videodev2.h`-compatible V4L2 API (open/`S_FMT`/`REQBUFS`/`QBUF`/
  `DQBUF`/`STREAMON`).
- [`esp_cam_sensor`](https://components.espressif.com/components/espressif/esp_cam_sensor)
  provides the SC2336/OV5647 register tables for each supported
  resolution/format/framerate combination.

Frames are captured on a dedicated FreeRTOS task and handed to ESPHome's
camera plumbing exactly like `esp32_camera` does, so listeners (e.g. the API
component) receive a `CameraImage` for every requested/streamed frame.

Rotation is applied per-frame in hardware using the ESP32-P4's PPA (Pixel
Processing Accelerator) and is currently supported for `RGB565`/`RGB888`
output only; other pixel formats ignore the `rotation` option (a warning is
logged).

## Example configuration

Camera FPC sharing the panel's existing touchscreen/RTC `i2c:` bus (the
JC8012P4A1C_I_W_Y new panel wiring):

```yaml
i2c:
  - id: bus_a
    sda: GPIO7
    scl: GPIO8

mipi_csi_camera:
  id: my_camera
  sensor: SC2336
  resolution: 1280x720
  framerate: 30
  pixel_format: RGB565
  data_lanes: 2
  rotation: 90
  i2c_id: bus_a
```

Camera with its own dedicated SCCB pins (no shared bus):

```yaml
mipi_csi_camera:
  id: my_camera
  sensor: SC2336
  resolution: 1280x720
  framerate: 30
  pixel_format: RGB565
  data_lanes: 2
  rotation: 90
  sccb_sda_pin: GPIO7
  sccb_scl_pin: GPIO8
  reset_pin: GPIO26
  power_down_pin: GPIO27
```

## Configuration variables

| Option               | Required | Default | Description                                                                 |
| --------------------- | -------- | ------- | ---------------------------------------------------------------------------- |
| `sensor`              | no       | `SC2336`| `SC2336` or `OV5647`.                                                        |
| `resolution`           | yes      |         | `WIDTHxHEIGHT`, must match one of the sensor's supported capture modes.      |
| `framerate`            | no       | `30`    | Frames per second; validated against the sensor's mode table.                |
| `pixel_format`         | no       | `RGB565`| `RAW8`, `RAW10`, `GRAYSCALE`, `RGB565`, `RGB888`, `YUV422`, `YUV420`.        |
| `data_lanes`           | no       | `2`     | MIPI-CSI data lane count (1 or 2); informational, tied to the selected mode. |
| `rotation`             | no       | `0`     | `0`, `90`, `180`, `270`; hardware PPA rotation (RGB565/RGB888 only).         |
| `xclk_frequency`       | no       | `24MHz` | Sensor input clock.                                                          |
| `i2c_id`               | see below| | ID of an existing `i2c:` bus to reuse for SCCB. Use this when the camera's SCCB pins are hard-wired to the same net as another shared I2C bus (see below). Mutually exclusive with `sccb_sda_pin`/`sccb_scl_pin`. |
| `sccb_sda_pin`         | see below| | SCCB (camera I2C) data pin, for a **dedicated** SCCB bus. Mutually exclusive with `i2c_id`. |
| `sccb_scl_pin`         | see below| | SCCB clock pin, for a **dedicated** SCCB bus.                                |
| `sccb_port`            | no       | `1`     | I2C port `esp_video` uses when it owns a dedicated SCCB bus (ignored when `i2c_id` is set). |
| `sccb_frequency`       | no       | `100kHz`| SCCB bus frequency (ignored when `i2c_id` is set; the shared bus's own frequency applies). |
| `reset_pin`            | no       |         | Sensor hardware reset pin.                                                   |
| `power_down_pin`       | no       |         | Sensor power-down pin.                                                       |
| `horizontal_mirror`    | no       | `false` | Mirrors the image horizontally (`V4L2_CID_HFLIP`).                          |
| `vertical_flip`        | no       | `false` | Flips the image vertically (`V4L2_CID_VFLIP`).                              |
| `contrast`/`brightness`/`saturation` | no | `0` | `-2` to `2`, forwarded to the sensor if supported.               |
| `jpeg_quality`         | no       | `0`     | `0` disables JPEG re-encoding; `6`-`63` re-encodes non-JPEG output.          |
| `frame_buffer_count`   | no       | `2`     | Number of V4L2 capture buffers (2-3).                                       |

Exactly one of `i2c_id` or the `sccb_sda_pin`/`sccb_scl_pin` pair must be set.

Automations: `on_image` (`CameraImageData image` with `data`/`length`),
`on_stream_start`, `on_stream_stop`.

## Notes on the SCCB (camera I2C) bus

Whether the camera's SCCB (control) bus needs a **dedicated** bus or must
**share** an existing `i2c:` bus depends on how the board wires it:

- **Dedicated bus** (`sccb_sda_pin`/`sccb_scl_pin`): `esp_video` initializes
  and owns its own I2C driver instance on these pins. Use this when the
  camera connector has its own, otherwise-unused SDA/SCL pins.
- **Shared bus** (`i2c_id`): reuses an already-configured ESPHome `i2c:` bus
  instead of starting a second I2C driver. This is **required** on the
  JC8012P4A1C_I_W_Y new panel: its schematic shows the camera FPC's SCCB
  lines (`ES_I2C_SDA`/`ES_I2C_SCL`) hard-wired to the same GPIO7/GPIO8 net
  already used by the panel's `i2c:` bus for the touchscreen (and RTC/audio
  codec on other revisions) — a second, independent I2C driver can't also
  claim those same pins. Point `i2c_id` at that existing `i2c:` bus:

  ```yaml
  i2c:
    - id: bus_a
      sda: GPIO7
      scl: GPIO8

  mipi_csi_camera:
    id: cam
    i2c_id: bus_a
    resolution: 1280x720
    ...
  ```



## Attribution

This component is original code written for espcontrol, built on top of the
`espressif/esp_video` and `espressif/esp_cam_sensor` managed IDF components
(Apache-2.0 licensed, published by Espressif Systems). See
`JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` in this repository for the
vendor reference example this component was modeled on.
