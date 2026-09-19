import logging
from typing import Any

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.components.psram import DOMAIN as psram_domain
import esphome.config_validation as cv
from esphome.const import (
    CONF_BRIGHTNESS,
    CONF_CONTRAST,
    CONF_I2C_ID,
    CONF_ID,
    CONF_RESET_PIN,
    CONF_RESOLUTION,
    CONF_ROTATION,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE
from esphome.core.entity_helpers import setup_entity
import esphome.final_validate as fv
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@jtenniswood"]
AUTO_LOAD = ["camera"]
DEPENDENCIES = ["esp32", "i2c"]

# Only the ESP32-P4 has a MIPI-CSI receiver.
ESP32_VARIANT_ESP32P4 = "ESP32P4"

mipi_csi_camera_ns = cg.esphome_ns.namespace("mipi_csi_camera")
MipiCsiCamera = mipi_csi_camera_ns.class_(
    "MipiCsiCamera", cg.Component, cg.EntityBase
)
MipiCsiCameraImageTrigger = mipi_csi_camera_ns.class_(
    "MipiCsiCameraImageTrigger", automation.Trigger.template()
)
MipiCsiCameraStreamStartTrigger = mipi_csi_camera_ns.class_(
    "MipiCsiCameraStreamStartTrigger", automation.Trigger.template()
)
MipiCsiCameraStreamStopTrigger = mipi_csi_camera_ns.class_(
    "MipiCsiCameraStreamStopTrigger", automation.Trigger.template()
)
MipiCsiCameraImageData = mipi_csi_camera_ns.struct("CameraImageData")

MipiCsiSensorModel = mipi_csi_camera_ns.enum("MipiCsiSensorModel")
SENSOR_MODELS = {
    "SC2336": MipiCsiSensorModel.MIPI_CSI_SENSOR_SC2336,
    "OV5647": MipiCsiSensorModel.MIPI_CSI_SENSOR_OV5647,
    "OV02C10": MipiCsiSensorModel.MIPI_CSI_SENSOR_OV02C10,
}

MipiCsiPixelFormat = mipi_csi_camera_ns.enum("MipiCsiPixelFormat")
PIXEL_FORMATS = {
    "RAW8": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_RAW8,
    "RAW10": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_RAW10,
    "GRAYSCALE": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_GRAYSCALE,
    "RGB565": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_RGB565,
    "RGB888": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_RGB888,
    "YUV422": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_YUV422,
    "YUV420": MipiCsiPixelFormat.MIPI_CSI_PIXEL_FORMAT_YUV420,
}

# Every mode below matches a real register table shipped in the vendored
# esp_cam_sensor drivers for SC2336 / OV5647; keeping this list in sync means
# invalid resolution/format/framerate combinations are rejected while
# generating the config, instead of failing at runtime on the device.
SUPPORTED_MODES = {
    "SC2336": {
        (800, 800, "RAW8", 30),
        (1024, 600, "RAW8", 30),
        (1280, 720, "RAW8", 30),
        (1920, 1080, "RAW8", 30),
        (640, 480, "RAW10", 50),
        (800, 800, "RAW10", 30),
        (1280, 720, "RAW10", 25),
        (1280, 720, "RAW10", 30),
        (1280, 720, "RAW10", 50),
        (1280, 720, "RAW10", 60),
        (1920, 1080, "RAW10", 15),
        (1920, 1080, "RAW10", 25),
        (1920, 1080, "RAW10", 30),
    },
    "OV5647": {
        (800, 1280, "RAW8", 50),
        (800, 640, "RAW8", 50),
        (800, 800, "RAW8", 50),
        (1920, 1080, "RAW10", 30),
        (1280, 960, "RAW10", 45),
    },
    "OV02C10": {
        (1288, 728, "RAW10", 30),
        (1920, 1080, "RAW10", 30),
    },
}
# OV02C10's register tables are also lane-count-specific (unlike SC2336/OV5647,
# where data_lanes is purely a wiring choice independent of the capture mode):
# 1288x728 only has a 1-lane table, 1920x1080 has both a 1-lane and 2-lane one.
OV02C10_LANE_MODES = {
    (1288, 728): {1},
    (1920, 1080): {1, 2},
}
# The ISP can convert any RAW mode into these processed output formats without
# needing a matching entry of its own in SUPPORTED_MODES.
ISP_OUTPUT_FORMATS = {"GRAYSCALE", "RGB565", "RGB888", "YUV422", "YUV420"}

CONF_SENSOR = "sensor"
CONF_FRAMERATE = "framerate"
CONF_SATURATION = "saturation"
CONF_PIXEL_FORMAT = "pixel_format"
CONF_DATA_LANES = "data_lanes"
CONF_POWER_DOWN_PIN = "power_down_pin"
CONF_HORIZONTAL_MIRROR = "horizontal_mirror"
CONF_VERTICAL_FLIP = "vertical_flip"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_FRAME_BUFFER_COUNT = "frame_buffer_count"
CONF_XCLK_FREQUENCY = "xclk_frequency"
CONF_INIT_LDO = "init_ldo"

CONF_ON_STREAM_START = "on_stream_start"
CONF_ON_STREAM_STOP = "on_stream_stop"
CONF_ON_IMAGE = "on_image"

camera_range_param = cv.int_range(min=-2, max=2)


def _validate_resolution(value: Any) -> tuple[int, int]:
    parts = cv.string(value).lower().replace(" ", "").split("x")
    if len(parts) != 2:
        raise cv.Invalid("resolution must be in the form WIDTHxHEIGHT, e.g. 1280x720")
    try:
        width, height = int(parts[0]), int(parts[1])
    except ValueError as err:
        raise cv.Invalid("resolution must be in the form WIDTHxHEIGHT, e.g. 1280x720") from err
    return width, height


def _validate_mode(config: ConfigType) -> ConfigType:
    sensor = config[CONF_SENSOR]
    width, height = config[CONF_RESOLUTION]
    fmt = config[CONF_PIXEL_FORMAT]
    framerate = config[CONF_FRAMERATE]

    modes = SUPPORTED_MODES[sensor]

    if fmt not in ISP_OUTPUT_FORMATS:
        # Raw sensor output requested directly (RAW8/RAW10): must be an exact
        # match to a real capture mode.
        if (width, height, fmt, framerate) not in modes:
            raise cv.Invalid(
                f"{sensor} has no {width}x{height} {fmt} mode at {framerate} fps. "
                f"Supported modes: {sorted(modes)}"
            )
    else:
        # Processed (ISP) output: only the resolution/framerate need to match
        # one of the sensor's native raw capture modes; the ISP performs the
        # RAW -> requested format conversion.
        if not any(
            (width, height, raw_mode_fmt, framerate) in modes
            for raw_mode_fmt in ("RAW8", "RAW10")
        ):
            raise cv.Invalid(
                f"{sensor} has no {width}x{height} capture mode at {framerate} fps to "
                f"derive {fmt} from. Supported modes: {sorted(modes)}"
            )

    if sensor == "OV02C10":
        allowed_lanes = OV02C10_LANE_MODES.get((width, height), set())
        data_lanes = config[CONF_DATA_LANES]
        if data_lanes not in allowed_lanes:
            raise cv.Invalid(
                f"OV02C10 has no {width}x{height} register table for {data_lanes} data "
                f"lane(s). Valid lane counts for this resolution: {sorted(allowed_lanes)}"
            )

    return config


def validate_jpeg_quality(config: ConfigType) -> ConfigType:
    quality = config.get(CONF_JPEG_QUALITY)
    if quality != 0 and (quality < 6 or quality > 63):
        raise cv.Invalid(f"jpeg_quality must be 0 (disabled) or between 6 and 63, got {quality}")
    if quality != 0 and config[CONF_PIXEL_FORMAT] not in ISP_OUTPUT_FORMATS:
        # The ESP32-P4's hardware JPEG encoder only accepts the ISP's processed output formats
        # (RGB565/RGB888/YUV422/YUV420/GRAYSCALE); the sensor's native RAW8/RAW10 Bayer data has
        # no direct JPEG source format to encode from.
        raise cv.Invalid(
            f"jpeg_quality requires an ISP output pixel_format ({sorted(ISP_OUTPUT_FORMATS)}); "
            f"got '{config[CONF_PIXEL_FORMAT]}', which can't be JPEG-encoded directly"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.ENTITY_BASE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(MipiCsiCamera),
            cv.Optional(CONF_SENSOR, default="SC2336"): cv.enum(
                SENSOR_MODELS, upper=True
            ),
            cv.Required(CONF_RESOLUTION): _validate_resolution,
            cv.Optional(CONF_FRAMERATE, default=30): cv.int_range(min=1, max=60),
            cv.Optional(CONF_PIXEL_FORMAT, default="RGB565"): cv.enum(
                PIXEL_FORMATS, upper=True
            ),
            cv.Optional(CONF_DATA_LANES, default=2): cv.int_range(min=1, max=2),
            cv.Optional(CONF_ROTATION, default=0): cv.one_of(0, 90, 180, 270, int=True),
            cv.Optional(CONF_XCLK_FREQUENCY, default="24MHz"): cv.All(
                cv.frequency, cv.float_range(min=6e6, max=27e6)
            ),
            # The MIPI-CSI clock/data lanes (fixed differential SerDes pins on
            # the ESP32-P4 package) are dedicated hardware and are not
            # GPIO-routable, so there's nothing to configure for them. SCCB
            # (the sensor's register/control bus) is an ordinary I2C bus,
            # though, so it's configured like any other I2C peripheral: via
            # `i2c_id`, reusing an already-declared `i2c:` bus rather than
            # opening a second, independent I2C driver on the same pins.
            cv.GenerateID(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
            cv.Optional(CONF_RESET_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_POWER_DOWN_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_HORIZONTAL_MIRROR, default=False): cv.boolean,
            cv.Optional(CONF_VERTICAL_FLIP, default=False): cv.boolean,
            cv.Optional(CONF_CONTRAST, default=0): camera_range_param,
            cv.Optional(CONF_BRIGHTNESS, default=0): camera_range_param,
            cv.Optional(CONF_SATURATION, default=0): camera_range_param,
            cv.Optional(CONF_JPEG_QUALITY, default=0): cv.Any(
                cv.one_of(0), cv.int_range(min=6, max=63)
            ),
            cv.Optional(CONF_FRAME_BUFFER_COUNT, default=3): cv.int_range(min=2, max=4),
            # On the ESP32-P4, the MIPI-CSI PHY and the MIPI-DSI display PHY share the same
            # internal LDO regulator channel (channel 3, 2.5V). If the display is already
            # configured via ESPHome's `esp_ldo:` component, that channel is already powered and
            # will reject a second exclusive acquire attempt. Set this to `false` in that case;
            # leave it at the default `true` if this is the only consumer of that LDO channel.
            cv.Optional(CONF_INIT_LDO, default=True): cv.boolean,
            cv.Optional(CONF_ON_STREAM_START): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        MipiCsiCameraStreamStartTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_STREAM_STOP): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        MipiCsiCameraStreamStopTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_IMAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        MipiCsiCameraImageTrigger
                    ),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_mode,
    validate_jpeg_quality,
)


