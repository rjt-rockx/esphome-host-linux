import logging

from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INPUT,
    CONF_INVERTED,
    CONF_MODE,
    CONF_NUMBER,
    CONF_OPEN_DRAIN,
    CONF_OUTPUT,
    CONF_PULLDOWN,
    CONF_PULLUP,
)

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

_LOGGER = logging.getLogger(__name__)

CONF_CHIP = "chip"
DEFAULT_CHIP = "/dev/gpiochip0"

linux_gpio_ns = cg.esphome_ns.namespace("linux_gpio")
LinuxGPIOPin = linux_gpio_ns.class_("LinuxGPIOPin", cg.InternalGPIOPin)


# Top-level config is currently a no-op marker so this module gets imported.
# Importing the module triggers the pin-schema overrides below; without it,
# the stock host stub schema (HostGPIOPin) wins.
CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_build_flag("-llgpio")
    cg.add_define("USE_LINUX_GPIO")


def _translate_pin(value):
    if isinstance(value, dict) or value is None:
        raise cv.Invalid(
            "This variable only supports pin numbers, not full pin schemas "
            "(with inverted and mode)."
        )
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if not isinstance(value, str):
        raise cv.Invalid(f"Invalid pin number: {value}")
    try:
        return int(value)
    except ValueError:
        pass
    if value.upper().startswith("GPIO"):
        return cv.int_(value[len("GPIO") :].strip())
    return value


def validate_gpio_pin(value):
    num = _translate_pin(value)
    return cv.int_range(min=0, max=63)(num)


LINUX_GPIO_PIN_SCHEMA = pins.gpio_base_schema(
    LinuxGPIOPin,
    validate_gpio_pin,
    modes=[CONF_INPUT, CONF_OUTPUT, CONF_OPEN_DRAIN, CONF_PULLUP, CONF_PULLDOWN],
).extend(
    {
        cv.Optional(CONF_CHIP, default=DEFAULT_CHIP): cv.string,
    }
)


async def linux_gpio_pin_to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_pin(config[CONF_NUMBER]))
    cg.add(var.set_chip_path(config[CONF_CHIP]))
    if config[CONF_INVERTED]:
        cg.add(var.set_inverted(True))
    cg.add(var.set_flags(pins.gpio_flags_expr(config[CONF_MODE])))
    return var


# Replace the stock host pin registration. ESPHome's pin schemas dispatch on
# CORE.target_platform ("host") via PIN_SCHEMA_REGISTRY. Overriding the "host"
# slot routes every bare pin config (e.g. pulse_counter, gpio.binary_sensor,
# gpio.switch, dallas, rotary_encoder) through LinuxGPIOPin instead of the
# broken HostGPIOPin stub.
pins.PIN_SCHEMA_REGISTRY["host"] = (
    linux_gpio_pin_to_code,
    LINUX_GPIO_PIN_SCHEMA,
    None,
)
