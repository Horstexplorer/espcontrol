# MIPI-CSI Camera Component — Plan & Progress

## Goal

Add a new ESPHome external component, `components/mipi_csi_camera`, that drives
the MIPI-CSI camera fitted to the newest **JC8012P4A1C_I_W_Y** panel revision
(ESP32-P4). The component must implement the standard ESPHome
`esphome::camera::Camera` interface (see `components_esphome/camera`) so that:

- Other espcontrol components can pull frames from it (listener interface).
- Home Assistant / the ESPHome API can request snapshots or a stream, exactly
  like the existing `esp32_camera` component does for OV2640-style sensors.

Configurable knobs requested: rotation, framerate, resolution, MIPI data rate
(lane count / bit rate), and image (pixel) format.

## Hardware / vendor reference facts

- Example project: `JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` — Espressif
  `esp_video` (V4L2-style) sample for the ESP32-P4-Function-EV-Board.
- Camera sensor: **SC2336** (2MP), MIPI-CSI, connected over SCCB (I2C-like).
  `OV5647` is also supported by the same BSP/driver stack.
- Driver stack used by the vendor examples (not something we should
  re-implement from scratch):
  - `esp_video` (managed IDF component, `espressif/esp_video ~2.0`) — provides
    `esp_video_init()` plus a Linux-`videodev2.h`-compatible V4L2 ioctl API
    (`open`, `VIDIOC_S_FMT`, `VIDIOC_REQBUFS`, `VIDIOC_QBUF`/`DQBUF`,
    `VIDIOC_STREAMON`/`OFF`, `VIDIOC_S_EXT_CTRLS` for flip).
  - `esp_cam_sensor` — sensor drivers (`sc2336`, `ov5647`, …) with baked-in
    register sequences per resolution/format/framerate combination
    (`JC8012P4A1C_I_W_Y_New_Panel/common_components/esp_cam_sensor/sensors/sc2336`).
    Each mode entry pins width, height, fps and MIPI lane count/bitrate
    together — you select a *mode*, not independent axes.
  - MIPI-CSI PHY + ISP bring-up is handled internally by `esp_video`; we do not
    talk to `esp_lcd_mipi_dsi`-style low level registers ourselves (that API is
    for the *display*, not the camera).
- `app_video.c`/`app_video.h` in the same example show the full frame
  lifecycle: open device → `VIDIOC_S_FMT` → `VIDIOC_REQBUFS`/`QBUF` (mmap or
  user pointers) → `VIDIOC_STREAMON` → task loop of `DQBUF` → consume → `QBUF`.
- Rotation is done in hardware via the ESP32-P4 **PPA** (Pixel Processing
  Accelerator, `driver/ppa.h`, `ppa_do_scale_rotate_mirror`) in
  `video_lcd_display/main/main.c`.

## Design decisions

1. Depend on Espressif's `esp_video` + `esp_cam_sensor` managed components
   (same approach `esp32_camera` takes with `espressif/esp32-camera`) instead
   of re-deriving MIPI-CSI PHY/ISP init — this matches vendor guidance and
   avoids duplicating hundreds of sensor register tables.
2. Implement `mipi_csi_camera::MipiCsiCamera` deriving from
   `esphome::camera::Camera`, so it slots into the existing camera listener /
   image-reader / API plumbing with no changes needed elsewhere.
3. Support `SC2336` and `OV5647` sensors (both already vendored as
   `esp_cam_sensor` drivers and both explicitly called out as supported by the
   ESP32-P4-Function-EV-Board BSP that the JC8012P4A1C_I_W_Y New Panel reuses).
4. Expose:
   - `resolution` (`WIDTHxHEIGHT`), `framerate`, `pixel_format` (validated
     against each sensor's known mode table so an invalid combination fails at
     config time, not at runtime).
   - `data_lanes` (1 or 2) — informational/validated against the sensor's mode
     table; the actual lane bit rate is intrinsic to the selected mode (as
     with the vendor examples) and is logged in `dump_config()`.
   - `rotation` (0/90/180/270) applied per-frame with the PPA driver.
   - Standard camera plumbing: I2C/SCCB bus, reset/power-down pins, frame
     buffer count, JPEG quality (for `esp32_camera`-style consumers that expect
     JPEG), horizontal/vertical flip (mapped to V4L2 `V4L2_CID_HFLIP/VFLIP`).
