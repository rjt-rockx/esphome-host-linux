"""ble_client/output — write a bool to a GATT characteristic."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker, output
import esphome.config_validation as cv
from esphome.const import CONF_CHARACTERISTIC_UUID, CONF_ID, CONF_SERVICE_UUID

from .. import BLE_CLIENT_SCHEMA, ble_client_ns, register_ble_node

DEPENDENCIES = ["ble_client"]

CONF_REQUIRE_RESPONSE = "require_response"

BLEBinaryOutput = ble_client_ns.class_(
    "BLEBinaryOutput", output.BinaryOutput, ble_client.BLEClientNode, cg.Component
)


def _set_uuid(prefix, var, val):
    if len(val) == len(esp32_ble_tracker.bt_uuid16_format):
        cg.add(getattr(var, prefix + "16")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid32_format):
        cg.add(getattr(var, prefix + "32")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid128_format):
        cg.add(getattr(var, prefix + "128")(esp32_ble_tracker.as_reversed_hex_array(val)))


CONFIG_SCHEMA = cv.All(
    output.BINARY_OUTPUT_SCHEMA.extend(
        {
            cv.Required(CONF_ID): cv.declare_id(BLEBinaryOutput),
            cv.Required(CONF_SERVICE_UUID): esp32_ble_tracker.bt_uuid,
            cv.Required(CONF_CHARACTERISTIC_UUID): esp32_ble_tracker.bt_uuid,
            cv.Optional(CONF_REQUIRE_RESPONSE, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    await register_ble_node(var, config)
    _set_uuid("set_service_uuid", var, config[CONF_SERVICE_UUID])
    _set_uuid("set_char_uuid", var, config[CONF_CHARACTERISTIC_UUID])
    cg.add(var.set_require_response(config[CONF_REQUIRE_RESPONSE]))
