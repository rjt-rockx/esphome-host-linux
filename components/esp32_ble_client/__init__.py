"""esp32_ble_client for the host platform.

Native BlueZ GATT client: BLEClientBase implements connect/discover/read/write/
notify directly on org.bluez via BLEGattHost. Links libsystemd (sd-bus) +
pthread on host.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import esp32_ble_tracker
from esphome.core import CORE

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = ["esp32_ble"]

esp32_ble_client_ns = cg.esphome_ns.namespace("esp32_ble_client")
BLEClientBase = esp32_ble_client_ns.class_("BLEClientBase", esp32_ble_tracker.ESPBTClient, cg.Component)


async def to_code(config):
    cg.add_define("USE_ESP32_BLE_CLIENT")
    cg.add_define("USE_ESP32_BLE_DEVICE")
    if CORE.is_host:
        cg.add_build_flag("-pthread")
        cg.add_build_flag("-lsystemd")