5. Frame capture runs on a dedicated FreeRTOS task (mirrors `app_video.c`
   pattern and `esp32_camera`'s producer/consumer queue design) so `loop()`
   just drains completed frames and notifies listeners.
6. Guard the whole component behind `USE_ESP32_VARIANT_ESP32P4` (MIPI-CSI only
   exists on P4), same convention as `components/mipi_dsi`.

## Task breakdown

1. [x] Explore reference material (BSP headers, vendor example app, existing
       `camera`/`esp32_camera` ESPHome components, `mipi_dsi`/`mipi_rgb`
       conventions used in this repo).
2. [x] Write this plan document.
3. [x] Implement `components/mipi_csi_camera/__init__.py` (config schema,
       codegen, IDF component + sdkconfig wiring).
4. [x] Implement `mipi_csi_camera.h` / `mipi_csi_camera.cpp` (V4L2 lifecycle,
       Camera interface, PPA rotation, listener notification, image reader).
5. [x] Add `README.md` (usage + supported sensors + config reference).
6. [x] Validate: ran `esphome config` (full schema/codegen pass, valid) and a
       full `esphome compile` against a throwaway ESP32-P4 test YAML in this
       worktree — the component builds and links successfully end-to-end
       against the real `espressif/esp_video`/`espressif/esp_cam_sensor` IDF
       components. No physical device test was possible from this
       environment — that still needs to happen on real JC8012P4A1C_I_W_Y
       hardware with a camera module attached.
7. [ ] Update any relevant docs (e.g. `dev-docs/devices-and-builds.md` or a
       device README) only if directly tied to enabling this on a real device
       config — not done yet, no device YAML references this component.

## Progress log

- 2026-09-18: Investigated hardware/software stack, confirmed SC2336/OV5647 +
  `esp_video`/`esp_cam_sensor` as the correct dependency, created working
  branch `add-mipi-csi-camera-component` in a dedicated worktree, wrote this
  plan.
- 2026-09-18: Implemented `components/mipi_csi_camera` (Python config schema +
  C++ `MipiCsiCamera` driver), wrote `README.md`. Verified with a scratch
  ESP32-P4 test YAML: `esphome config` passed cleanly, and `esphome compile`
  succeeded (found and fixed one real issue along the way — `esp_video ~2.0`
  requires `esp_cam_sensor ~2.0`, not `~1.1` as first assumed from the older
  vendored example). Firmware built successfully (Flash 25.3%, RAM 12.7%).
  Rotation is currently limited to RGB565/RGB888 output (PPA hardware
  rotator constraint); other formats skip rotation with a logged warning.
  Remaining work: enable this on an actual device YAML once a camera-equipped
  JC8012P4A1C_I_W_Y unit is available for physical testing.
- 2026-09-18: Extracted text from `JC8012P4A1C_I_W_Y_New_Panel/JC8012P4A1.pdf`
  (schematic) to determine the real camera FPC pinout, since the first device
  YAML the user tried hit real pin conflicts. Findings: the camera FPC's SCCB
  lines (`ES_I2C_SDA`/`ES_I2C_SCL`) are hard-wired to the exact same
  GPIO7/GPIO8 net as the panel's existing shared `i2c:` bus (used for the
  touchscreen, and RTC/audio codec on other revisions) — not separate pins.
  `GPIO27` is already the LCD's `reset_pin`. The camera FPC's `CSI_IO0`/
  `CSI_IO1` lines are only pulled up to VDDA via 10k resistors with no GPIO
  connection, so this hardware has no software-controlled camera reset/
  power-down pin. This invalidated the original "esp_video owns a dedicated
  SCCB I2C driver" design for this specific board (two independent I2C
  drivers can't both claim GPIO7/GPIO8). Reworked the component to support
  both modes: a new `i2c_id` option shares an already-initialized ESPHome
  `i2c:` bus (via `i2c_master_get_bus_handle()` on the bus's port, exposed
  through `i2c::InternalI2CBus::get_port()`) for boards like this one, while
  the original `sccb_sda_pin`/`sccb_scl_pin` dedicated-bus mode remains for
  boards with independent camera I2C pins. Re-validated end-to-end with
  `esphome config` + a full `esphome compile` using the shared-bus mode
  against `i2c: bus_a` on GPIO7/GPIO8 — both passed.
