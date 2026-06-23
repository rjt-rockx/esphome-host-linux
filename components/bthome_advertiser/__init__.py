"""Host (Linux/BlueZ) BTHome v2 advertiser.

Broadcasts a BTHome v2 Service Data AD (UUID 0xFCD2) via its own org.bluez
LEAdvertisement1 on the shared esp32_ble worker: the Linux box publishes its own
readings as BTHome for Home Assistant / other receivers.

Measurements are static, built once at setup. Live sensor binding and encryption
(bindkey/AES-CCM) are not supported.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE, CONF_VALUE

AUTO_LOAD = ["esp32_ble"]
DEPENDENCIES = []
CODEOWNERS = ["@rjt-rockx"]

bthome_advertiser_ns = cg.esphome_ns.namespace("bthome_advertiser")
BTHomeAdvertiser = bthome_advertiser_ns.class_("BTHomeAdvertiser", cg.Component)

CONF_MEASUREMENTS = "measurements"

# Supported BTHome v2 measurement type name -> object id. Sizes/scales/signedness
# live in the C++ encoder (bthome_encoder.cpp).
BTHOME_OBJECTS = {
    "packet_id": 0x00,
    "battery": 0x01,
    "temperature": 0x02,
    "humidity": 0x03,
    "pressure": 0x04,
    "illuminance": 0x05,
    "voltage": 0x0C,
    "co2": 0x12,
    "tvoc": 0x13,
    "moisture": 0x14,
    # binary sensors (0/1)
    "power": 0x10,
    "motion": 0x21,
    "occupancy": 0x23,
}

# cv.boolean first so on/off/true/false (and YAML bools) become booleans;
# anything else falls through to a number.
MEASUREMENT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TYPE): cv.one_of(*BTHOME_OBJECTS, lower=True),
        cv.Required(CONF_VALUE): cv.Any(cv.boolean, cv.float_),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BTHomeAdvertiser),
        cv.Required(CONF_MEASUREMENTS): cv.All(
            cv.ensure_list(MEASUREMENT_SCHEMA), cv.Length(min=1)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_ESP32_BLE_UUID")
    cg.add_define("USE_ESP32_BLE_ADVERTISING")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for m in config[CONF_MEASUREMENTS]:
        object_id = BTHOME_OBJECTS[m[CONF_TYPE]]
        value = m[CONF_VALUE]
        if isinstance(value, bool):
            value = 1.0 if value else 0.0
        cg.add(var.add_measurement(object_id, float(value)))
