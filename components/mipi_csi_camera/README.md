# mipi_csi_camera

ESPHome external component that drives the MIPI-CSI camera connector on the
ESP32-P4 (as used by the JC8012P4A1C_I_W_Y "new panel" revision). It
implements ESPHome's standard `esphome::camera::Camera` interface, so it works
with the built-in Home Assistant camera entity/API snapshot & stream requests
and can be consumed by other espcontrol components through the same
listener/image-reader interface as `esp32_camera`.

## Supported hardware

- **ESP32-P4 only** — MIPI-CSI is not available on other ESP32 variants.
- **ESP-IDF framework only** — this component is built entirely on ESP-IDF
  APIs (`esp_video`, PPA, the hardware JPEG encoder, ...), so `esp32:
  framework: type` must be `esp-idf`. (ESPHome only supports ESP-IDF for the
  ESP32-P4 anyway, so this is normally automatic.)
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

Sensor-specific behavior that doesn't belong in the generic V4L2 capture
pipeline (linker keep-alive tricks for vendored sensors, one-off software
color correction, ...) is kept out of `mipi_csi_camera.cpp`/`.h` entirely via
a small `SensorExtension` interface
(`mipi_csi_camera_sensor_extension.h`/`sensor_extensions.cpp`) - see "Adding
another sensor" below.

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
| `rotation`             | no       | `0`     | `0`, `90`, `180`, `270`; hardware PPA rotation (RGB565/RGB888 only). Requires a `psram:` component (the rotated frame is buffered in PSRAM). |
| `xclk_frequency`       | no       | `24MHz` | Sensor input clock.                                                          |
| `i2c_id`               | no*      |         | ID of the `i2c:` bus SCCB should use. If omitted and exactly one `i2c:` bus is declared, that bus is used automatically (standard ESPHome behavior); otherwise it must be specified explicitly. |
| `reset_pin`            | no       |         | Sensor hardware reset pin (only if your camera module has one wired to a GPIO). |
| `power_down_pin`       | no       |         | Sensor power-down pin (only if your camera module has one wired to a GPIO). |
| `horizontal_mirror`    | no       | `false` | Mirrors the image horizontally (`V4L2_CID_HFLIP`).                          |
| `vertical_flip`        | no       | `false` | Flips the image vertically (`V4L2_CID_VFLIP`).                              |
| `contrast`/`brightness`/`saturation` | no | `0` | `-2` to `2`, forwarded to the sensor if supported.               |
| `jpeg_quality`         | no       | `0`     | `0` disables JPEG re-encoding; `6`-`63` re-encodes the ISP's output (RGB565/RGB888/YUV422/YUV420/GRAYSCALE only, using the ESP32-P4's hardware JPEG encoder) so it can be viewed in Home Assistant/the API. Lower numbers mean higher quality (same inverted scale as `esp32_camera`'s `jpeg_quality`). Requires an `psram:` component; not valid with `RAW8`/`RAW10`. |
| `frame_buffer_count`   | no       | `3`     | Number of V4L2 capture buffers (2-4). 3+ is strongly recommended: with only 2, `capture_task()`/`loop()` can end up holding both buffers outside the driver's free-list simultaneously (one queued-but-unread, one actively being processed), leaving none free for the CSI/ISP DMA engine - some drivers then overwrite a buffer while it's still being read, producing torn/split-color frames. |
| `init_ldo`             | no       | `true`  | Whether this component should power the shared MIPI PHY LDO regulator (channel 3, 2.5V on the ESP32-P4). Set to `false` if an `esp_ldo:` component elsewhere in your config (typically for the MIPI-DSI display) already powers that same channel — acquiring it twice fails with `esp_ldo_acquire_channel(...): can't acquire the channel, already in use by others or not adjustable`. |

Automations: `on_image` (`CameraImageData image` with `data`/`length`),
`on_stream_start`, `on_stream_stop`.

## Adding another sensor

- Sensors already published in Espressif's `esp_cam_sensor` managed
  component registry (like SC2336/OV5647) just need an entry in
  `SENSOR_MODELS`/`SUPPORTED_MODES` in `__init__.py` plus the matching
  `add_idf_sdkconfig_option("CONFIG_CAMERA_<NAME>", ...)` call - no C++
  changes at all.
- Sensors that (like OV02C10) aren't in that registry need their driver
  vendored directly into this component, following the same flat,
  `<name>_*`-prefixed file layout as the OV02C10 files (see "Attribution"
  below for why they can't live in a subfolder), plus a `SensorExtension`
  implementation (`mipi_csi_camera_sensor_extension.h`) for whatever
  sensor-specific hooks it needs (linker keep-alive, color correction, ...) -
  see `ov02c10_esphome.h`/`.cpp` for the reference shape. The only change
  needed in the generic driver itself is one `case` in
  `get_sensor_extension()` (`sensor_extensions.cpp`); `mipi_csi_camera.cpp`/
  `.h` never need to know about a specific sensor.

## OV02C10 valid resolution/lane combinations

OV02C10's register tables (all combined into `ov02c10_settings.h`) are baked
in per resolution *and* lane count (unlike SC2336/OV5647, where `data_lanes`
is purely a wiring choice). Only these three combinations are valid; anything
else is rejected at config-validation time:

| `resolution`  | `data_lanes` | `pixel_format` (RAW) | `framerate` |
| ------------- | ------------ | --------------------- | ----------- |
| `1288x728`    | `1`          | `RAW10`               | `30`        |
| `1920x1080`   | `1`          | `RAW10`               | `30`        |
| `1920x1080`   | `2`          | `RAW10`               | `30`        |

> **Prefer the 2-lane mode.** It's what the panel vendor's own demo
> application (`video_lcd_display`) uses, and it leaves generous headroom
> on the MIPI link. The 1-lane modes run the link close to its rated
> capacity; treat them as last-resort fallbacks for boards that physically
> wire only one lane.
>
> Note: if you see torn/streaked frames or `ISP: fifo overflow` log spam
> with any mode, make sure your build pulled esp_video **>= 2.3.0** - this
> component requires it, because 2.0.x-2.2.x hardcode the ISP processor
> clock to 80 MHz (exactly the sensor's ~80 Mpx/s output rate), which
> makes the ISP input FIFO chronically overflow.

As with the other sensors, any ISP output format (`GRAYSCALE`/`RGB565`/
`RGB888`/`YUV422`/`YUV420`) can be requested at the same resolution/framerate
instead of `RAW10`.

### OV02C10 color correction

The ESP32-P4's ISP performs a plain demosaic (Bayer → RGB conversion) with no
auto exposure/gain/white-balance/color-correction unless the `esp_ipa`-driven
auto pipeline (`CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`) is enabled -
and that pipeline needs per-sensor JSON calibration data
(`esp_cam_sensor/sensors/<name>/cfg/<name>_default.json`) that only exists for
sensors in the official `esp_cam_sensor` registry (SC2336/OV5647/OV2710).
OV02C10 isn't in that registry, so enabling the auto pipeline for it produces
a far worse image (a fully-saturated solid-color frame) rather than a better
one - see `dev-docs/mipi-csi-camera-plan.md` for the investigation. Instead,
when `pixel_format: RGB565` is used with OV02C10, `MipiCsiCamera::loop()`
calls into `Ov02c10Extension::apply_rgb565_color_correction()`
(`ov02c10_esphome.cpp`), which applies a small fixed per-channel gain
correction to counteract the plain demosaic's visible green cast (Bayer
sensors sample green at 2x the rate of red/blue). This is a static
approximation, not real auto white balance, so exposure
still comes solely from the sensor's built-in per-mode register defaults (no
closed-loop auto exposure).

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

Because SCCB shares the bus with other I2C peripherals (e.g. the panel's
touchscreen), this component intentionally sets up *after* them
(`setup_priority::PROCESSOR`, below the `setup_priority::DATA` that
touchscreens and most other I2C peripherals default to). The initial sensor
mode configuration is a large one-shot burst of SCCB register writes,
immediately followed by a background task that keeps using the bus while
streaming; sharing the bus with a peripheral that's still in the middle of
its own timing-sensitive setup (e.g. a touch controller uploading firmware)
has been observed to corrupt that peripheral's I2C transactions and leave it
failed. If you still see I2C errors on another peripheral right after this
component's setup log line, try moving that peripheral's config earlier in
your YAML (equal-priority components set up in declaration order) or, in
rare cases where it uses a lower/equal priority itself, raising its priority
explicitly with `component.set_priority` / device-specific config.

This component also stops streaming and releases the CSI PHY/sensor/SCCB/LDO
claims on shutdown (`on_shutdown()`, called before every reboot - OTA
updates, safe-mode reboots, etc). This matters because a software reset
doesn't power-cycle external peripherals: without an explicit teardown, the
sensor would still be mid-stream when the *next* boot starts, and that
leftover activity on the shared I2C bus was observed to corrupt other
peripherals' (e.g. a touchscreen's) own boot-time setup - even before this
component's own `setup()` ran again on that new boot.