def _final_validate(config: ConfigType) -> None:
    fconf = fv.full_config.get()
    esp32_conf = fconf.get("esp32", {})
    variant = str(esp32_conf.get("variant", "")).upper()
    if variant and variant != ESP32_VARIANT_ESP32P4:
        raise cv.Invalid(
            "mipi_csi_camera requires an ESP32-P4 (MIPI-CSI is not available on "
            f"the '{variant}' variant)"
        )

    # This component is built entirely on ESP-IDF-only APIs (esp_video/esp_cam_sensor managed
    # components, driver/ppa.h, driver/jpeg_encode.h, esp_cache.h, ...); none of that exists under
    # the Arduino framework. Without this check, picking Arduino here doesn't fail cleanly at
    # config time - it fails deep in the build with a wall of unrelated-looking compiler/linker
    # errors instead.
    framework_type = esp32_conf.get("framework", {}).get("type")
    if framework_type and framework_type != "esp-idf":
        raise cv.Invalid(
            f"mipi_csi_camera requires the 'esp-idf' framework, got '{framework_type}'"
        )

    # Rotation and (defensively) de-striding both allocate their working buffer with
    # MALLOC_CAP_SPIRAM (see rotate_frame_()/destride_frame_() in mipi_csi_camera.cpp) - without
    # PSRAM that allocation simply fails and the frame is sent unrotated, which is easy to miss
    # in a busy boot log. JPEG re-encoding has the same requirement for its own buffers.
    needs_psram = config[CONF_JPEG_QUALITY] or config[CONF_ROTATION]
    if needs_psram and psram_domain not in CORE.loaded_integrations:
        reason = "JPEG re-encoding" if config[CONF_JPEG_QUALITY] else "Rotation"
        raise cv.Invalid(
            f"{reason} requires the '{psram_domain}' component for buffer allocation"
        )


