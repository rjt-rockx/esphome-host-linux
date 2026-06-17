"""Shadow ble_client for the host platform (native BlueZ GATT client).

Step 1 scope: a connectable BLEClient with auto_connect + node registration.
Triggers/actions (on_connect/on_disconnect/ble_write/pairing) are added in the
later build steps. Drops the esp32 dependency.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import esp32_ble_client, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MAC_ADDRESS, CONF_NAME

AUTO_LOAD = ["esp32_ble_client"]
CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["esp32_ble_tracker"]

ble_client_ns = cg.esphome_ns.namespace("ble_client")
BLEClient = ble_client_ns.class_("BLEClient", esp32_ble_client.BLEClientBase)
BLEClientNode = ble_client_ns.class_("BLEClientNode")

CONF_AUTO_CONNECT = "auto_connect"

MULTI_CONF = True

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLEClient),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_NAME): cv.string,
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


# Schema for sub-platform nodes (sensor/switch/etc.) to attach to a BLEClient.
CONF_BLE_CLIENT_ID = "ble_client_id"
BLE_CLIENT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BLE_CLIENT_ID): cv.use_id(BLEClient),
    }
)


async def register_ble_node(var, config):
    paren = await cg.get_variable(config[CONF_BLE_CLIENT_ID])
    cg.add(paren.register_ble_node(var))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    await esp32_ble_tracker.register_client(var, config)