## Viewing the camera in Home Assistant

Home Assistant (via the ESPHome API) always treats camera image bytes as
JPEG - it doesn't know anything about MIPI-CSI, RAW Bayer data, or ISP
formats. This means:

- `RAW8`/`RAW10` frames can **never** be displayed in Home Assistant (there's
  no JPEG source format for raw sensor Bayer data); they're only usable
  through `on_image` for a custom downstream consumer.
- `RGB565`/`RGB888`/`YUV422`/`YUV420`/`GRAYSCALE` frames **need**
  `jpeg_quality` set to a non-zero value (e.g. `jpeg_quality: 10`) - this
  component then re-encodes each frame to JPEG using the ESP32-P4's hardware
  JPEG encoder before handing it to the API/Home Assistant. With
  `jpeg_quality: 0` (the default), the API is given the raw ISP output
  bytes directly and Home Assistant will show a broken/undecodable image.

So, to get a working camera entity in Home Assistant, use one of the ISP
output formats together with `jpeg_quality`, e.g.:

```yaml
mipi_csi_camera:
  sensor: OV02C10
  resolution: 1288x728
  data_lanes: 1
  pixel_format: RGB565
  jpeg_quality: 10
```

The camera entity itself is created automatically (no separate `camera:`
platform block needed, same as `esp32_camera`) as long as `api:` is enabled;
look for it under the ESPHome device's entities in Home Assistant
(Settings → Devices & Services → Devices → your device) if it doesn't appear
on a dashboard automatically.

