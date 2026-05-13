import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

linux_w1_ns = cg.esphome_ns.namespace("linux_w1")
LinuxW1Sensor = linux_w1_ns.class_("LinuxW1Sensor", sensor.Sensor, cg.PollingComponent)


def _validate_w1_address(value):
    """Accept either a full 1-Wire id ('28-3c01b556cab1') or just the suffix.

    The kernel exposes devices as /sys/bus/w1/devices/<family>-<id>/. Family
    0x28 is DS18B20; we don't restrict to that here since DS1822, DS18S20, and
    others are also valid. Pass the full directory name in unchanged.
    """
    if isinstance(value, str) and len(value) >= 3 and value[2] == "-":
        return value
    raise cv.Invalid(
        f"1-Wire address must be in the form 'FF-XXXXXXXXXXXX' (e.g. '28-3c01b556cab1'), got: {value!r}"
    )


CONFIG_SCHEMA = sensor.sensor_schema(
    LinuxW1Sensor,
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.Required(CONF_ADDRESS): _validate_w1_address,
    }
).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_address(config[CONF_ADDRESS]))
