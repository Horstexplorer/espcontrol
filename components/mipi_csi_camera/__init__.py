import logging
from typing import Any

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option
from esphome.components.psram import DOMAIN as psram_domain
import esphome.config_validation as cv
from esphome.const import (
    CONF_BRIGHTNESS,
    CONF_CONTRAST,
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
DEPENDENCIES = ["esp32"]

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
}
# The ISP can convert any RAW mode into these processed output formats without
# needing a matching entry of its own in SUPPORTED_MODES.
ISP_OUTPUT_FORMATS = {"GRAYSCALE", "RGB565", "RGB888", "YUV422", "YUV420"}

CONF_SENSOR = "sensor"
CONF_FRAMERATE = "framerate"
CONF_SATURATION = "saturation"
CONF_PIXEL_FORMAT = "pixel_format"
CONF_DATA_LANES = "data_lanes"
CONF_SCCB_SDA_PIN = "sccb_sda_pin"
CONF_SCCB_SCL_PIN = "sccb_scl_pin"
CONF_SCCB_PORT = "sccb_port"
CONF_SCCB_FREQUENCY = "sccb_frequency"
CONF_POWER_DOWN_PIN = "power_down_pin"
CONF_HORIZONTAL_MIRROR = "horizontal_mirror"
CONF_VERTICAL_FLIP = "vertical_flip"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_FRAME_BUFFER_COUNT = "frame_buffer_count"
CONF_XCLK_FREQUENCY = "xclk_frequency"

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

    return config


def validate_jpeg_quality(config: ConfigType) -> ConfigType:
    quality = config.get(CONF_JPEG_QUALITY)
    if quality != 0 and (quality < 6 or quality > 63):
        raise cv.Invalid(f"jpeg_quality must be 0 (disabled) or between 6 and 63, got {quality}")
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
            # The SCCB (camera control) bus is a dedicated I2C-like link that
            # esp_video initializes and owns directly; it is intentionally
            # separate from ESPHome's `i2c:` component/bus.
            cv.Required(CONF_SCCB_SDA_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_SCCB_SCL_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_SCCB_PORT, default=1): cv.int_range(min=0, max=1),
            cv.Optional(CONF_SCCB_FREQUENCY, default="100kHz"): cv.All(
                cv.frequency, cv.float_range(min=1e3, max=400e3)
            ),
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
            cv.Optional(CONF_FRAME_BUFFER_COUNT, default=2): cv.int_range(min=2, max=3),
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

    if (
        config[CONF_PIXEL_FORMAT] != "RAW8"
        and config[CONF_JPEG_QUALITY]
        and psram_domain not in CORE.loaded_integrations
    ):
        raise cv.Invalid(
            f"JPEG re-encoding requires the '{psram_domain}' component for buffer allocation"
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
}


async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_CAMERA")
    var = cg.new_Pvariable(config[CONF_ID])
    await setup_entity(var, config, "camera")
    await cg.register_component(var, config)

    cg.add(
        var.set_sccb_bus(
            config[CONF_SCCB_PORT],
            config[CONF_SCCB_SDA_PIN],
            config[CONF_SCCB_SCL_PIN],
            int(config[CONF_SCCB_FREQUENCY]),
        )
    )

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
    add_idf_component(name="espressif/esp_video", ref="~2.0")
    add_idf_component(name="espressif/esp_cam_sensor", ref="~2.0")

    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE", True)
    add_idf_sdkconfig_option("CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE", True)
    add_idf_sdkconfig_option("CONFIG_CAMERA_SC2336", config[CONF_SENSOR] == "SC2336")
    add_idf_sdkconfig_option("CONFIG_CAMERA_OV5647", config[CONF_SENSOR] == "OV5647")
