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
- 2026-09-19: User flashed the OV02C10 build and reported a *new*, different
  failure. Their fresh log confirms real progress: `dump_config` now shows
  `Sensor: OV02C10`, `SCCB: shared i2c bus (port 0)`, and (critically)
  `Setup Failed: ESP_OK` — meaning `esp_video_init()` itself now succeeds
  (the sensor is correctly detected/initialized), so the failure has moved
  to the component's own post-init steps: `open_device_()` (`open()` +
  `VIDIOC_QUERYCAP`), `configure_format_()` (`VIDIOC_S_FMT` + optional
  flip/mirror controls), or `allocate_buffers_()` (`VIDIOC_REQBUFS` +
  per-buffer `VIDIOC_QUERYBUF`/`mmap()`/`VIDIOC_QBUF`). None of these three
  functions' `ESP_LOGE` failure messages appeared anywhere in the user's
  1270-line log, even though the log clearly captures other components'
  real-time setup() output at the same log level — so a fix landed to
  produce genuinely diagnostic output on the next attempt rather than
  theorize further from an already-exhausted log: every `open()`/`ioctl()`
  failure branch in these three functions now logs `strerror(errno)` +
  the numeric `errno` alongside the existing message, and a new
  `setup_failure_reason_` string field is set whenever this code path
  causes `mark_failed()`, so `dump_config()` no longer misleadingly prints
  the stale `ESP_OK` `init_error_` for this specific failure path (a
  secondary, pre-existing diagnostic bug fixed alongside it). Re-verified
  end-to-end with a scratch ESP32-P4 test YAML (`sensor: OV02C10`,
  `1920x1080`, `data_lanes: 2`, `pixel_format: RGB565`): `esphome config`
  and a full `esphome compile` both succeeded. Root cause of the actual
  buffer/format failure is still unknown — waiting on the user's next log
  capture with these diagnostics in place.
- 2026-09-18 (later): User's next log/serial capture (with the errno
  diagnostics in place) revealed the real root cause immediately:
  `open("/dev/video0", ...)` failed with `errno 2` (`ENOENT`) — the video
  device node was never created at all. Traced through Espressif's own
  `esp_video_init.c`/`esp_cam_sensor_detect.c` source (fetched from
  `espressif/esp-video-components` on GitHub) to understand why:
  `esp_video_init()` discovers camera sensors purely by iterating a runtime
  array returned by `esp_cam_sensor_detect_get_array()`, built from every
  `ESP_CAM_SENSOR_DETECT_FN(...)`-registered function that the *linker*
  actually keeps in the final binary — via a custom `.esp_cam_sensor_detect_fn`
  linker section. `esp_cam_sensor_detect_detect.h`'s own comment explicitly
  documents that any driver using this macro needs its own build script to
  add `target_link_libraries(... INTERFACE "-u <detect_fn>")`, "because
  otherwise the linker will ignore camera driver as it has no other files
  depending on any symbols in it" — i.e. an unreferenced object file with
  only a self-registering function gets silently dropped from the final
  image by the linker. Espressif's own `sc2336`/`ov5647` drivers get this
  treatment automatically from `esp_cam_sensor`'s own per-sensor
  CMakeLists.txt (gated by their own `CONFIG_CAMERA_SC2336`/`..._OV5647`
  Kconfig options), but our vendored `ov02c10.c` lives outside that build
  system entirely, so nothing forced it to be linked in — meaning
  `esp_video_init()`'s sensor-detection loop had *zero* MIPI-CSI entries to
  try, matching every earlier symptom exactly: it returned instantly
  (nothing to iterate), returned `ESP_OK` (nothing failed, nothing was
  attempted), logged nothing (the "failed to detect" `ESP_LOGE` only fires
  *inside* the loop body, which never executed), and never created
  `/dev/video0` (the create-video-device step is likewise inside that same
  loop body). Fixed by adding an explicit reference to the public
  `ov02c10_detect()` symbol from `mipi_csi_camera.cpp` (a
  `static void *const ... __attribute__((used)) = &ov02c10_detect;`) —
  this forces the linker to pull in `ov02c10.c`'s object file to resolve
  the reference, which brings its self-registering detect-function entry
  along with it, without needing any ESP-IDF-specific linker-flag plumbing
  through ESPHome's external-component build generation. Verified via a
  scratch ESP32-P4 test YAML (`sensor: OV02C10`, `1920x1080`,
  `data_lanes: 2`): `esphome compile` succeeded, and the generated
  `test.map` link map now shows
  `.text.__esp_cam_sensor_detect_fn_ov02c10_detect_ESP_CAM_SENSOR_MIPI_CSI`
  present in the final image (previously would have been dropped).
  Scratch test directory removed afterward.
