"""Shadow esp32_ble for host.

Provides just enough of upstream esp32_ble's Python surface that downstream
components (esp32_ble_tracker, ble_presence, ble_rssi, ...) can import this
module on host without dragging in ESP-IDF specific machinery.

This shadow does not register a YAML key. The C++ side only provides the
ESPBTUUID type via ble_uuid.h/cpp, which the host-side esp32_ble_tracker
includes.
"""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@rjt-rockx"]

esp32_ble_ns = cg.esphome_ns.namespace("esp32_ble")
ESPBTUUID = esp32_ble_ns.class_("ESPBTUUID")

# Host shadow of the ESP32BLE component. Owns the advertising state; the GATT
# server / beacon are Parented<ESP32BLE> and call advertising_* on it. The actual
# org.bluez advertising is done by an AdvertisingBackend the server registers.
ESP32BLE = esp32_ble_ns.class_("ESP32BLE", cg.Component)

CONF_BLE_ID = "ble_id"

# Referenced by esp32_ble_server's final-validate (max_clients sanity check). BlueZ
# multiplexes centrals itself; this is just the upstream-compatible soft default.
DEFAULT_MAX_CONNECTIONS = 3


def register_gatts_event_handler(parent_var, handler_var) -> None:
    """No-op on host: BlueZ delivers GATT-server events directly to BLEGattServer,
    not via an IDF gatts callback fan-out, so there is nothing to register."""


def register_ble_status_event_handler(parent_var, handler_var) -> None:
    """No-op on host (see register_gatts_event_handler)."""


def register_bt_logger(*loggers) -> None:
    """No-op on host: BlueZ logging is independent of the IDF BT logger categories."""

bt_uuid16_format = "XXXX"
bt_uuid32_format = "XXXXXXXX"
bt_uuid128_format = "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"


def bt_uuid(value):
    in_value = cv.string_strict(value)
    value = in_value.upper()
    if len(value) == len(bt_uuid16_format):
        if not re.match("^[A-F0-9]{4,}$", value):
            raise cv.Invalid(
                f"Invalid hexadecimal value for 16 bit UUID format: '{in_value}'"
            )
        return value
    if len(value) == len(bt_uuid32_format):
        if not re.match("^[A-F0-9]{8,}$", value):
            raise cv.Invalid(
                f"Invalid hexadecimal value for 32 bit UUID format: '{in_value}'"
            )
        return value
    if len(value) == len(bt_uuid128_format):
        if not re.match(
            "^[A-F0-9]{8,}-[A-F0-9]{4,}-[A-F0-9]{4,}-[A-F0-9]{4,}-[A-F0-9]{12,}$",
            value,
        ):
            raise cv.Invalid(
                f"Invalid hexadecimal value for 128 UUID format: '{in_value}'"
            )
        return value
    raise cv.Invalid(
        f"Bluetooth UUID must be in 16 bit '{bt_uuid16_format}', 32 bit '{bt_uuid32_format}', or 128 bit '{bt_uuid128_format}' format"
    )


# A minimal, single-instance config so the GATT server / beacon (which AUTO_LOAD
# esp32_ble and use_id(ESP32BLE)) get an ESP32BLE var created. All advertising
# tuning is host-irrelevant here; BlueZ controls the radio.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ESP32BLE),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_ESP32_BLE")
    cg.add_define("USE_ESP32_BLE_UUID")
    if CORE.is_host:
        cg.add_build_flag("-pthread")
        cg.add_build_flag("-lsystemd")
