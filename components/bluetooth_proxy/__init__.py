"""bluetooth_proxy for the host platform (native BlueZ).

Host as a Home Assistant Bluetooth proxy: streams raw advertisements from the
BLE hub's callback and proxies active GATT connections over the ESPHome native
API.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import esp32_ble_client, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import CONF_ACTIVE, CONF_ID

AUTO_LOAD = ["esp32_ble_client", "esp32_ble_tracker"]
DEPENDENCIES = ["api"]
CODEOWNERS = ["@rjt-rockx"]

CONF_CONNECTIONS = "connections"
CONF_CONNECTION_SLOTS = "connection_slots"
MAX_CONNECTIONS = 8

bluetooth_proxy_ns = cg.esphome_ns.namespace("bluetooth_proxy")
BluetoothProxy = bluetooth_proxy_ns.class_("BluetoothProxy", cg.Component)
BluetoothConnection = bluetooth_proxy_ns.class_("BluetoothConnection", esp32_ble_client.BLEClientBase)

CONNECTION_SCHEMA = esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(BluetoothConnection),
    }
)


def _expand_connection_slots(config):
    """If active and no explicit `connections:`, materialize connection_slots
    auto-ID'd connection entries so each gets a valid declared id."""
    if config.get(CONF_ACTIVE) and CONF_CONNECTIONS not in config:
        config[CONF_CONNECTIONS] = [CONNECTION_SCHEMA({}) for _ in range(config[CONF_CONNECTION_SLOTS])]
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BluetoothProxy),
            cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
            cv.Optional(CONF_CONNECTION_SLOTS, default=3): cv.All(
                cv.positive_int, cv.Range(min=1, max=MAX_CONNECTIONS)
            ),
            cv.Optional(CONF_CONNECTIONS): cv.All(
                cv.ensure_list(CONNECTION_SCHEMA), cv.Length(min=1, max=MAX_CONNECTIONS)
            ),
        }
    ).extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    _expand_connection_slots,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_active(config[CONF_ACTIVE]))
    hub = await cg.get_variable(config[esp32_ble_tracker.CONF_ESP32_BLE_ID])
    cg.add(var.set_ble_hub(hub))

    connections = config.get(CONF_CONNECTIONS, [])
    # Sized exactly as upstream: the api component uses this for
    # BluetoothConnectionsFreeResponse.allocated, and 0 is a valid count for an
    # advertisement-only proxy.
    cg.add_define("BLUETOOTH_PROXY_MAX_CONNECTIONS", len(connections))
    if connections:
        # Gates the whole GATT message family in the api component; without it
        # api::BluetoothDeviceRequest and friends do not exist.
        cg.add_define("USE_BLUETOOTH_PROXY_CONNECTIONS")
    cg.add_define("BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE", 16)

    for connection_conf in connections:
        connection_var = cg.new_Pvariable(connection_conf[CONF_ID])
        await cg.register_component(connection_var, connection_conf)
        cg.add(var.register_connection(connection_var))
        await esp32_ble_tracker.register_client(connection_var, connection_conf)

    cg.add_define("USE_BLUETOOTH_PROXY")