**Important:** set a `name:` on the `mipi_csi_camera:` block (in addition to
or instead of `id:`). Like every other ESPHome entity, if only `id:` is
given (no `name:`), ESPHome marks the entity **internal** by default -
it will still work for `on_image`/lambdas, but it is hidden from the API
entity list entirely, so it will never show up in Home Assistant no matter
how `pixel_format`/`jpeg_quality` are configured. Setting `name: Camera` (or
similar) makes it a normal, visible entity:

```yaml
mipi_csi_camera:
  id: my_camera        # for lambdas/automations
  name: Camera          # required for the entity to be visible in Home Assistant
  sensor: OV02C10
  resolution: 1288x728
  data_lanes: 1
  pixel_format: RGB565
  jpeg_quality: 10
```

## ISP throughput (RGB565/RGB888 at high resolution)

Converting RAW Bayer data to RGB565/RGB888 in real time via the ESP32-P4's
hardware ISP is bandwidth-intensive, and on a full device (display + Wi-Fi +
Home Assistant API, not the vendor's minimal single-purpose demo) the ISP,
camera, and MIPI-DSI display all compete for the same PSRAM bus. At
1920x1080/30fps this can overflow the ISP's internal FIFO
(`ISP: fifo overflow` in the log), which floods the log fast enough to trip
the watchdog and reboot the device. If you hit this:

- Request `RAW8`/`RAW10` instead (bypasses the ISP color-conversion step
  entirely; do any RGB conversion downstream) - this is the most reliable
  fix, though it means Home Assistant can't display the image directly (see
  above), or
- Drop to OV02C10's smaller `1288x728` (`data_lanes: 1`) mode, which cuts the
  pixel count (and therefore ISP/PSRAM bandwidth) to less than half of
  1920x1080. Note OV02C10 only ships 30fps register tables in this
  component, so framerate itself isn't independently reducible for this
  sensor - only resolution is.

## Attribution

