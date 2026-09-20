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
  `esp_video` (V4L2-style) sample for the ESP32-P4-Function-EV-Board. **This
  folder is a local-only copy of Guition's board vendor SDK, kept on
  contributors' machines during development - it is not committed to this
  repository** (it's untracked/gitignored-in-practice; too large and not
  ours to redistribute wholesale). Paths under it in this document are
  development notes for whoever has a local copy, not references to files
  that ship in the repo. See `components/mipi_csi_camera/README.md`'s
  "Attribution" section for how the one piece of code actually copied from
  it (the OV02C10 sensor driver) is attributed without depending on that
  local path.
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
- 2026-09-19: Checked where the vendored `ov02c10*` files with Espressif
  copyright headers actually come from, since the README's Attribution
  section cited `JC8012P4A1C_I_W_Y_New_Panel/video_lcd_display` as being "in
  this repository" - confirmed that folder is **untracked** in git in the
  main checkout and doesn't exist at all in this worktree, so that citation
  was factually wrong for anyone cloning the real repo. Found independent
  public confirmation that the driver is Espressif's own Apache-2.0 code,
  distributed only through board-vendor SDK drops (not the public
  `espressif/esp_cam_sensor` component registry, which has `os02n10`/
  `os04c10` but not `ov02c10`): `kdmukai/esp-board-common`'s `PROVENANCE.md`
  documents pulling the identical driver from a Guition `JC4880P443C_I_W.zip`
  SDK package, and `kruzio1985/BETTA-HA-PANEL-10-Guiton-JC8012P4A1C` vendors
  the same files the same way. Rewrote `README.md`'s Attribution section to
  cite the files' own SPDX headers plus these external, independently
  verifiable sources instead of the untracked local folder, and added a note
  in "Hardware / vendor reference facts" above flagging that folder as a
  local-only dev reference, not part of the repository.
- 2026-09-19: Audited the component's declared dependencies in `__init__.py`
  for gaps. `DEPENDENCIES = ["esp32", "i2c"]` and `AUTO_LOAD = ["camera"]`
  were already correct. Found two real gaps in `_final_validate()`:
  - No check that `esp32: framework: type` is `esp-idf`, despite the
    component being built entirely on ESP-IDF-only APIs (`esp_video`/
    `esp_cam_sensor` managed components, `driver/ppa.h`, `driver/
    jpeg_encode.h`, `esp_cache.h`, ...). Added a `cv.Invalid` check mirroring
    the existing ESP32-P4-variant check. In practice ESPHome's own `esp32`
    component already refuses Arduino for the P4 variant before this check
    would even run (confirmed via a scratch `esphome config` test), so this
    is currently unreachable in normal use - but it documents the real
    requirement explicitly and guards against any future ESPHome change that
    might loosen that restriction.
  - `rotate_frame_()` (and, defensively, the currently-no-op
    `destride_frame_()`) allocate their working buffer from
    `MALLOC_CAP_SPIRAM` whenever `rotation != 0`, but the existing psram
    check only fired for `jpeg_quality`. Extended the same check to also
    require `psram:` when `rotation != 0`, with a message naming the actual
    reason ("Rotation" vs "JPEG re-encoding"). Previously this failed softly
    at runtime (a logged warning + falling back to the unrotated frame) -
    now it's caught at config time instead, matching the existing UX for
    `jpeg_quality`.
  - **Verified** with three scratch `esphome`/`esphome compile` runs: (1)
    `rotation: 90` without a `psram:` block now fails config validation with
    the new "Rotation requires the 'psram' component..." message; (2) the
    same config with `framework: type: arduino` fails with ESPHome's own
    P4/Arduino rejection (before our check runs, as expected); (3) the full
    valid config (esp-idf, psram, rotation, mirror options) still compiles
    successfully end-to-end. Scratch test directory removed afterward.
  - Also updated `README.md`'s configuration table (`rotation` row) and
    "Supported hardware" section to document the psram-for-rotation and
    esp-idf-only requirements for future readers.