- 2026-09-19: User's next log capture (with the linker fix in place) showed
  real forward progress: the MIPI-CSI detect loop now runs, but fails at
  `esp_video_init: failed to initialize SCCB`. Fetching the full raw log
  (not just the earlier keyword-filtered grep) revealed the two preceding
  native ESP-IDF lines that had been missed before:
  `i2c.master: i2c_master_bus_add_device(1182): invalid scl frequency` and
  `sccb_i2c: sccb_new_i2c_io(54): failed to add device`. Root cause: our own
  `setup()` builds `esp_video_init_sccb_config_t sccb_config{}` with
  aggregate-initialization (zero-fills every field) and never sets
  `sccb_config.freq`, so the SCCB "device" that `esp_video_init()` registers
  on the shared I2C bus was requesting a 0 Hz clock — which the newer IDF
  `i2c_master_bus_add_device()` correctly rejects as invalid. This was a
  plain omission in our own code, not a bus/address conflict with the
  GSL3680 touchscreen (checked `devices/guition-esp32-p4-jc8012p4a1-v3/device/device.yaml`'s
  `i2c:`/`touchscreen:` block — no `0x36` conflict was ever present; the
  main I2C bus's own 400kHz `frequency:` setting is unrelated, since SCCB
  register access is added as a *separate* device on the same bus with its
  own clock speed). Fixed by explicitly setting `sccb_config.freq = 100000`
  (100kHz), matching every reference board example in the vendor tree
  (`xiaozhi-esp32-main/main/boards/**/*.cc`, all of which use 100kHz or
  400kHz SCCB clocks — 100kHz is the more universally supported default).
  Verified via a scratch ESP32-P4 test YAML with a real `i2c:` bus block
  (`sensor: OV02C10`, `1920x1080`, `data_lanes: 2`): `esphome compile`
  succeeded. Scratch test directory removed afterward.
