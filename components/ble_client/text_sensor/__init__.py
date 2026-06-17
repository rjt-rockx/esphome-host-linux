"""Shadow ble_client/text_sensor — read/notify a characteristic as text (hex)."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker, text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_CHARACTERISTIC_UUID, CONF_NOTIFY, CONF_SERVICE_UUID

from .. import BLE_CLIENT_SCHEMA, ble_client_ns, register_ble_node

DEPENDENCIES = ["ble_client"]

CONF_DESCRIPTOR_UUID = "descriptor_uuid"

BLETextSensor = ble_client_ns.class_(
    "BLETextSensor", text_sensor.TextSensor, cg.PollingComponent, ble_client.BLEClientNode
)


def _set_uuid(prefix, var, val):
    if len(val) == len(esp32_ble_tracker.bt_uuid16_format):
        cg.add(getattr(var, prefix + "16")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid32_format):
        cg.add(getattr(var, prefix + "32")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid128_format):
        cg.add(getattr(var, prefix + "128")(esp32_ble_tracker.as_reversed_hex_array(val)))


CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(BLETextSensor)
    .extend(cv.polling_component_schema("60s"))
    .extend(BLE_CLIENT_SCHEMA)
    .extend(
        {
            cv.Required(CONF_SERVICE_UUID): esp32_ble_tracker.bt_uuid,
            cv.Required(CONF_CHARACTERISTIC_UUID): esp32_ble_tracker.bt_uuid,
            cv.Optional(CONF_DESCRIPTOR_UUID): esp32_ble_tracker.bt_uuid,
            cv.Optional(CONF_NOTIFY, default=False): cv.boolean,
        }
    )
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await register_ble_node(var, config)
    _set_uuid("set_service_uuid", var, config[CONF_SERVICE_UUID])
    _set_uuid("set_char_uuid", var, config[CONF_CHARACTERISTIC_UUID])
    if CONF_DESCRIPTOR_UUID in config:
        _set_uuid("set_descr_uuid", var, config[CONF_DESCRIPTOR_UUID])
    cg.add(var.set_enable_notify(config[CONF_NOTIFY]))
