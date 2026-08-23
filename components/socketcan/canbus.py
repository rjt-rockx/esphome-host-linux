"""SocketCAN host platform for ESPHome's canbus component.

Mirrors esp32_can/canbus.py: a directory under components/ whose canbus.py
implements the Canbus base class's three pure virtuals against a raw CAN_RAW
socket bound to a kernel CAN netdev.

    canbus:
      - platform: socketcan
        interface: can0
        on_frame:
          - can_id: 0x123
            then:
              - lambda: |-
                  ESP_LOGI("can", "rx %u bytes", x.size());

The interface must already be configured at the OS level, e.g.
`ip link set can0 up type can bitrate 500000`. ESPHome cannot set the bitrate
from userspace via the socket API; the base `bit_rate` option is accepted for
schema compatibility but is informational only (same pattern as i2c host
frequency).
"""

import sys

from esphome.components import canbus
from esphome.components.canbus import CanbusComponent
from esphome.components.host_patches import ensure_patch_script
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

CONF_INTERFACE = "interface"

socketcan_ns = cg.esphome_ns.namespace("socketcan")
SocketCAN = socketcan_ns.class_("SocketCAN", CanbusComponent)


def _validate_interface(value):
    value = cv.string_strict(value)
    # Linux netdev names are <= 15 chars, alphanumeric plus -._
    if len(value) > 15 or not all(c.isalnum() or c in "-._" for c in value):
        raise cv.Invalid(
            f"interface must be a Linux CAN network interface name (e.g. can0), got: {value!r}"
        )
    return value


def _validate_host_linux(config):
    if not CORE.is_host:
        raise cv.Invalid("socketcan is only available on the host platform")
    if not sys.platform.lower().startswith("linux"):
        raise cv.Invalid(
            f"socketcan requires a Linux host. Current platform: {sys.platform}"
        )
    return config


CONFIG_SCHEMA = cv.All(
    canbus.CANBUS_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(SocketCAN),
            cv.Required(CONF_INTERFACE): _validate_interface,
        }
    ),
    _validate_host_linux,
)


async def to_code(config):
    ensure_patch_script()
    var = cg.new_Pvariable(config[CONF_ID])
    await canbus.register_canbus(var, config)
    cg.add(var.set_interface(config[CONF_INTERFACE]))
