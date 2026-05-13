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

CODEOWNERS = ["@rjt-rockx"]

esp32_ble_ns = cg.esphome_ns.namespace("esp32_ble")
ESPBTUUID = esp32_ble_ns.class_("ESPBTUUID")

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