This component is original code written for espcontrol, built on top of the
`espressif/esp_video` and `espressif/esp_cam_sensor` managed IDF components
(Apache-2.0 licensed, published by Espressif Systems). It was modeled on the
`JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` vendor reference example
(part of Guition's board SDK for this panel; kept locally by contributors
during development, **not committed to this repository** - see below for
why), and on
[sullb/esphome-p4-csi-camera](https://github.com/sullb/esphome-p4-csi-camera)
(another independent ESPHome MIPI-CSI camera implementation for the same
board, targeting the OV02C10 sensor) used as a secondary reference while
building this component.

The OV02C10 sensor driver (`ov02c10.c`/`ov02c10*.h`) is vendored directly
into this component rather than pulled from the `espressif/esp_cam_sensor`
managed component registry, because OV02C10 isn't published there (no
release of `esp_cam_sensor`, up to and including the versions available as
of writing, ships an `ov02c10` sensor directory - the registry has `os02n10`
and `os04c10`, but not `ov02c10`). Its **only known source** is Espressif's
own driver as redistributed inside board vendors' proprietary SDK packages;
in this project's case, Guition's `JC8012P4A1C_I_W_Y_New_Panel` SDK bundle.
That bundle is not committed to this repository (it's a large, vendor-owned
archive kept locally by whoever is developing this component, not something
this project can redistribute as a whole) - so this Attribution section, not
a path inside the repo, is the citable record of where these files came
from. The files themselves are copied verbatim (only minimal adaptation, see
`ov02c10_compat.h` and the `#pragma once`/self-contained include additions,
plus combining all three per-mode register tables into a single
`ov02c10_settings.h` instead of one file per mode, needed because ESPHome
bare-`#include`s every header it finds under a component directory) and each
still carries
its own original `SPDX-FileCopyrightText: ... Espressif Systems (Shanghai)
CO LTD` / `SPDX-License-Identifier: Apache-2.0` header, which is what
actually establishes the license these files are redistributed under here -
not the vendor SDK's own (unknown/unpublished) terms for the rest of that
package. This isn't a one-off situation: the identical driver (SPDX headers,
code, and structure) also turns up vendored the same way in several other
independent public projects for the same or related boards - e.g.
[kdmukai/esp-board-common](https://github.com/kdmukai/esp-board-common)
(whose own `PROVENANCE.md` documents pulling it from a Guition
`JC4880P443C_I_W.zip` vendor SDK drop, and confirms OV02C10 is absent from
the public `esp_cam_sensor` registry) and forks such as
[kruzio1985/BETTA-HA-PANEL-10-Guiton-JC8012P4A1C](https://github.com/kruzio1985/BETTA-HA-PANEL-10-Guiton-JC8012P4A1C) -
confirming this vendor SDK, not any public Espressif GitHub repository, is
genuinely the only place this driver circulates from.

All OV02C10-specific glue code that isn't part of the vendored driver itself
(the linker keep-alive workaround, and the software color-cast correction -
see "OV02C10 color correction" above) lives in `Ov02c10Extension`
(`ov02c10_esphome.{h,cpp}`), an implementation of the generic
`SensorExtension` interface (`mipi_csi_camera_sensor_extension.h`). This
keeps the generic driver sensor-agnostic: adding another vendored
(non-`esp_cam_sensor`-registry) sensor in the future means adding another
`SensorExtension` implementation with the same shape and one `case` in
`sensor_extensions.cpp`'s `get_sensor_extension()`, not touching the generic
capture/rotate/encode pipeline at all - see "Adding another sensor" above.
All of a vendored sensor's files still have to live flat in this directory
(there's no subfolder here) — ESPHome's external-component loader only picks
up source files placed directly inside the component's own directory, it
does not recurse into subdirectories.

Because it's vendored outside `esp_cam_sensor`'s own build system, OV02C10
also needs one small extra piece of glue: `esp_cam_sensor` discovers cameras
by iterating a runtime array of every `ESP_CAM_SENSOR_DETECT_FN`-registered
detect function that the *linker* actually kept in the final binary.
Espressif's own sensors (SC2336, OV5647, …) get force-linked automatically
by their own component's build scripts; nothing does this for a vendored
driver, so the linker would otherwise silently drop `ov02c10.c` entirely
(no compile/link error — it just never gets called, `esp_video_init()`
succeeds trivially with no camera found, and `/dev/video0` never gets
created). `ov02c10_esphome.cpp` works around this by taking the address of
the public `ov02c10_detect()` function in a `__attribute__((used))` static
variable, and `mipi_csi_camera.cpp` calls
`sensor_extension_->force_link()` once during `setup()` (via the generic
`SensorExtension` interface) to make sure that translation unit itself is
linked in. See
[`esp_cam_sensor_detect.h`](https://github.com/espressif/esp-video-components/blob/master/esp_cam_sensor/include/esp_cam_sensor_detect.h)'s
own comment for background on why this is necessary.