- 2026-09-19 (later): Refactored the sensor-specific code into a real plugin
  architecture, consolidated OV02C10's per-mode register files, and
  cross-checked our OV02C10 handling against two external references
  ([intel/ipu6-drivers](https://github.com/intel/ipu6-drivers)'s Linux
  `ov02c10.c`, and
  [sullb/esphome-p4-csi-camera](https://github.com/sullb/esphome-p4-csi-camera)'s
  `ov02c10_settings.h`), per the user's request.
  - **New `SensorExtension` interface**
    (`mipi_csi_camera_sensor_extension.h`): a small virtual interface
    (`force_link()`, `apply_rgb565_color_correction()`) that sensors vendored
    outside the `esp_cam_sensor` registry implement to hook into
    `setup()`/`loop()`. `sensor_extensions.cpp` is the single touch point
    that maps `MipiCsiSensorModel` -> `SensorExtension*` (returns `nullptr`
    for SC2336/OV5647, which don't need one). `mipi_csi_camera.cpp`/`.h` no
    longer contain any `if (sensor_model_ == MIPI_CSI_SENSOR_OV02C10)`
    special-casing at all - they just call through
    `this->sensor_extension_` (cached once in `setup()`) when it's non-null.
    Adding a future vendored sensor now means: add its `<name>_*` driver
    files, implement `SensorExtension` for it (see `ov02c10_esphome.h`/`.cpp`
    for the reference shape, now `class Ov02c10Extension final : public
    SensorExtension`), and add one `case` to `get_sensor_extension()` -
    nothing else in the generic driver changes.
  - **Consolidated OV02C10's three per-mode register header files**
    (`ov02c10_mipi_1lane_24Minput_1288x728_raw10_30fps.h`,
    `..._1lane_24Minput_1920x1080_...`, `..._2lane_24Minput_1920x1080_...`)
    into the single `ov02c10_settings.h`, matching the structure
    `sullb/esphome-p4-csi-camera` uses for the same driver (one
    `ov02c10_settings.h` holding every mode's register array, rather than one
    file per mode) - per the user's explicit request ("we dont need multiple
    dedicated configuration options files"). Verified byte-for-byte
    completeness by counting `{0x...` register-entry lines before/after
    (226 entries x 3 modes = 678, matched exactly) before deleting the three
    old files.
  - **Found and fixed a real, previously-untested bug** while verifying the
    refactor: `ov02c10.c` is always compiled by ESPHome regardless of which
    `sensor:` is selected (nothing gates the whole file on `sensor ==
    OV02C10`), but `__init__.py` only ever supplied
    `CONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DEFAULT` as a build flag when
    OV02C10 was selected - so selecting `SC2336` or `OV5647` failed to
    *compile* at all (`'CONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DEFAULT'
    undeclared`). This had apparently never been scratch-compiled before
    (all prior scratch tests in this project used OV02C10). Fixed with a
    `#ifndef`-guarded fallback default (`0`) in `ov02c10_compat.h`; harmless
    when unused since none of the `CONFIG_CAMERA_OV02C10_MIPI_RAW10_*` mode
    macros are defined either in that case, so
    `ov02c10_mipi_format_index[]` is simply empty.
  - **Cross-checked against `intel/ipu6-drivers`'s `ov02c10.c`** (the Linux
    kernel/IPU6 driver for the same sensor, used on Intel platforms):
    confirms the same register semantics we already use (`0x0100`
    standby/streaming toggle, `0x3508`/`0x350a` analog/digital gain,
    `0x3501`/`0x3502` exposure, `0x380c-0x380f` HTS/VTS) and the same
    stream-start/stop sequence pattern (`REG_MODE_SELECT` write last after
    programming a mode, standby write first when stopping) that
    `ov02c10_set_stream()` in our vendored `ov02c10.c` already implements -
    no discrepancy found, so no changes made there. Also noted the Intel
    driver's `OV02C10_REG_TEST_PATTERN` (`0x4503`, bit 7) - a built-in test
    pattern generator that could help distinguish a genuine sensor/CSI
    problem from a downstream (ISP/rotation/JPEG) processing bug in the
    future, but wasn't added as a user-facing option in this pass since it
    wasn't asked for and the current image-quality issues already have an
    identified, unrelated root cause (ISP demosaic, addressed by the color
    correction above) - noted here as a candidate future diagnostic aid
    instead.
  - **Cross-checked against `sullb/esphome-p4-csi-camera`'s
    `ov02c10_settings.h`**: byte-for-byte identical register tables to ours
    (same Espressif-authored driver), confirming our consolidation approach
    (one settings file, register tables selected via the same
    `CONFIG_CAMERA_OV02C10_MIPI_RAW10_*`-style build flags) matches how an
    independent implementation for the same sensor already organizes this.
    No additional settings/power-mode knobs were found there beyond what our
    driver already exposes (gain/exposure/mirror/flip via V4L2 controls,
    mode-table-driven resolution/framerate).
  - **Verified** with three scratch `esphome compile` runs: OV02C10 (with
    `horizontal_mirror`/`vertical_flip`/`rotation` all set, to exercise the
    color-correction and sensor-extension force-link path) compiled and
    linked successfully; SC2336 (exercising the `nullptr` sensor-extension
    path, and the just-fixed compile bug) failed before the
    `ov02c10_compat.h` fix and compiled successfully after it. Scratch test
    directory removed afterward.
  - Updated `README.md`: new "Adding another sensor" section describing the
    `SensorExtension` extension point, updated "How it works"/"OV02C10 color
    correction"/"Attribution" sections to reference the new
    `Ov02c10Extension`/`sensor_extensions.cpp` names instead of the old
    free-function `ov02c10::force_link()`/`ov02c10::apply_rgb565_color_correction()`,
    and updated the per-mode-file mentions to reflect the single
    `ov02c10_settings.h`.

- **Fixed the persistent torn/shifted-image bug (missing cache invalidation
  after DMA capture)**: user reported that after the refactor, images were
  still visibly torn/shifted with diagonal color-block artifacts, and colors
  changed every other frame (an unreliable signal, so color-based debugging
  of the artifact wasn't pursued further). Root-caused by re-reading the
  V4L2 capture path in `mipi_csi_camera.cpp`:
  - `capture_task()` dequeues a filled buffer via `VIDIOC_DQBUF` and hands
    its index to `loop()` via `frame_queue_`; `loop()` then reads
    `capture_buffers_[index]` directly (via `destride_frame_()` /
    `rotate_frame_()` / JPEG-encode / raw pass-through).
  - The MIPI-CSI/ISP DMA engine writes captured frames directly into PSRAM.
    On the ESP32-P4, DMA writes to PSRAM are **not** automatically coherent
    with the CPU data cache - the CPU can still see stale, previously-cached
    bytes for that same buffer address until the cache is explicitly
    invalidated for that range.
  - The code already did this correctly for the *rotated* buffer (the PPA
    hardware rotator's DMA output was invalidated via `esp_cache_msync(...,
    ESP_CACHE_MSYNC_FLAG_DIR_M2C)` before use), but the **raw captured
    buffer itself was never invalidated after `VIDIOC_DQBUF`** before any of
    destride/rotate/encode touched it. This meant every frame was read as a
    mix of fresh DMA'd bytes and stale cached bytes from a previous
    read/frame - a near-perfect match for "torn/shifted image, colors change
    every other frame" (the exact stale/fresh mix depends on unrelated cache
    eviction activity elsewhere in the firmware, so it looked essentially
    random from frame to frame).
  - **Fix**: added an `esp_cache_msync(raw, this->capture_buffer_size_,
    ESP_CACHE_MSYNC_FLAG_DIR_M2C)` call in `loop()` immediately after
    obtaining the raw buffer pointer for the dequeued index, before any
    processing touches it. This runs on whichever core executes `loop()`
    (the actual consumer), matching the pattern already used for the PPA
    rotate output.
  - Verified via a scratch `esphome compile` (OV02C10, RGB565, rotation 90):
    compiled and linked successfully. This is a data-correctness fix with no
    build-time behavior change to verify other than compilation; the real
    verification is on hardware.
  - **Next**: ask the user to reflash and confirm whether the tearing is
    resolved. If any residual artifact remains, the next suspects would be
    the destride/rotate paths' own buffer handling (already cache-safe) or a
    genuine capture-buffer-count/timing issue, but the missing invalidate on
    the raw DMA buffer was the most direct explanation for the reported
    symptom and had not been checked before.

- **Follow-up: raised the default `frame_buffer_count` from 2 to 3 (max
  raised from 3 to 4)**: after the cache-invalidate fix, the user reported
  the tearing was less frequent/severe but still present - a screenshot
  showed a frame apparently spliced from two temporally different captures
  (a distinct horizontal seam with a different color cast above vs. below
  it, not just row noise). Re-reviewing `capture_task()`/`loop()`'s
  buffer-ownership handling found a second, independent bug: with only 2
  total V4L2 buffers, the worst case has **both** buffers held outside the
  driver's free-list at the same time - one sitting consumed-but-unread in
  the single-slot `frame_queue_`, and one actively being read by `loop()`
  after being pulled off that queue (destride/rotate/JPEG-encode can take
  a while) - leaving **zero** buffers free for the CSI/ISP DMA engine to
  capture the next frame into. Depending on how gracefully the underlying
  esp_video/CSI driver handles buffer starvation, it may keep writing into
  a buffer anyway even though it's still logically "checked out", tearing a
  frame that's mid-read - a plausible explanation for the two-different-
  captures-spliced-together artifact. With 3 buffers there is always at
  least one free for capture (1 in-queue + 1 being processed + 1 free).
  Bumped the default from 2 to 3 and the allowed range from `2-3` to `2-4`
  (kept 2 available for memory-constrained setups, with a new runtime
  `ESP_LOGW` warning when configured below 3 explaining the tearing risk).
  Verified via a scratch `esphome compile` (OV02C10, RGB565, rotation 90,
  default frame_buffer_count): compiled successfully. **Awaiting hardware
  retest** to confirm whether this fully resolves the tearing or whether a
  further timing issue remains.

- **User retested with 1920x1080/1-lane (the vendor-proven combo) and the
  bumped frame_buffer_count: tearing was unchanged in both regards**,
  disproving both the register-table-timing hypothesis and the buffer-
  starvation hypothesis - the artifact is resolution/lane/buffer-count
  independent, meaning it's a bug in our own processing pipeline, not the
  sensor or V4L2 buffer plumbing. The reported screenshot showed a sharp
  horizontal seam: a coherent, correctly-colored image above it, and an
  entirely unrelated-looking, differently-lit/differently-colored scene
  below it - with `rotation: 0` (ruling out the PPA rotate path entirely).

  - **Root-caused the real bug**: in `loop()`, `frame_size` (the size
    trusted as "the valid captured frame", fed into RGB565 color-correction
    and the JPEG encoder) was computed as `capture_buffer_size_` in the
    common case where `destride_frame_()` is a no-op (driver's row stride
    already matches the packed width - the normal case for RGB565 on this
    hardware). `capture_buffer_size_` is `buf.length` from
    `VIDIOC_QUERYBUF` - the V4L2 buffer's **allocated capacity** - not
    necessarily equal to `width * height * bytes_per_pixel` for the
    current frame. Any padding/rounding the driver applies when sizing
    that allocation is *trailing bytes within the very same reused PSRAM
    buffer* left over from whatever was previously written there (a prior
    frame, at a prior resolution/rotation, or simply stale memory). Our
    code fed all of `capture_buffer_size_` - real pixels *and* that
    trailing stale region - into color correction and JPEG encoding as if
    it were all valid image data, producing a frame that's correct up to
    the real data boundary and garbled/unrelated beyond it. This explains
    every piece of evidence: identical behavior across resolutions/lane
    counts/buffer counts (the bug is in our own size accounting, not
    capture timing), and no dependence on rotation (the bug is upstream of
    `rotate_frame_()`).
  - **Fix**: `frame_size` is now unconditionally `width * height *
    bytes_per_pixel` (the exact size of a de-strided frame), regardless of
    whether `destride_frame_()` performed a real copy or was a no-op.
    `capture_buffer_size_` remains correctly used elsewhere (buffer
    `munmap()` size, and the cache-invalidate range, where using the full
    allocated capacity is safe/conservative rather than wrong).
  - Verified via a scratch `esphome compile` (OV02C10, RGB565, 1920x1080,
    1 lane, rotation 0, matching the user's exact failing config): compiled
    successfully. **Awaiting hardware retest** - this is the most direct,
    concrete explanation found so far for the persistent tearing, and
    unlike the two previous fixes it explains why changing resolution/lanes
    and buffer count had no effect.

- **User retested the `frame_size` fix: still corrupted**, though the new
  screenshot showed a different-looking variant of the same problem class:
  a correct/recognizable top portion, then several repeated bands showing
  the *same* scene content again with progressively worse color drift, and
  finally the bottom of the frame degenerating into high-contrast
  blocky/speckled noise. Ruling out yet another theory required two
  separate investigations:

  - **Hardware/signal-integrity check**: had the user flash the vendor's
    own unmodified `video_lcd_display` example (from
    `JC8012P4A1C_I_W_Y_New_Panel`) onto the same physical device. Its
    camera preview showed **no tearing at all** - conclusively ruling out
    a hardware problem (bad FPC cable, camera module defect, insufficient
    LDO sequencing, MIPI signal integrity) and confirming the bug is
    somewhere in our own software.
  - **Init/config cross-check**: dispatched a background research pass
    comparing our `setup()`/`configure_format_()`/ISP/CSI-PHY
    configuration against the vendor's `app_video.c` line-by-line
    (`esp_video_init_csi_config_t` fields, ISP enablement, `esp_cam_ctlr`
    settings, buffer memory mode, stream-start ordering,
    `ov02c10_isp_info_mipi[1]` metadata for the exact mode used). Result:
    no meaningful difference found. The one flagged difference
    (`CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` off in our build,
    on in the vendor's) turned out to be the ISP's *auto-exposure/AWB 3A
    control loop* (esp_ipa) - a feature we deliberately left disabled
    already (documented in `loop()`'s comments: enabling it made images
    worse, not better, since this sensor lacks the calibration data that
    algorithm needs). Not a bug, and unrelated to the corruption.

  - **New root cause found**: our `capture_task()`/`loop()` pipeline was
    discarding a critical piece of information the V4L2 driver provides on
    every dequeue - `struct v4l2_buffer`'s `bytesused` field, the *actual*
    number of valid bytes the CSI/ISP DMA wrote for that specific captured
    frame. We only ever threaded the buffer *index* through
    `frame_queue_`, then unconditionally assumed the whole
    `width * height * bytes_per_pixel` region was valid (this is exactly
    what the previous `frame_size` fix hard-coded). If the CSI receiver
    ever only manages to write *part* of a frame during a given capture
    cycle (a dropped/short line group, a synchronization hiccup, an ISP
    stall) - which the vendor's simpler/slower unthrottled capture loop
    may simply never trigger, or triggers rarely enough that it wasn't
    reported as visible tearing - then everything past the real
    `bytesused` boundary in that (reused) buffer is stale leftover pixel
    data from a *previous* capture into the same buffer, not fresh data.
    Encoding that stale tail as if it were valid image content produces
    exactly the observed pattern: a correct region, followed by
    recognizable-but-wrong repeated content (stale previous frame(s)),
    followed by effectively random noise (long-uninitialized/very old
    buffer content) - independent of resolution, lane count, rotation, or
    buffer count, since none of those change whether a given capture cycle
    was complete.
  - **Fix**: `capture_task()` now threads a small `CapturedFrame{index,
    bytesused}` struct through `frame_queue_` instead of a bare buffer
    index. `loop()` compares the driver-reported `bytesused` against the
    expected `packed_size` (`width * height * bytes_per_pixel`): if
    `bytesused` is non-zero and smaller than expected, the frame is a
    confirmed short/partial capture - it's dropped outright (buffer
    immediately requeued to the driver) rather than encoding known-corrupt
    data into a JPEG and sending it to Home Assistant. A `ESP_LOGW` logs
    every dropped frame with both byte counts, and a `ESP_LOGD` logs any
    other bytesused/expected-size mismatch, so hardware testing will now
    produce direct log evidence either confirming or ruling out this
    theory, rather than more guesswork from screenshots alone.
  - Verified via a scratch `esphome compile` (OV02C10, RGB565, 1920x1080,
    1 lane): compiled successfully. **Awaiting hardware retest** with log
    capture - if `ESP_LOGW "Dropping short frame"` messages appear
    correlated with corrupted-looking frames, this confirms the theory;
    if frames are never reported short yet corruption persists, the next
    step is instrumenting a raw (non-JPEG, pre-ISP) Bayer dump for
    byte-level inspection off-device, decoupling the diagnosis from
    Home Assistant's rendering entirely.

- Hardware retest of the `bytesused` fix was inconclusive: the provided
  log showed no `"Dropping short frame"` messages, but also no evidence
  the camera was actually viewed/streamed during that capture window, so
  the theory was neither confirmed nor refuted.
- However, the same retest surfaced a new, more serious, and clearly
  resolution-dependent bug: immediately after flashing 1920x1080, the
  device hit `Guru Meditation Error: Core 1 panic'ed (Interrupt wdt
  timeout on CPU1)` on boot, requiring two automatic reboots before
  settling into a previous (1288x728) build. The user confirmed reverting
  to 1288x728 avoids the crash entirely, and that after reverting there is
  still no `"Dropping short frame"` log line - meaning the earlier
  "1920x1080 shows no improvement" test was likely never actually
  exercising a running 1920x1080 build in the first place.
- Root cause found by reading ESP-IDF's actual
  `components/esp_mm/esp_cache_msync.c`: for the **M2C** (invalidate)
  direction - exactly what our `loop()` uses on the raw captured buffer,
  and what `rotate_frame_()` uses on the PPA output buffer - the entire
  requested range is invalidated inside a single, unchunked critical
  section with interrupts disabled for the whole call
  (`esp_os_enter_critical_safe` / `cache_hal_invalidate_addr` /
  `esp_os_exit_critical_safe`). ESP-IDF does provide a chunking mitigation
  for exactly this problem (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS`),
  but it **only applies to the C2M (writeback) direction** - there is no
  equivalent for M2C. At 1920x1080 RGB565 (~4.1MB) vs 1288x728 RGB565
  (~1.9MB), the interrupt-disabled window scales with buffer size; 1920x1080
  apparently crosses the interrupt watchdog's threshold outright, while
  1288x728 stays under it but could still be long enough to delay the
  CSI/ISP driver's own interrupt-driven buffer handoff - a very plausible
  explanation for the "correct top, repeated/color-shifting middle bands,
  noisy bottom" pattern seen at every resolution tested so far, since it is
  driven by total buffer size rather than resolution, lane count, rotation,
  or buffer count (all previously ruled out).
- **Fix implemented**: added a small `invalidate_cache_chunked()` helper in
  `mipi_csi_camera.cpp` that loops `esp_cache_msync(..., ESP_CACHE_MSYNC_FLAG_DIR_M2C)`
  over fixed 32 KB chunks instead of invalidating the whole buffer in one
  call, keeping each individual critical section short so interrupts get
  serviced between chunks. Both M2C invalidate call sites now use it: the
  raw captured-frame invalidate in `loop()`, and the PPA rotate-output
  invalidate in `rotate_frame_()`. (Chunk starts/sizes stay cache-line
  aligned automatically: the original buffers were already validated as
  aligned when a single unchunked call worked, and 32 KB is itself a
  multiple of any realistic cache line size, so every chunk - including the
  final, possibly-shorter one - remains a multiple of the cache line size.)
  Verified via a scratch `esphome compile` (OV02C10, RGB565, 1920x1080, 1
  lane): compiled successfully. **Awaiting hardware retest** on both
  1920x1080 (does the boot-time WDT crash disappear?) and 1288x728 (does
  the visual tearing improve or disappear?), with logs specifically
  checked for `"Dropping short frame"` warnings and for any repeat of the
  WDT panic.

- **Before that hardware retest happened**, asked to double-check whether
  the reference `esphome-p4-csi-camera` component does the same manual
  cache invalidation on the raw captured buffer. It does not call
  `esp_cache_msync()` anywhere at all. Investigating why led to the real
  root cause: ESP-IDF's own CSI controller driver
  (`components/esp_driver_cam/csi/src/esp_cam_ctlr_csi.c` in
  `espressif/esp-idf`) already invalidates each completed buffer's cache
  itself, **inside its own DMA-done ISR**, immediately when a transfer
  finishes and using the exact `received_size` actually captured -
  *before* the V4L2/`esp_video` layer ever calls back and hands the buffer
  to our code via `VIDIOC_DQBUF`. Our own manual invalidate in `loop()` was
  therefore never necessary in the first place.
- This reframes every prior fix attempt in this investigation: none of
  them (rotation, lanes, framerate, buffer count, resolution, `bytesused`
  short-frame detection, even the chunked invalidate from immediately
  above) addressed the actual defect, because the redundant invalidate
  itself was still present in all of them. The most likely mechanism for
  the tearing: our manual `esp_cache_msync()` call runs on the application
  task, disables interrupts for its critical section, and directly
  competes for PSRAM bus bandwidth with the CSI hardware's own real-time
  DMA of the *next* incoming frame - a large enough or badly-timed stall
  here could plausibly cause the CSI receiver to lose synchronization
  mid-frame, producing exactly the "correct region, then repeated/shifted
  bands, then noise" pattern reported throughout this investigation. It
  also explains the resolution-dependent WDT crash directly: a bigger
  buffer means a bigger unnecessary critical section stacked on top of the
  ISR's own (already-sufficient) invalidate.
- **Fix**: removed the manual `esp_cache_msync()` / `invalidate_cache_chunked()`
  call on the raw captured buffer in `loop()` entirely, with a comment
  explaining why it's unnecessary and citing the ESP-IDF driver source.
  The `invalidate_cache_chunked()` helper is kept and still used for the
  one place we *do* own the DMA coherency ourselves: the PPA rotate output
  buffer in `rotate_frame_()` (the PPA is a separate hardware block from
  the CSI controller and is not covered by the CSI driver's internal
  invalidate). Verified via a scratch `esphome compile` (OV02C10, RGB565,
  1288x728, 1 lane): compiled successfully. **Awaiting hardware retest** -
  this is now considered the most likely actual fix for the tearing (not
  just a defensive/safety improvement like the previous attempts), so
  results from this retest are the most important signal yet in this
  investigation.

- Hardware retest result: **no improvement** - image still streaked/torn,
  and no "Dropping short frame" log lines (so frames are never reported
  short by the driver). This ruled out the cache-invalidate theory as the
  root cause (removing it was still correct - it was redundant - but it
  wasn't the streaking mechanism).
- **Systematic three-way diff (ours vs reference `esphome-p4-csi-camera`
  vs vendor `video_lcd_display` demo)** - this found the actual answer:
  - Sensor register tables: ours are byte-identical to the vendor's own
    esp_cam_sensor ov02c10 driver (which our port came from) and
    content-identical to the reference's tables. The misleading
    "2lane...10fps" table name for the 1288x728 1-lane mode is just a
    leftover label from the vendor's source; contents are the proper
    1288x728 mode. NOT the bug.
  - esp_video version: the vendor demo uses `~2.0` from the registry -
    the SAME version we use (the reference uses git master, but the vendor
    demo proves ~2.0 works). NOT the bug.
  - ISP pipeline controller (esp_ipa 3A): esp_video's own Kconfig help
    confirms it is only a statistics->algorithm->tuning task for image
    *quality* (AE/AWB/AF); it does not touch the capture data path, so it
    cannot cause streaks/shifts. Confirmed NOT the bug (our decision to
    leave it off stands; note the vendor's esp_cam_sensor fork actually
    DOES ship OV02C10 esp_ipa calibration JSONs
    (`sensors/ov02c10/cfg/ov02c10_default_p4_eco4/eco5.json`) - so
    re-enabling 3A WITH that calibration data is a viable future
    color/exposure-quality improvement, revisited after the streaking is
    fixed).
  - **Data lanes: THE difference.** Every known-good configuration uses
    TWO data lanes: the vendor demo
    (`CONFIG_CAMERA_OV02C10_MIPI_RAW10_1920x1080_2LAN_30FPS=y`) and the
    reference component (`data_lanes: 2`, auto-detect default mode).
    Every failing configuration of ours used `data_lanes: 1` (1288x728
    and 1920x1080 alike).
  - Link budget math (from the vendor's own isp_info metadata):
    - 1288x728 1-lane: pclk=81MHz x 10bit RAW = **810 Mbps payload** on a
      400MHz DDR lane = **800 Mbps capacity** - undersized by ~1.2%
      *before* CSI-2 packet overhead -> chronic sensor FIFO overrun.
    - 1920x1080 1-lane: 810 Mbps payload vs 405MHz DDR = 810 Mbps -
      exactly zero margin, overhead pushes it over.
    - 1920x1080 2-lane (vendor demo): 840 Mbps payload vs 2x810 = 1620
      Mbps capacity -> ~48% headroom. Clean.
    A chronically overrun link drops/garbles bytes mid-frame at drifting
    offsets - which produces exactly the reported symptoms: recognizable
    image with streaks, horizontal splits/shifts, repeated bands, and
    colors that drift frame-to-frame; and it is immune to every
    software-side fix we tried (buffers, stride, cache, rotation,
    framerate) because the loss happens on the wire before any software
    sees the data.
  - **Action (pure YAML change, no code change needed - our component
    already ships the vendor's byte-identical 2-lane table):**
    `resolution: 1920x1080, data_lanes: 2` - exactly the vendor demo's
    proven mode. **Awaiting user hardware retest.**

- User retested with `1920x1080, data_lanes: 2`: **crash again**
  (`Interrupt wdt timeout on CPU1`) - but this time the log contained the
  smoking gun: hundreds of `ISP: fifo overflow` errors right before the
  panic (the error-interrupt log spam itself is what trips the watchdog -
  esp_video's own Kconfig help for ESP_VIDEO_DISABLE_ISP_ERROR_INTERRUPT
  describes exactly this failure mode).
- Re-checked the earlier 1288x728/1-lane log: it ALSO contained 322
  `ISP: fifo overflow` errors (previously overlooked while grepping for
  "Dropping short frame"). **The ISP input FIFO overflows at every
  resolution and lane count** - which kills the 1-lane link-budget theory
  (all OV02C10 modes feed the ISP at the same ~80 Mpx/s pclk; the MIPI
  lane rate was never the bottleneck) and points at ISP throughput.
- Root cause found in esp_video's own CHANGELOG: **esp_video 2.0.x-2.2.x
  hardcode the ISP processor clock to 80 MHz regardless of clock source**;
  2.3.0 fixed it ("`clk_hz` now derived from `clk_src` (XTAL/PLL160/
  PLL240) **to prevent FIFO overflow errors**"). We pinned
  `espressif/esp_video ~2.0`, i.e. 2.0.x - ISP running at exactly the
  sensor's ~80 Mpx/s output rate, so the slightest DMA/PSRAM contention
  overflows the ISP input FIFO -> torn/streaked frames at every
  resolution, matching every symptom and every failed software fix.
  (The vendor demo got away with the same esp_video 2.0.x because it ran
  on ESP-IDF 5.4.0; we run ESPHome's IDF 5.5.5, whose refactored ISP/
  MIPI-CSI drivers behave worse with the misconfigured clock. The
  reference component sidesteps it entirely by using esp_video master.)
- **Fix**: bumped `espressif/esp_video` to `~2.4.1` (ISP clock fix in
  2.3.0 + ISP/MIPI-CSI driver compatibility fixes for the IDF 5.5.x line
  in 2.4.1) and `espressif/esp_cam_sensor` to `~2.4.0` (esp_video 2.4.1's
  required version), and enabled
  `CONFIG_ESP_VIDEO_DISABLE_ISP_ERROR_INTERRUPT` (added in 2.4.0 for
  exactly this crash mode) so a stray overflow can never watchdog-reset
  the device again. Resolved versions verified in the scratch build:
  esp_video 2.4.1, esp_cam_sensor 2.4.0; vendored OV02C10 driver compiles
  unchanged against the 2.4 API. Verified via a scratch `esphome compile`
  (OV02C10, RGB565, 1920x1080, 2 lanes): compiled successfully.
  **Awaiting user hardware retest** - expectation: no `fifo overflow`
  lines, no WDT crash, and (finally) a clean image. The 1-lane modes
  remain link-marginal on paper, so 2-lane 1920x1080 stays the
  recommended config regardless.

- Follow-up: user requested going straight to the newest esp_video
  instead, so the pin is now `~2.5.0` (2.5.0 is the latest release;
  nothing newer exists yet). This additionally pulls esp_cam_sensor
  2.6.0 + esp_ipa 2.4.0. Vendored OV02C10 driver still compiles unchanged
  against the 2.6 API (verified via scratch `esphome compile`, same
  config as above). If any regression shows up on hardware, falling back
  to `~2.4.1` (which also contains the FIFO-overflow fix) is a one-line
  change.

- Hardware retest with esp_video 2.5.0: **the FIFO overflows and the WDT
  crash are gone** - streaming runs stable. New failure instead:
  `Failed to allocate JPEG output buffer (4147200 bytes)` every frame at
  1920x1080. Cause: `encode_jpeg_()` allocated *both* a ~4.1MB input copy
  buffer and a ~4.1MB output buffer via `jpeg_alloc_encoder_mem()` on
  every single frame, and freed them right after - by the time the camera
  streams, the display/LVGL stack has fragmented PSRAM enough (~10MB free
  total, but no contiguous 4.1MB block left) that these per-frame
  allocations fail. (1288x728 never hit this because its ~1.9MB buffers
  still fit in the fragmented heap.)
- **Fix**: persistent buffers, same pattern as the reference component -
  the output buffer is allocated once in `setup()` (while PSRAM is still
  contiguous, failing fast with a clear log line if even that doesn't
  fit) and reused for every frame; `encode_jpeg_()` now encodes directly
  from the source buffer (V4L2 capture / PPA rotate buffers are
  cache-line aligned in practice, like in the reference component) and
  only falls back to a lazily-allocated persistent input copy buffer if
  the driver rejects the source as misaligned. This also removes ~8.3MB
  of per-frame memcpy+alloc churn. Verified via scratch `esphome compile`
  (1920x1080, 2 lanes). **Awaiting user hardware retest.**

- Retest result: **no more crash, no more `fifo overflow`** - but frames
  still showed tearing, and brightness/colors shifted from frame to frame.
  The attached snapshot showed a structurally intact frame (recognizable
  scene, no splits/bands) that was badly underexposed and color-drifting -
  i.e. the *capture* path is healthy now and the remaining issue is image
  *quality/stability*, which is exactly what the ISP pipeline controller
  (esp_ipa 3A: auto exposure/gain/white balance) exists to fix. Both
  known-good references (vendor demo AND esphome-p4-csi-camera) run with
  it enabled; we had it disabled because OV02C10 previously had no esp_ipa
  calibration data, and enabling 3A without calibration made things worse.
- **Calibration data found**: the vendor's bundled esp_cam_sensor fork
  ships OV02C10 esp_ipa calibration JSONs
  (`sensors/ov02c10/cfg/ov02c10_default_p4_eco4.json` (10KB) and
  `ov02c10_default_p4_eco5.json` (204KB), Apache-2.0). Mechanism:
  esp_cam_sensor's `project_include.cmake` registers each enabled sensor's
  JSON into the global `ESP_IPA_JSON_CONFIG_FILE_PATH` build property, and
  esp_ipa's CMakeLists compiles it into `esp_video_ipa_config.c`. Our
  vendored OV02C10 driver bypasses esp_cam_sensor's build, so nothing ever
  registered its JSON.
- **Fix implemented**: vendored both JSONs into the component, added a new
  `isp_pipeline_controller` YAML option (default `true`), wired
  `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` to it, and - because
  ESPHome merges external components into the single `src` main component
  (so a `project_include.cmake` in our own directory would never be
  processed) - the Python codegen now *generates* a `project_include.cmake`
  into the main component directory that registers the right JSON
  (eco4/eco5 selected by `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, same gate
  the vendor uses). The manual fixed green-cast correction is now skipped
  when the pipeline is enabled (real AWB supersedes it). Verified end to
  end in a scratch build: `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y`,
  the IPA config generator ran, and the generated `esp_video_ipa_config.c`
  contains the OV02C10 calibration arrays. **Awaiting user hardware
  retest** - expectation: correct exposure, stable colors, no pumping.

- Retest result: the pipeline itself works (auto exposure converged:
  luma climbed 11 -> 42 against target 45 and held there), BUT the device
  became unusable - WiFi/API never came up. Cause: the esp_ipa algorithms
  log ~10 DEBUG lines per frame each (~1500 lines in 14s, hundreds per
  second); on a DEBUG-level ESPHome config that logger flood starves the
  main loop. First fix attempt: clamp the `esp_ipa_*` log tags to WARN via
  esp_log_level_set() in setup().
- Retest of the clamp: **no effect** - same spam, and this time the device
  crashed with a task watchdog reset: loopTask starved while isp_task sat
  inside `uart_tx_all` (decoded from the register dump). Root cause of the
  clamp failing: ESPHome builds with `CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=n`
  (esphome/components/esp32/__init__.py), which makes `esp_log_level_set()`
  a no-op at runtime.
- **Second fix**: wrap the installed vprintf handler
  (`esp_log_set_vprintf`, saving the previous one) and drop DEBUG/VERBOSE
  messages whose tag starts with `esp_ipa` - the IDF log formatter embeds
  level and tag in the format string itself (`D (12345) esp_ipa_agc: ...`,
  optionally ANSI-color-prefixed), so filtering there works regardless of
  the dynamic-level-control setting. Verified via scratch
  `esphome compile`. **Awaiting user hardware retest.**

