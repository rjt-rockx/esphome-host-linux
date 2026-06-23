"""ble_client/sensor — `characteristic` (read/notify) and `rssi` sensor types."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_client, esp32_ble_tracker, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHARACTERISTIC_UUID,
    CONF_LAMBDA,
    CONF_NOTIFY,
    CONF_SERVICE_UUID,
    CONF_TYPE,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL_MILLIWATT,
)

from .. import BLE_CLIENT_SCHEMA, ble_client_ns, register_ble_node

DEPENDENCIES = ["ble_client"]

CONF_DESCRIPTOR_UUID = "descriptor_uuid"
TYPE_CHARACTERISTIC = "characteristic"
TYPE_RSSI = "rssi"

adv_data_t = cg.std_vector.template(cg.uint8)
adv_data_t_const_ref = adv_data_t.operator("ref").operator("const")

BLESensor = ble_client_ns.class_("BLESensor", sensor.Sensor, cg.PollingComponent, ble_client.BLEClientNode)
BLEClientRSSISensor = ble_client_ns.class_(
    "BLEClientRSSISensor", sensor.Sensor, cg.PollingComponent, ble_client.BLEClientNode
)


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
            TYPE_RSSI: sensor.sensor_schema(
                BLEClientRSSISensor,
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
            )
            .extend(cv.polling_component_schema("60s"))
            .extend(BLE_CLIENT_SCHEMA),
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

    if config[CONF_TYPE] == TYPE_RSSI:
        return  # RSSI sensor needs no characteristic UUIDs

    _set_uuid("set_service_uuid", var, config[CONF_SERVICE_UUID])
    _set_uuid("set_char_uuid", var, config[CONF_CHARACTERISTIC_UUID])
    if CONF_DESCRIPTOR_UUID in config:
        _set_uuid("set_descr_uuid", var, config[CONF_DESCRIPTOR_UUID])
    if CONF_NOTIFY in config:
        cg.add(var.set_enable_notify(config[CONF_NOTIFY]))
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(config[CONF_LAMBDA], [(adv_data_t_const_ref, "x")], return_type=cg.float_)
        cg.add(var.set_data_to_value(lambda_))
