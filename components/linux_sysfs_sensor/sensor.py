"""linux_sysfs_sensor -- publishes a numeric sysfs attribute as an ESPHome sensor.

Reads one decimal integer from an explicit sysfs path on each poll and scales it.
Typical sources (path + scale):
  /sys/class/thermal/thermal_zone0/temp          scale: 0.001  (millidegrees C -> C)
  /sys/class/hwmon/hwmon0/temp1_input             scale: 0.001  (millidegrees C -> C)
  /sys/class/hwmon/hwmon0/in1_input               scale: 0.001  (millivolts -> V)
  /sys/bus/iio/devices/iio:device0/in_voltage0_raw scale: <per-device>

There is deliberately NO auto-discovery: sysfs indices (hwmonN, iio:deviceN) are
assigned by probe order and are not stable across boots. Supply the exact path
and scale. To pin a hwmon chip reliably, check its name first
(`cat /sys/class/hwmon/hwmon*/name`) or use a udev-created symlink.
"""

import sys

from esphome.components import sensor
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

CONF_PATH = "path"
CONF_SCALE = "scale"

linux_sysfs_sensor_ns = cg.esphome_ns.namespace("linux_sysfs_sensor")
LinuxSysfsSensor = linux_sysfs_sensor_ns.class_(
    "LinuxSysfsSensor", sensor.Sensor, cg.PollingComponent
)


def _validate_sysfs_path(value):
    value = cv.string_strict(value)
    if not value.startswith("/sys/"):
        raise cv.Invalid(
            "path must be an absolute sysfs path under /sys/ "
            f"(e.g. /sys/class/thermal/thermal_zone0/temp), got: {value!r}"
        )
    # These attributes hold text (a chip/label name), not the single decimal
    # integer this sensor parses -- catch the common mistake at config time.
    if value.endswith("/name") or value.endswith("_label"):
        raise cv.Invalid(
            f"{value!r} is a text attribute, not a numeric reading; "
            "point at an integer attribute such as *_input or temp"
        )
    return value


def _validate_host_linux(config):
    if not CORE.is_host:
        raise cv.Invalid("linux_sysfs_sensor is only available on the host platform")
    if not sys.platform.lower().startswith("linux"):
        raise cv.Invalid(
            "linux_sysfs_sensor requires a Linux host. "
            f"Current platform: {sys.platform}"
        )
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(LinuxSysfsSensor)
    .extend(
        {
            cv.Required(CONF_PATH): _validate_sysfs_path,
            cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        }
    )
    .extend(cv.polling_component_schema("60s")),
    _validate_host_linux,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_path(config[CONF_PATH]))
    if (scale := config.get(CONF_SCALE)) is not None:
        cg.add(var.set_scale(scale))
