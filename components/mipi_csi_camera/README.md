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
  JC8012P4A1C_I_W_Y New Panel reuses, plus **OV02C10** (2MP), which is what
  actual JC8012P4A1C_I_W_Y New Panel units ship with (see below).

## How it works

Rather than talking to the MIPI-CSI PHY registers directly, this component
wraps Espressif's own camera stack:

- [`esp_video`](https://components.espressif.com/components/espressif/esp_video)
  brings up the CSI receiver and hardware ISP and exposes a
  Linux-`videodev2.h`-compatible V4L2 API (open/`S_FMT`/`REQBUFS`/`QBUF`/
  `DQBUF`/`STREAMON`).
- [`esp_cam_sensor`](https://components.espressif.com/components/espressif/esp_cam_sensor)
  provides the SC2336/OV5647 register tables for each supported
  resolution/format/framerate combination. **OV02C10 is vendored directly
  into this component instead** (`ov02c10*.{c,h}`): it isn't published in
  that managed component registry, so its driver — Espressif's own,
  Apache-2.0 licensed — is compiled in directly. See "Attribution" below.

Frames are captured on a dedicated FreeRTOS task and handed to ESPHome's
camera plumbing exactly like `esp32_camera` does, so listeners (e.g. the API
component) receive a `CameraImage` for every requested/streamed frame.

Rotation is applied per-frame in hardware using the ESP32-P4's PPA (Pixel
Processing Accelerator) and is currently supported for `RGB565`/`RGB888`
output only; other pixel formats ignore the `rotation` option (a warning is
logged).

### Why there's no MIPI-CSI pin configuration

The CSI clock/data lanes are a fixed differential SerDes block wired directly
into the ESP32-P4 package (pins `CSI_CLK_P/N` and `CSI_DATA0/1_P/N`) — unlike
regular GPIOs, they aren't routed through the GPIO matrix, so there's nothing
to make configurable there; this component talks to them only through
`esp_video`.

The sensor's SCCB bus (its register/control interface) is a different story:
it's an ordinary I2C bus on regular GPIOs. That means it's configured the
same way any other I2C peripheral is in ESPHome — via `i2c_id`, referencing
an `i2c:` bus you declare separately — instead of the component opening a
second, independent I2C driver of its own. See below.

## Example configuration

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

## Configuration variables

| Option               | Required | Default | Description                                                                 |
| --------------------- | -------- | ------- | ---------------------------------------------------------------------------- |
| `sensor`              | no       | `SC2336`| `SC2336`, `OV5647`, or `OV02C10` (the sensor actual JC8012P4A1C_I_W_Y units ship with). |
| `resolution`           | yes      |         | `WIDTHxHEIGHT`, must match one of the sensor's supported capture modes.      |
| `framerate`            | no       | `30`    | Frames per second; validated against the sensor's mode table.                |
| `pixel_format`         | no       | `RGB565`| `RAW8`, `RAW10`, `GRAYSCALE`, `RGB565`, `RGB888`, `YUV422`, `YUV420`.        |
| `data_lanes`           | no       | `2`     | MIPI-CSI data lane count (1 or 2). For SC2336/OV5647 this is informational (tied to the mode); for OV02C10 it also selects which register table is compiled in, so only specific resolution/lane combinations are valid (see below). |
| `rotation`             | no       | `0`     | `0`, `90`, `180`, `270`; hardware PPA rotation (RGB565/RGB888 only).         |
| `xclk_frequency`       | no       | `24MHz` | Sensor input clock.                                                          |
| `i2c_id`               | no*      |         | ID of the `i2c:` bus SCCB should use. If omitted and exactly one `i2c:` bus is declared, that bus is used automatically (standard ESPHome behavior); otherwise it must be specified explicitly. |
| `reset_pin`            | no       |         | Sensor hardware reset pin (only if your camera module has one wired to a GPIO). |
| `power_down_pin`       | no       |         | Sensor power-down pin (only if your camera module has one wired to a GPIO). |
| `horizontal_mirror`    | no       | `false` | Mirrors the image horizontally (`V4L2_CID_HFLIP`).                          |
| `vertical_flip`        | no       | `false` | Flips the image vertically (`V4L2_CID_VFLIP`).                              |
| `contrast`/`brightness`/`saturation` | no | `0` | `-2` to `2`, forwarded to the sensor if supported.               |
| `jpeg_quality`         | no       | `0`     | `0` disables JPEG re-encoding; `6`-`63` re-encodes non-JPEG output.          |
| `frame_buffer_count`   | no       | `2`     | Number of V4L2 capture buffers (2-3).                                       |

Automations: `on_image` (`CameraImageData image` with `data`/`length`),
`on_stream_start`, `on_stream_stop`.

## OV02C10 valid resolution/lane combinations

OV02C10's register tables are baked in per resolution *and* lane count (unlike
SC2336/OV5647, where `data_lanes` is purely a wiring choice). Only these three
combinations are valid; anything else is rejected at config-validation time:

| `resolution`  | `data_lanes` | `pixel_format` (RAW) | `framerate` |
| ------------- | ------------ | --------------------- | ----------- |
| `1288x728`    | `1`          | `RAW10`               | `30`        |
| `1920x1080`   | `1`          | `RAW10`               | `30`        |
| `1920x1080`   | `2`          | `RAW10`               | `30`        |

As with the other sensors, any ISP output format (`GRAYSCALE`/`RGB565`/
`RGB888`/`YUV422`/`YUV420`) can be requested at the same resolution/framerate
instead of `RAW10`.

## Notes on the SCCB (camera I2C) bus

SCCB always reuses an existing `i2c:` bus (via `i2c_id`) rather than the
component starting a second, independent I2C driver of its own. On the
JC8012P4A1C_I_W_Y new panel this isn't just a convenience: the schematic
shows the camera FPC's SCCB lines (`ES_I2C_SDA`/`ES_I2C_SCL`) hard-wired to
the exact same GPIO7/GPIO8 net already used by the panel's `i2c:` bus for the
touchscreen (and RTC/audio codec on other revisions) — a second, independent
I2C driver instance couldn't claim those same pins even if we wanted it to.

The same camera FPC's other two control lines (`CSI_IO0`/`CSI_IO1`) are only
pulled up to 3.3V through resistors on this board, with no GPIO connection —
so there's no software-controlled reset/power-down pin to set for this
particular panel revision; leave `reset_pin`/`power_down_pin` unset.

## Attribution

This component is original code written for espcontrol, built on top of the
`espressif/esp_video` and `espressif/esp_cam_sensor` managed IDF components
(Apache-2.0 licensed, published by Espressif Systems). See
`JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` in this repository for the
vendor reference example this component was modeled on, and
[sullb/esphome-p4-csi-camera](https://github.com/sullb/esphome-p4-csi-camera)
for another independent ESPHome MIPI-CSI camera implementation for the same
board (targeting the OV02C10 sensor) used as a secondary reference while
building this component.

The OV02C10 sensor driver (`ov02c10.c`/`ov02c10*.h`) is vendored directly
into this component rather than pulled from the `espressif/esp_cam_sensor`
managed component registry, because OV02C10 isn't published there. It's
copied, with only minimal adaptation (see `ov02c10_compat.h`, and the
`#pragma once`/self-contained include additions to the per-mode register
table headers, needed because ESPHome bare-`#include`s every header it finds
under a component directory), from Espressif's own driver bundled with the
`JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` vendor reference SDK in this
repository — genuinely Apache-2.0 licensed (see the SPDX headers in each
file), even though it isn't (yet, as of writing) published in the public
managed-component registry.