- 2026-09-18: User pushed back on exposing dedicated-vs-shared SCCB bus
  config at all, pointing out the CSI differential lanes (ESP32-P4 package
  pins 42-48, `CSI_CLK_P/N`/`CSI_DATA0/1_P/N`) are fixed SerDes hardware, not
  GPIO-matrix pins — correct, and that was never exposed as config anyway
  (only SCCB pins were). Cross-checked
  [`sullb/esphome-p4-csi-camera`](https://github.com/sullb/esphome-p4-csi-camera)
  (an independent ESPHome MIPI-CSI component for the same board, targeting
  OV02C10): its C++ hardcodes `sccb_config.init_sccb = true` on port 0,
  SCL=GPIO8/SDA=GPIO7 — confirming SCCB/I2C really is required by
  `esp_video` for any sensor (register-level control), but that project just
  hardcodes it for one board rather than exposing it as YAML config. Since
  this component targets multiple sensors/boards, SCCB does need to stay
  configurable — but the bespoke dual-mode `sccb_sda_pin`/`sccb_scl_pin`/
  `sccb_port`/`sccb_frequency` schema was unnecessary complexity. Simplified
  to the standard ESPHome idiom instead: a single
  `cv.GenerateID(CONF_I2C_ID): cv.use_id(i2c.I2CBus)` option, matching how
  every other I2C-peripheral component is configured (auto-resolves to the
  sole configured `i2c:` bus when there's only one). Removed the dedicated-
  bus mode entirely from the Python schema, C++ setter/members, `setup()`/
  `dump_config()` branching, and README. Re-validated end-to-end: recreated
  a scratch ESP32-P4 test YAML with `i2c: bus_a` (GPIO7/GPIO8) and
  `mipi_csi_camera: { i2c_id: bus_a, ... }`; `esphome config` passed
  (confirmed `i2c_id` also auto-resolves correctly when omitted with a
  single `i2c:` bus declared); `esphome compile` initially caught two real
  bugs introduced by the simplification — a duplicate `esp_err_t err`
  declaration in `setup()` (rename to `ppa_err`) and three `%u`/`uint32_t`
  format-specifier mismatches in `allocate_buffers_()` logging (fixed with
  `PRIu32`) — after fixing both, the full compile succeeded and linked
  cleanly against the real `esp_video`/`esp_cam_sensor` IDF components
  (Flash 5.7%, RAM 12.2%). Scratch test directory removed afterward.

- 2026-09-19: User reported `Setup Failed: ESP_FAIL` on their actual device
  with `sensor: SC2336`. Neither of the component's own `ESP_LOGE` failure
  paths appeared anywhere in the 1273-line log they provided, so diagnosed
  from other evidence instead: the log's I2C bus scan showed a device only
  at address `0x36`, never at `0x30` (SC2336's SCCB address) — and the
  vendor's own `JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display/sdkconfig.defaults`
  sets `CONFIG_CAMERA_OV02C10=y`, whose SCCB address is `0x36`. Confirmed:
  actual JC8012P4A1C_I_W_Y New Panel units ship with an **OV02C10** sensor,
  not SC2336/OV5647 — those were only ever an assumption carried over from
  the generic ESP32-P4-Function-EV-Board BSP this component was originally
  modeled on. This was a genuine feature gap, not a user config mistake.
  OV02C10 isn't published in the `espressif/esp_cam_sensor` managed
  component registry (confirmed via GitHub directory listing/code search),
  so it can't be added the same way as SC2336/OV5647. User chose to add
  OV02C10 support now.
- 2026-09-19: Vendored Espressif's own OV02C10 driver (Apache-2.0, sourced
  from the untracked `JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display/
  components/esp_cam_sensor/sensors/ov02c10/` reference dump the user
  originally provided) directly into `components/mipi_csi_camera/` — flat,
  not in a subdirectory, because ESPHome's external-component file discovery
  only picks up source files directly in a component's own directory (no
  subfolder nesting at all, confirmed by reading `esphome/loader.py`).
  Required three adaptations to the vendored files: (1) a small
  `ov02c10_compat.h` defining two Kconfig-derived constants
  (`CONFIG_CAMERA_OV02C10_MAX_SUPPORT`, `..._ABSOLUTE_GAIN_LIMIT`) that have
  no real Kconfig entry in our build (ESP-IDF silently drops
  `sdkconfig.defaults` options with no matching Kconfig declaration, ruling
  out reusing `add_idf_sdkconfig_option()` here); (2) the three mutually-
  exclusive "which capture mode is compiled in" macros
  (`CONFIG_CAMERA_OV02C10_MIPI_RAW10_...`) plus
  `..._FORMAT_INDEX_DEFAULT` are instead supplied as global `-D` build flags
  from Python, chosen from the user's `resolution`/`data_lanes` selection;
  (3) gave each per-mode register-table header (`ov02c10_mipi_*.h`) its own
  `#pragma once` and made it include its own dependencies
  (`ov02c10_regs.h`/`ov02c10_types.h`) — needed because ESPHome's
  auto-generated `esphome.h` bare-`#include`s every header found under a
  component directory (alphabetically, independent of any other header's
  internal include order), which both broke macro-definition order and
  caused duplicate-definition errors the first time this was compiled.
  Added `OV02C10` to `SENSOR_MODELS`/`MipiCsiSensorModel`, its 3 valid
  resolution/lane combinations (`1288x728`@1-lane, `1920x1080`@1-lane,
  `1920x1080`@2-lane, all `RAW10`@30fps) with dedicated `cv.Invalid`
  validation, and a `sensor_model_to_str()` case. Also fixed a copy-paste
  bug inherited from the vendor source (`ov02c10.h`'s detect-function
  prototype said `sc2336_detect` instead of `ov02c10_detect`). Verified
  end-to-end with a scratch ESP32-P4 test YAML (`sensor: OV02C10`,
  `1920x1080`, `data_lanes: 2`): `esphome config` passed, and a full
  `esphome compile` succeeded (including compiling `ov02c10.c` itself) after
  the header-ordering fix above. Scratch test directory removed afterward.
  Updated `README.md` (OV02C10 supported-sensor note, valid resolution/lane
  table, attribution for the vendored driver).

