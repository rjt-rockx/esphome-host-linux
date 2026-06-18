"""Host (Linux/BlueZ) shadow of esp32_ble_beacon.

Advertises an Apple-iBeacon via org.bluez LEAdvertisement1 (Type="broadcast",
ManufacturerData company 0x004C) on the shared esp32_ble worker, instead of the
ESP-IDF esp_ble_gap_config_adv_data_raw path. Codegen mirrors upstream's setters
so a stock ``esp32_ble_beacon:`` block compiles unchanged on host. Host transforms:
DEPENDENCIES drops "esp32"; no add_idf_sdkconfig_option / gap-handler registration
(BlueZ owns the radio). ``min_interval`` / ``max_interval`` / ``tx_power`` are
accepted for config parity but are adapter-global on Linux, so they are
informational (logged in dump_config) — the kernel/BlueZ control them.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble
from esphome.components.esp32_ble import CONF_BLE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TX_POWER, CONF_TYPE, CONF_UUID
from esphome.core import TimePeriod

AUTO_LOAD = ["esp32_ble"]
DEPENDENCIES = []
CODEOWNERS = ["@rjt-rockx"]

esp32_ble_beacon_ns = cg.esphome_ns.namespace("esp32_ble_beacon")
ESP32BLEBeacon = esp32_ble_beacon_ns.class_("ESP32BLEBeacon", cg.Component)
CONF_MAJOR = "major"
CONF_MINOR = "minor"
CONF_MIN_INTERVAL = "min_interval"
CONF_MAX_INTERVAL = "max_interval"
CONF_MEASURED_POWER = "measured_power"


def validate_config(config):
    if config[CONF_MIN_INTERVAL] > config.get(CONF_MAX_INTERVAL):
        raise cv.Invalid("min_interval must be <= max_interval")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ESP32BLEBeacon),
            cv.GenerateID(CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
            cv.Required(CONF_TYPE): cv.one_of("IBEACON", upper=True),
            cv.Required(CONF_UUID): cv.uuid,
            cv.Optional(CONF_MAJOR, default=10167): cv.uint16_t,
            cv.Optional(CONF_MINOR, default=61958): cv.uint16_t,
            cv.Optional(CONF_MIN_INTERVAL, default="100ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=TimePeriod(milliseconds=20), max=TimePeriod(milliseconds=10240)
                ),
            ),
            cv.Optional(CONF_MAX_INTERVAL, default="100ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=TimePeriod(milliseconds=20), max=TimePeriod(milliseconds=10240)
                ),
            ),
            cv.Optional(CONF_MEASURED_POWER, default=-59): cv.int_range(
                min=-128, max=0
            ),
            # Accepted for parity; BlueZ controls adapter TX power on Linux.
            cv.Optional(CONF_TX_POWER, default="3dBm"): cv.decibel,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    validate_config,
)


async def to_code(config):
    cg.add_define("USE_ESP32_BLE_UUID")

    uuid = config[CONF_UUID].hex
    uuid_arr = [
        cg.RawExpression(f"0x{uuid[i : i + 2]}") for i in range(0, len(uuid), 2)
    ]
    var = cg.new_Pvariable(config[CONF_ID], uuid_arr)

    await cg.register_component(var, config)
    cg.add(var.set_major(config[CONF_MAJOR]))
    cg.add(var.set_minor(config[CONF_MINOR]))
    cg.add(var.set_min_interval(config[CONF_MIN_INTERVAL]))
    cg.add(var.set_max_interval(config[CONF_MAX_INTERVAL]))
    cg.add(var.set_measured_power(config[CONF_MEASURED_POWER]))
    cg.add(var.set_tx_power(int(config[CONF_TX_POWER])))

    cg.add_define("USE_ESP32_BLE_ADVERTISING")