FINAL_VALIDATE_SCHEMA = _final_validate

SETTERS = {
    CONF_RESET_PIN: "set_reset_pin",
    CONF_POWER_DOWN_PIN: "set_power_down_pin",
    CONF_HORIZONTAL_MIRROR: "set_horizontal_mirror",
    CONF_VERTICAL_FLIP: "set_vertical_flip",
    CONF_CONTRAST: "set_contrast",
    CONF_BRIGHTNESS: "set_brightness",
    CONF_SATURATION: "set_saturation",
    CONF_JPEG_QUALITY: "set_jpeg_quality",
    CONF_FRAME_BUFFER_COUNT: "set_frame_buffer_count",
    CONF_INIT_LDO: "set_init_ldo",
}


async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)

    i2c_bus = await cg.get_variable(config[CONF_I2C_ID])
    cg.add(var.set_i2c_bus(i2c_bus))

    cg.add(var.set_sensor_model(config[CONF_SENSOR]))
    width, height = config[CONF_RESOLUTION]
    cg.add(var.set_resolution(width, height))
    cg.add(var.set_framerate(config[CONF_FRAMERATE]))
    cg.add(var.set_pixel_format(config[CONF_PIXEL_FORMAT]))
    cg.add(var.set_data_lanes(config[CONF_DATA_LANES]))
    cg.add(var.set_rotation(config[CONF_ROTATION]))
    cg.add(var.set_xclk_frequency(int(config[CONF_XCLK_FREQUENCY])))

    for key, setter in SETTERS.items():
        if key in config:
            cg.add(getattr(var, setter)(config[key]))

    for conf in config.get(CONF_ON_STREAM_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_STREAM_STOP, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_IMAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(MipiCsiCameraImageData, "image")], conf
        )

    # Espressif's V4L2-style camera stack: esp_video provides the MIPI-CSI
    # capture pipeline + ISP, esp_cam_sensor provides the SC2336/OV5647
    # register tables. Both are required regardless of which sensor is used.
    # esp_video >= 2.3.0 is required: 2.0.x-2.2.x hardcode the ISP processor
    # clock to 80MHz regardless of the actual clock source, which is exactly
    # at the OV02C10's ~80Mpx/s output rate - any DMA/PSRAM hiccup then makes
    # the ISP input FIFO overflow ("ISP: fifo overflow" spam, torn/streaked
    # frames, and eventually an interrupt-WDT crash from the error-interrupt
    # spam). 2.3.0 derives the ISP clock from clk_src (up to 240MHz) instead;
    # 2.4.1 additionally fixes ISP/MIPI-CSI driver compatibility on the
    # ESP-IDF 5.5.x line that ESPHome uses. See dev-docs/mipi-csi-camera-plan.md.
    add_idf_component(name="espressif/esp_video", ref="~2.4.1")
    add_idf_component(name="espressif/esp_cam_sensor", ref="~2.4.0")

    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE", True)
    # Belt-and-braces against the ISP error interrupt storm resetting the chip if a
    # FIFO overflow ever does happen again (option added in esp_video 2.4.0 for
    # exactly this purpose): lose the error log, keep the device alive.
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_DISABLE_ISP_ERROR_INTERRUPT", True)
    # NOTE: CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER (the esp_ipa-driven auto exposure/
    # gain/white-balance task) was tried here and reverted - see dev-docs/mipi-csi-camera-plan.md.
    # esp_ipa's AWB/AGC/color-correction algorithms are tuned via per-sensor JSON calibration data
    # (esp_cam_sensor/sensors/<name>/cfg/<name>_default.json for SC2336/OV5647/OV2710); OV02C10 has
    # no such calibration data (it isn't in the official esp_cam_sensor registry at all), so
    # enabling the controller made the image *worse* - a fully-saturated solid-color frame instead
    # of a dark/green-tinted one - because the 3A algorithms had no valid tuning to work from.
    add_idf_sdkconfig_option("CONFIG_CAMERA_SC2336", config[CONF_SENSOR] == "SC2336")
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647", config[CONF_SENSOR] == "OV5647")

    if config[CONF_SENSOR] == "OV02C10":
        # OV02C10 isn't in the espressif/esp_cam_sensor managed component
        # registry, so its driver is vendored directly in this component's
        # directory instead (files prefixed `ov02c10_*`; see
        # ov02c10_compat.h and README.md for details). Note: it can't live in
        # its own subfolder - ESPHome's external-component loader only picks
        # up source files placed directly inside the component's directory.
        # It has no real Kconfig entry to toggle via add_idf_sdkconfig_option,
        # so its "which capture mode is compiled in" macro is supplied
        # directly as a global build flag instead.
        width, height = config[CONF_RESOLUTION]
        lanes = config[CONF_DATA_LANES]
        format_macro = {
            (1288, 728, 1): "CONFIG_CAMERA_OV02C10_MIPI_RAW10_1288X728_30FPS",
            (1920, 1080, 1): "CONFIG_CAMERA_OV02C10_MIPI_RAW10_1920X1080_30FPS_1_LANE",
            (1920, 1080, 2): "CONFIG_CAMERA_OV02C10_MIPI_RAW10_1920X1080_30FPS_2_LANE",
        }[(width, height, lanes)]
        cg.add_build_flag(f"-D{format_macro}=1")
        cg.add_build_flag("-DCONFIG_CAMERA_OV02C10_MIPI_IF_FORMAT_INDEX_DEFAULT=0")
