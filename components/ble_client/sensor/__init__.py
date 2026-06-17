"""Shadow ble_client/sensor (characteristic read/notify). Native host port.

Handles the `characteristic` sensor type. The `rssi` type is added in the RSSI
build step. Ported to the native BLEClientNode hooks (no IDF event handler).
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker, sensor
import esphome.config_validation as cv
from esphome.const import CONF_CHARACTERISTIC_UUID, CONF_LAMBDA, CONF_NOTIFY, CONF_SERVICE_UUID, CONF_TYPE

from .. import BLE_CLIENT_SCHEMA, ble_client_ns, register_ble_node

DEPENDENCIES = ["ble_client"]

CONF_DESCRIPTOR_UUID = "descriptor_uuid"
TYPE_CHARACTERISTIC = "characteristic"

adv_data_t = cg.std_vector.template(cg.uint8)
adv_data_t_const_ref = adv_data_t.operator("ref").operator("const")

BLESensor = ble_client_ns.class_("BLESensor", sensor.Sensor, cg.PollingComponent, ble_client.BLEClientNode)


def _checktype(value):
    if CONF_TYPE not in value and CONF_SERVICE_UUID in value:
        raise cv.Invalid("Add `type: characteristic` to your ble_client sensor config.")
    return value


CONFIG_SCHEMA = cv.All(
    _checktype,
    cv.typed_schema(
        {
            TYPE_CHARACTERISTIC: sensor.sensor_schema(BLESensor, accuracy_decimals=0)
            .extend(cv.polling_component_schema("60s"))
            .extend(BLE_CLIENT_SCHEMA)
            .extend(
                {
                    cv.Required(CONF_SERVICE_UUID): esp32_ble_tracker.bt_uuid,
                    cv.Required(CONF_CHARACTERISTIC_UUID): esp32_ble_tracker.bt_uuid,
                    cv.Optional(CONF_DESCRIPTOR_UUID): esp32_ble_tracker.bt_uuid,
                    cv.Optional(CONF_LAMBDA): cv.returning_lambda,
                    cv.Optional(CONF_NOTIFY, default=False): cv.boolean,
                }
            ),
        },
        default_type=TYPE_CHARACTERISTIC,
        lower=True,
    ),
)


def _set_uuid(setter_prefix, var, uuid_val):
    """Emit the right set_*_uuid{16,32,128} call for a parsed bt_uuid value."""
    if len(uuid_val) == len(esp32_ble_tracker.bt_uuid16_format):
        cg.add(getattr(var, setter_prefix + "16")(esp32_ble_tracker.as_hex(uuid_val)))
    elif len(uuid_val) == len(esp32_ble_tracker.bt_uuid32_format):
        cg.add(getattr(var, setter_prefix + "32")(esp32_ble_tracker.as_hex(uuid_val)))
    elif len(uuid_val) == len(esp32_ble_tracker.bt_uuid128_format):
        uuid128 = esp32_ble_tracker.as_reversed_hex_array(uuid_val)
        cg.add(getattr(var, setter_prefix + "128")(uuid128))


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await register_ble_node(var, config)

    _set_uuid("set_service_uuid", var, config[CONF_SERVICE_UUID])
    _set_uuid("set_char_uuid", var, config[CONF_CHARACTERISTIC_UUID])
    if CONF_DESCRIPTOR_UUID in config:
        _set_uuid("set_descr_uuid", var, config[CONF_DESCRIPTOR_UUID])
    if CONF_NOTIFY in config:
        cg.add(var.set_enable_notify(config[CONF_NOTIFY]))
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(config[CONF_LAMBDA], [(adv_data_t_const_ref, "x")], return_type=cg.float_)
        cg.add(var.set_data_to_value(lambda_))