- 2026-09-19 (later): User's next log capture (with the SCCB frequency fix
  in place) showed the SCCB fix worked completely — the I2C bus scan now
  finds the sensor at `0x36` and the SCCB device registers successfully —
  but a new, later failure appeared: `csi_video: csi_video_init(414):
  failed to init LDO`, then `esp_video: video->ops->init=102`, and our own
  `open("/dev/video0")` fails with `errno 22` (`EINVAL`) since the video
  device node itself was never created (the LDO failure happens earlier in
  `esp_video_init()`, before the character device is registered). The
  user's device already logs `esp_ldo: Acquired LDO channel 3 with voltage
  2500mV` at boot (their `esp_ldo:` component, used for the MIPI-DSI
  display) followed later by our camera's own attempt hitting
  `ldo: esp_ldo_acquire_channel(109): can't acquire the channel, already in
  use by others or not adjustable`. Root cause: on the ESP32-P4, the
  MIPI-CSI receiver PHY and the MIPI-DSI display PHY share the same
  internal LDO regulator channel (channel 3, 2.5V) — confirmed by reading
  `esp_video_csi_device.c` from `espressif/esp-video-components` on GitHub,
  which hardcodes `CSI_LDO_UNIT_ID = 3`. IDF's `esp_ldo_acquire_channel()`
  (`components/esp_hw_support/ldo/esp_ldo_regulator.c` in `espressif/esp-idf`)
  only allows a second acquire of an already-in-use channel when neither
  side requests "adjustable" mode; ESPHome's `esp_ldo:` component acquires
  its channel as adjustable, so any second acquirer (our camera component)
  is unconditionally rejected. Espressif anticipated exactly this
  shared-PHY scenario: `esp_video_init_csi_config_t` has a `dont_init_ldo`
  field (added in `esp_video` 1.4.0 specifically to let callers skip LDO
  initialization when it's already powered elsewhere). Added a new
  `init_ldo` (default `true`) config option, wired to
  `csi_config.dont_init_ldo = !init_ldo_`, so users whose device already
  configures `esp_ldo:` for the display (as the JC8012P4A1C_I_W_Y "new
  panel" device config does) can set `init_ldo: false` to reuse that
  existing, already-powered channel instead of conflicting with it.
  Documented in the README's configuration table. Verified with a scratch
  ESP32-P4 test YAML including `init_ldo: false`: `esphome compile`
  succeeded. Scratch test directory removed afterward. Reported back to
  the user to set `init_ldo: false` in their real device config (since it
  already declares `esp_ldo:` for the display) and reflash/retest.

- **ISP FIFO overflow at 1920x1080/30fps RGB565 (WDT panic)**: after
  `init_ldo: false` unblocked setup, the user's next real-hardware log
  showed dozens of `E (XXX) ISP: fifo overflow` lines followed by a
  `Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1)`.
  Compared our OV02C10/1920x1080/2-lane/30fps/RGB565 config line-for-line
  against Espressif's own combined LCD+camera reference
  (`JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display`'s `sdkconfig.defaults`,
  `CONFIG_CAMERA_OV02C10_MIPI_RAW10_1920x1080_2LAN_30FPS`,
  `CONFIG_SPIRAM_SPEED_200M`) - our settings matched exactly, ruling out a
  config bug. Traced `ISP: fifo overflow` to
  `esp_isp_isr_dispatcher()`/`ISP_LL_EVENT_ASYNC_FIFO_OVF` in ESP-IDF's
  `esp_driver_isp`, which just logs on every overflow with no back-off -
  under sustained overflow this floods the log fast enough to starve CPU1
  until the watchdog fires. Root cause: real-time RAW-to-RGB565 ISP
  conversion at 1920x1080/30fps is bandwidth-intensive, and unlike the
  vendor's minimal single-purpose demo, the user's full ESPHome app has the
  MIPI-DSI display continuously refreshing and competing for the same
  PSRAM/system bus, pushing the pipeline over the edge. Recommended a
  diagnostic: switch `pixel_format` to `RAW10` (skips the ISP color
  conversion step entirely) at the same resolution/framerate. The user
  retested and confirmed: `Setup mipi_csi_camera took 95ms`, zero
  `ISP: fifo overflow` lines, and a stable ~3600-line log with no crash -
  confirming the diagnosis. RAW10 (or RAW8) is now the recommended pixel
  format for 1920x1080/30fps on this hardware; RGB565/RGB888 conversion
  should happen downstream of capture if needed, or only be requested at
  lower resolutions (see next entry).

- **I2C bus contention corrupting the touchscreen (setup-priority fix)**:
  the same RAW10 log that confirmed the FIFO fix also showed, right after
  `mipi_csi_camera`'s setup log line and now for the first time, a
  touchscreen failure: `touchscreen.gsl3680:169: Unexpected byte in
  read_ram: got 0x0, expected 0x5a` -> `I2C Error: 6` -> `touchscreen was
  marked as failed`. Root cause: ESPHome's default
  `Component::get_setup_priority()` is `setup_priority::DATA` (600); our
  camera previously used `setup_priority::HARDWARE` (800), so the camera's
  `setup()` - a large one-shot SCCB register burst immediately followed by
  spawning a persistent background capture task that keeps using the
  shared I2C bus - ran *before* the touchscreen's own timing-sensitive
  firmware-upload sequence (default priority `DATA`) even started,
  corrupting its I2C transactions via bus contention/timing interference.
  Fix: lowered `MipiCsiCamera::get_setup_priority()` to
  `setup_priority::PROCESSOR` (400), below `DATA`, so the touchscreen (and
  any other DATA-or-higher-priority I2C peripheral) finishes its own setup
  before the camera's SCCB burst/background task begins. Verified with a
  scratch ESP32-P4 test YAML (`i2c:` bus, `OV02C10`, `1288x728`,
  `data_lanes: 1`, `RGB565`, `init_ldo: false`): `esphome compile`
  succeeded. Documented the setup-order rationale in the README. Scratch
  test directory removed afterward.

- **Lower-resolution RGB565 option for reduced ISP/PSRAM load**: re-examined
  `SUPPORTED_MODES["OV02C10"]` in `__init__.py` to find a lower-load RGB565
  alternative to 1920x1080/30fps. Confirmed OV02C10 only ships two resolution
  register tables in this component - `1288x728` (1 data lane only) and
  `1920x1080` (1 or 2 data lanes) - both hard-coded at 30fps; unlike SC2336,
  no lower-framerate OV02C10 tables are vendored, so framerate itself isn't
  independently reducible for this sensor. Recommended `resolution: 1288x728`
  with `data_lanes: 1` as the lower-load RGB565 option (roughly 46% the pixel
  count of 1920x1080, reducing ISP/PSRAM bandwidth pressure while keeping
  RGB565 output). Verified this combination compiles cleanly. Documented the
  tradeoff and the valid resolution/lane table cross-reference in the
  README's new "ISP throughput" section.

  - **Touchscreen still corrupted with 1288x728/RGB565, despite the
    setup-priority fix (cross-boot residual state, not within-boot ordering)**:
    the user tested `resolution: 1288x728`, `data_lanes: 1`, `RGB565` with the
    setup-priority fix applied and still saw the touchscreen fail
    (`gsl3680:169` "Unexpected byte in read_ram" -> `I2C Error: 6` -> marked
    failed) - but this time the log showed the touchscreen's `setup()`
    starting and failing *before* `mipi_csi_camera`'s own `setup()` had run at
    all on that boot (consistent with the priority fix actually working this
    time), which disproved the "within-boot ordering" theory as the sole
    cause. Re-examined the boot: `rst:0xc (SW_CPU_RESET)` showed this was a
    *software* reset (from the OTA update that immediately preceded it), not a
    power-on reset - and a software reset does not power-cycle external
    peripherals. The camera component had no `on_shutdown()`/teardown at all:
    its background capture task ran an unconditional infinite loop, and
    `setup()` never got paired with any code path that stopped streaming,
    closed the video device, or called `esp_video_deinit()`. This meant every
    OTA update (or any other soft reboot) left the CSI sensor mid-stream and
    the SCCB/LDO claims held, so the *next* boot's I2C bus started out
    disturbed by still-active MIPI-CSI hardware right as the touchscreen began
    its own timing-sensitive firmware-upload sequence - before the camera's
    own `setup()` even ran again on the new boot. Fix: added
    `MipiCsiCamera::on_shutdown()` (called by ESPHome before every reboot) that
    (1) sets a stop flag and calls `VIDIOC_STREAMOFF` to unblock the capture
    task's pending `VIDIOC_DQBUF` call, (2) waits (bounded to 200ms) for the
    capture task to notice, requeue, and self-delete via `vTaskDelete(nullptr)`
    (the task loop itself was changed from `while (true)` to check the stop
    flag), (3) deletes the frame queue, (4) unregisters the PPA client, (5)
    `munmap()`s and clears all capture buffers, (6) closes the video fd, and
    (7) calls `esp_video_deinit()` to release the CSI PHY, reset/power down
    the sensor over SCCB, and (unless `init_ldo: false`) release the shared
    MIPI PHY LDO channel - leaving a clean slate for the next boot. Verified
    with a scratch ESP32-P4 test YAML (`OV02C10`, `1288x728`, `data_lanes: 1`,
    `RGB565`, `init_ldo: false`): `esphome compile` succeeded. Documented the
    shutdown behavior and its rationale in the README. Scratch test directory
    removed afterward.

    - **Camera entity not usable in Home Assistant (no JPEG encoding was ever
      implemented)**: after a successful clean install (no exceptions in the
      log), the user reported no way to view the camera in Home Assistant's UI.
      Traced through ESPHome's `camera`/`api` components
      (`esphome/components/camera/camera.h`,
      `esphome/components/api/api_connection.cpp`): the entity itself is created
      automatically via the `camera::Camera` singleton (`Camera::instance()`,
      `AUTO_LOAD = ["camera"]`, `setup_entity(var, config, "camera")` - same
      pattern as `esp32_camera`) as soon as `api:` is enabled, so no separate
      registration step was needed there. However, `homeassistant/components/
      esphome/camera.py` (HA's ESPHome integration) and ESPHome's API protocol
      always treat camera image bytes as JPEG - and this component's
      `jpeg_quality` config option, while present in the schema since the
      initial implementation (intended to mirror `esp32_camera`'s re-encoding
      behavior), was never actually wired to any encoding code: `loop()` always
      sent the raw ISP/sensor output bytes (RGB565/RAW10/etc.) directly to
      listeners. Home Assistant would therefore either not display anything
      useful or fail to decode the image, regardless of pixel format chosen.
      Fix: implemented real JPEG re-encoding using the ESP32-P4's hardware JPEG
      encoder (`driver/jpeg_encode.h`, `jpeg_new_encoder_engine()` /
      `jpeg_encoder_process()`), created in `setup()` when `jpeg_quality > 0` and
      invoked per-frame in `loop()` via a new `encode_jpeg_()` helper. Mapped our
      pixel formats to the driver's `jpeg_enc_input_format_t` values
      (`RGB565`→`JPEG_ENCODE_IN_FORMAT_RGB565`, `RGB888`→`_RGB888`,
      `YUV422`→`_YUV422`, `YUV420`→`_YUV420`, `GRAYSCALE`→`_GRAY`; `RAW8`/`RAW10`
      have no direct JPEG source format and are rejected at config-validation
      time if `jpeg_quality` is set). Both the encoder's input and output
      buffers must satisfy the hardware's DMA2D/cache-line alignment
      constraints (confirmed by reading `jpeg_encoder_process()`'s source in
      `esp-idf`'s `esp_driver_jpeg/jpeg_encode.c`) - our V4L2/PPA-rotated
      buffers aren't guaranteed to meet that, so `encode_jpeg_()` copies the
      source frame into a `jpeg_alloc_encoder_mem()`-allocated, properly-aligned
      input buffer before encoding (a small extra copy, traded for correctness/
      safety on real hardware rather than risking subtle alignment bugs).
      `loop()` now sends the JPEG-encoded buffer to listeners when available,
      falling back to the raw/rotated buffer if encoding is disabled or fails.
      `on_shutdown()` now also releases the JPEG encoder engine
      (`jpeg_del_encoder_engine()`). Updated Python validation
      (`validate_jpeg_quality`) to reject `jpeg_quality > 0` combined with
      `RAW8`/`RAW10` pixel formats, and simplified the redundant PSRAM
      final-validation check. Verified with a scratch ESP32-P4 test YAML
      (`OV02C10`, `1288x728`, `data_lanes: 1`, `RGB565`, `jpeg_quality: 10`,
      `psram: hex`): `esphome compile` succeeded; also verified the
      `jpeg_quality` + `RAW10` combination is correctly rejected by `esphome
      config`. Documented the Home Assistant JPEG requirement and a working
      example config in a new README section. Scratch test directories removed
      afterward.
- 2026-09-19: **Camera entity still didn't appear in Home Assistant even
  after the JPEG fix landed and was reflashed** (`Pixel Format: RGB565`,
  `JPEG Re-encoding: quality 10 (enabled)` both confirmed present in the
  dump_config output, no errors anywhere in the device log). Ruled out a
  device-side crash/registration bug by enabling ESPHome integration debug
  logging in Home Assistant (Settings → Devices & Services → ESPHome →
  three-dot menu → Enable debug logging) and inspecting the raw
  `aioesphomeapi` protocol trace: the device's `ListEntitiesDoneResponse`
  arrived correctly, but **no `ListEntitiesCameraResponse` message was ever
  sent** - meaning the entity itself was excluded from the API's entity list
  on the device side, not merely mis-rendered by HA. Reproduced locally with
  a scratch compile matching the user's exact `mipi_csi_camera:` block
  (`id: my_camera`, no `name:`) and inspected the generated `main.cpp`: the
  entity's `configure_entity_(...)` call included the internal-flag bit
  (`// internal` in the generated comment) whenever only `id:` was set. This
  is standard ESPHome behavior for *any* entity (not specific to this
  component): an entity declared with only an `id:` (no `name:`) is treated
  as an automation/lambda-only helper and is automatically marked
  `internal: true`, which excludes it from `ListEntitiesIterator`
  (`component_iterator.cpp`'s `on_camera()` guard: `!camera_instance->
  is_internal() || include_internal_`) and therefore from the Home Assistant
  entity list entirely - regardless of pixel format or JPEG settings. This
  was not a bug in the component; it was a configuration gap in the example
  YAML that never set an explicit `name:`. Verified the fix with a matching
  scratch compile: adding `name: Camera` alongside `id: my_camera` changes
  the generated `configure_entity_()` call's flags from the internal bit set
  to `0` (visible). Updated the README's "Viewing the camera in Home
  Assistant" section to state this requirement explicitly and adjusted the
  example config to include both `id:` and `name:`. Scratch test directory
  removed afterward. User to add `name:` to their device YAML, reflash, and
  confirm the camera entity now appears in Home Assistant.

- **2026-09-20 — Fixed severely streaky/disturbed image (PPA output buffer
  cache-line alignment).** With the entity now visible, the user reported the
  displayed image was badly corrupted. The provided hardware log showed the
  PPA hardware rotator failing on essentially every captured frame:
  `ppa_core: out.buffer addr or out.buffer_size not aligned to cache line
  size` → `ESP_ERR_INVALID_ARG`, caught by the component's existing fallback
  (`PPA rotate failed: ... returning unrotated image`). Two separate defects
  were involved:
  1. `rotate_frame_()`'s destination buffer was allocated with a plain
     `heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`,
     which does not guarantee the DMA2D/cache-line alignment (start address
     *and* size) that `ppa_do_scale_rotate_mirror()` requires for its output
     buffer per ESP-IDF's PPA documentation
     (`docs/en/api-reference/peripherals/ppa.rst`, "Buffer Alignment"
     section) — the same class of bug previously found and fixed for the
     JPEG encoder's buffers, but never applied to the PPA rotation path.
  2. Even once the PPA call succeeds, its output is written via DMA into
     PSRAM; ESP-IDF drivers (e.g. `esp_driver_jpeg`) that do the same kind
     of DMA-into-PSRAM pattern explicitly call `esp_cache_msync(...,
     ESP_CACHE_MSYNC_FLAG_DIR_M2C)` afterwards to invalidate the CPU data
     cache for that range before reading it back — without this, the CPU
     (and the JPEG encoder reading from the rotated buffer) could observe
     stale/uninitialized cache lines instead of the PPA's actual output,
     which independently causes visual corruption.
  - **Fix applied** in `mipi_csi_camera.cpp`'s `rotate_frame_()`:
    - Added `#include "esp_cache.h"` and `#include
      "esp_private/esp_cache_private.h"` (the latter is required for
      `esp_cache_get_alignment()`'s declaration; without it the function is
      undeclared even though `esp_cache.h` is included — confirmed via a
      failing scratch compile before adding it).
    - Replaced the destination buffer's `heap_caps_malloc()` with
      `esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_line_size)`
      (64-byte fallback) followed by `heap_caps_aligned_alloc(cache_line_size,
      aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`, where
      `aligned_size` rounds `frame_size` up to a `cache_line_size` multiple.
    - Updated `srm_config.out.buffer_size` from `frame_size` to the new
      `aligned_size` to match the actual allocated/aligned capacity.
    - Added an `esp_cache_msync(dest, aligned_size,
      ESP_CACHE_MSYNC_FLAG_DIR_M2C)` call immediately after a successful
      `ppa_do_scale_rotate_mirror()`, before `dest` is returned to the
      caller (JPEG encoder / raw-frame listeners), to invalidate the CPU
      cache for the PPA's DMA output.
  - The V4L2 capture buffer (`src`, the PPA's *input*) is left untouched —
    it's managed by the `esp_video`/V4L2 driver's `DQBUF` path, which already
    handles cache coherency for captured frames internally.
  - **Verified** with a scratch compile (`tmp_test/test.yaml`, matching the
    user's config: OV02C10, 1288x728, 1 data lane, RGB565, jpeg_quality 10,
    rotation 90, `init_ldo: false`, plus a `name:` for the entity) — first
    attempt failed with `'esp_cache_get_alignment' was not declared in this
    scope` (missing the private header), second attempt after adding
    `esp_private/esp_cache_private.h` compiled successfully
    ("Successfully compiled program."). Scratch test directory removed
    afterward. Runtime PPA behavior can only be confirmed on real hardware.
  - User to cherry-pick/pull the fix, reflash, and confirm both that the PPA
    rotate warning no longer appears in the log and that the displayed image
    is no longer streaky/disturbed.

    - **2026-09-19 (follow-up) — PPA fix alone wasn't enough: image was still
      severely disturbed with color bands/splits/shifts (screenshot showed a very
      dark, green-tinted, badly corrupted image).** No new I2C/DMA/PPA errors
      appeared in the log this time, so the cause had to be something silent.
      Investigated two theories in parallel:
      1. **Row-stride mismatch.** Suspected the CSI/ISP pipeline might pad each
         captured row to an alignment boundary (`bytesperline` > `width *
         bytes_per_pixel`), which every downstream stage (PPA, JPEG encode, raw
         pass-through) assumed was *not* the case (tightly-packed rows only).
         Added defensive handling: `configure_format_()` now reads back
         `format.fmt.pix.bytesperline` after `VIDIOC_S_FMT`, and a new
         `destride_frame_()` helper repacks a captured frame into tightly-packed
         rows before rotation/encoding/pass-through if the driver ever reports a
         stride wider than the packed width (zero-copy no-op otherwise). Reading
         `esp-video-components`' own `esp_video_set_format()` source, however,
         showed it never actually writes a negotiated `bytesperline` back into the
         caller's `v4l2_format` struct (the function takes a `const` pointer and
         only forwards it to the sensor/ISP ops); in practice `capture_stride_`
         will therefore always equal the packed size and `destride_frame_()` is a
         no-op today. Kept as defensive code (harmless, in case a future esp_video
         version does start reporting a real stride) but this was **not** the
         actual cause.
      2. **Missing ISP auto-exposure/auto-gain/auto-white-balance ("3A") pipeline
         — this was the actual cause.** Found and reviewed an independent,
         similar community project (github.com/sullb/esphome-p4-csi-camera) that
         targets the same OV02C10 + ESP32-P4 ISP pipeline. Its code comments
         explicitly state: *"The ISP demosaic produces green-biased output
         without AWB"* and that AWB is normally handled by the `esp_ipa`
         component "via `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`" — that
         project works around the missing AWB by hand-correcting RGB565 pixels
         with fixed gain multipliers. Checked `esp-video-components`' own
         `esp_video/Kconfig`: `ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` defaults
         to **`n`**. Without it, no "isp_task" runs to read the ISP hardware's
         statistics and feed them to `esp_ipa`'s auto exposure/auto gain/auto
         white balance/color-correction algorithms - the ISP just performs a raw,
         uncorrected demosaic. That precisely explains the reported symptoms: a
         very dark image (no auto exposure/gain convergence), a green color cast
         (Bayer's 2x green sample density showing through with no AWB), and
         noisy/banded color patches (uncorrected demosaic + no denoising/color
         correction). `esp_ipa` is already an automatic managed-component
         dependency of `esp_video` on ESP32-P4 (`esp_video/idf_component.yml`),
         and its individual algorithms (AWB/AGC/AEC/ACC/ADN/AF/ATC) all default
         to enabled - only the pipeline controller task that drives them was
         off.
      - **Fix applied** in `components/mipi_csi_camera/__init__.py`: added
        `add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER",
        True)` alongside the existing MIPI-CSI/ISP video device options.
      - **Verified** with a scratch compile: confirmed the build now compiles
        `esp_video_isp_pipeline.c` (previously excluded when the controller was
        off) and that the generated `sdkconfig` contains
        `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y` alongside the
        already-enabled `CONFIG_ESP_IPA_AWB_ALGORITHM=y` /
        `CONFIG_ESP_IPA_AGC_ALGORITHM=y`. Scratch test directory removed
        afterward. Runtime image quality (does auto exposure/AWB actually
        converge and produce a clean image) can only be confirmed on real
        hardware.
      - User to cherry-pick/pull the fix, reflash, and confirm the image is no
        longer dark/green-tinted/banded. Auto exposure/AWB convergence may take a
        moment after boot or after significant scene changes, so allow the image
        a second or two to settle before judging quality.

    - **2026-09-19 (follow-up 2) — ISP pipeline controller made it *worse*, not
      better: image became a large solid saturated-red block with only a thin
      legible strip at the top.** That symptom (a single channel fully clipped
      across most of the frame) pointed at the auto gain/white-balance/color-
      correction loop driving its outputs to an unstable, maxed-out state
      rather than merely "not converged yet". Investigated two candidate
      causes:
      1. Reviewed the hand-ported `ov02c10.c` sensor driver's
         `ov02c10_set_total_gain_val()` gain-write path (one analog fine-gain
         register write is commented out). This matches the sensor's own
         documented capability (its analog gain path has no fine-gain step,
         only coarse), so this is *not* a bug — the driver's actual gain/exposure
         write path is otherwise complete and correctly wired to
         `esp_cam_sensor_ops`.
      2. Found the real cause: read `esp_ipa`'s own README
         (`espressif/esp-video-components`). Its auto white-balance/auto-gain/
         color-correction algorithms are **not** generic — each needs a
         per-sensor JSON calibration file (e.g.
         `esp_cam_sensor/sensors/sc2336/cfg/sc2336_default.json`,
         `ov5647_default.json`, `ov2710_default.json`) with sensor-specific
         tuning ranges (AWB gray-world bounds, gain-step limits, color
         correction matrix, etc.). **OV02C10 has no such calibration file**
         because it isn't part of Espressif's official `esp_cam_sensor`
         registry at all (this project's `ov02c10.c` is a hand-ported driver
         added specifically for this panel). With the ISP pipeline controller
         enabled, `esp_ipa`'s algorithms ran with no valid tuning data for this
         sensor and drove the color-correction/gain output to a degenerate,
         fully-saturated state — worse than the "dumb but at least stable"
         uncorrected demosaic from before.
      - **Fix:** reverted the `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER`
        sdkconfig option (removed from `__init__.py`, replaced with a comment
        explaining why it's intentionally left disabled for this sensor).
        Added a lightweight software workaround instead, matching the same
        approach used by the independent `sullb/esphome-p4-csi-camera`
        project: in `loop()`, when `pixel_format` is `RGB565`, apply a fixed
        per-channel gain correction (R ×1.30, G ×0.90, B ×1.30, clamped to each
        channel's bit depth) directly on the captured pixel data before
        rotation/streaming. This does not require any per-sensor calibration
        data and specifically counteracts the ISP's uncorrected demosaic's
        known green bias (Bayer sensors sample green at 2x the rate of red/
        blue). It does **not** provide auto exposure — very dark/bright scenes
        will still look under/over-exposed since gain/exposure now come solely
        from the sensor driver's built-in per-mode defaults (no closed-loop
        AEC), but this is a much safer starting point than a pipeline that can
        clip an entire frame to one saturated color.
      - **Verified** with a scratch compile (same OV02C10/1288x728/RGB565/90°
        config as before): compiled successfully, and confirmed the generated
        `sdkconfig` now contains `# CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
        is not set`. Scratch test directory removed afterward. Runtime image
        quality (is the color cast reduced, is exposure acceptable in the
        user's actual lighting) can only be confirmed on real hardware.
      - User to cherry-pick/pull the fix, reflash, and confirm the red-out is
        gone and report whether the remaining color/exposure looks
        "acceptable" or needs further tuning (e.g. adjusting the fixed gain
        multipliers, or exposing exposure/gain as a configurable option in a
        future iteration if the sensor's baked-in defaults are too dark/bright
        for the user's environment).

        - **2026-09-19 (maintainability cleanup) — separated OV02C10-specific code from
          the generic driver, and confirmed the horizontal/vertical mirror config
          option already exists and is wired up correctly.**
          - Extracted the two remaining pieces of OV02C10-only logic that were living
            directly in the generic `mipi_csi_camera.cpp`/`.h` (the linker
            keep-alive workaround for the vendored, non-`esp_cam_sensor`-registry
            driver, and the fixed RGB565 color-correction gains from the previous
            entry) into a new small adapter pair, `ov02c10_esphome.h`/`.cpp`, exposing
            just `ov02c10::force_link()` and `ov02c10::apply_rgb565_color_correction()`.
            `mipi_csi_camera.cpp` now only calls these two named functions (gated on
            `sensor_model_ == MIPI_CSI_SENSOR_OV02C10`) instead of containing any
            OV02C10-specific register/tuning detail itself, so it stays sensor-agnostic
            and a future second vendored (non-registry) sensor would only need its own
            `<name>_esphome.{h,cpp}` adapter, not changes to the generic capture/
            rotate/encode pipeline.
          - **Attempted, then reverted, moving all `ov02c10_*` files into a
            `sensors/ov02c10/` subfolder** for clearer separation. A scratch compile
            showed the subfolder's files were silently never copied into the ESP-IDF
            build tree at all (`fatal error: sensors/ov02c10/ov02c10_esphome.h: No
            such file or directory`, and inspecting the generated build source tree
            confirmed only the two top-level files were copied). Traced this to
            ESPHome's own component loader
            (`esphome/loader.py::ComponentManifest.resources()`): regular components
            (including git/local `external_components`) are only scanned with
            `recursive_sources=False`, so only files directly inside the component's
            own top-level directory are ever picked up - one-level subdirectories are
            only supported for ESPHome's own core code
            (`recursive_sources=True`, used solely by `esphome.core.config`). Moved
            all `ov02c10_*` files back to the flat top-level directory (their
            original location) to match this constraint; kept the new
            `ov02c10_esphome.h`/`.cpp` adapter files (also flat, prefixed
            `ov02c10_`) since that separation is still real and useful even without
            a subfolder. Updated `README.md`/`__init__.py` comments to describe the
            flat-with-prefix convention instead of a subfolder, and to document why
            a subfolder isn't possible, to save a future rediscovery of this
            constraint.
          - Checked the "mirror both horizontally and vertically" request against
            the existing code: `horizontal_mirror`/`vertical_flip` config options
            already exist, are already wired through `V4L2_CID_HFLIP`/`V4L2_CID_VFLIP`
            in `configure_format_()`, and the OV02C10 driver already implements both
            (`ov02c10_set_mirror()`/`ov02c10_set_vflip()`, simple register-bit
            toggles at 0x3821/0x3820) - reviewed this code and it looks complete and
            correct, unlike the earlier gain-control investigation. Setting both
            options to `true` mirrors the image on both axes; no new option was
            needed. Documented this explicitly in `README.md`'s new "OV02C10 color
            correction" section area so it isn't rediscovered as a gap later.
          - **Verified** with a scratch compile (same config as before, plus
            `horizontal_mirror: true`/`vertical_flip: true` added to exercise that
            code path): compiled successfully. Scratch test directory removed
            afterward. Runtime behavior (does the image actually appear mirrored on
            both axes) can only be confirmed on real hardware.
          - No behavior change from this cleanup beyond the (already-existing)
            mirror options being called out explicitly; user to reflash and confirm
            the color-correction fix from the previous entry still works as expected
            (this entry didn't change any of that logic, only where it lives).
