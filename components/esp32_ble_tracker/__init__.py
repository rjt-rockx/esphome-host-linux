"""Shadow esp32_ble_tracker for host.

Replaces upstream esp32_ble_tracker with a Linux/HCI raw socket backed
implementation so that stock components like ble_presence and ble_rssi
compile and operate on a Raspberry Pi without any ESP-IDF dependencies.

YAML surface is intentionally trimmed: the upstream component exposes scan
parameters, automation triggers, etc. We accept the same keys for YAML
compatibility but only honor `scan_parameters.{duration,interval,window,
active,continuous}` on host. Triggers are accepted but no-op for now.
"""

from __future__ import annotations

from pathlib import Path

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32_ble
from esphome.components.esp32_ble import (
    bt_uuid,
    bt_uuid16_format,
    bt_uuid32_format,
    bt_uuid128_format,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTIVE,
    CONF_CONTINUOUS,
    CONF_DURATION,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MAC_ADDRESS,
    CONF_MANUFACTURER_ID,
    CONF_ON_BLE_ADVERTISE,
    CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE,
    CONF_ON_BLE_SERVICE_DATA_ADVERTISE,
    CONF_SERVICE_UUID,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = []
AUTO_LOAD = ["esp32_ble"]

CONF_ESP32_BLE_ID = "esp32_ble_id"
CONF_SCAN_PARAMETERS = "scan_parameters"
CONF_WINDOW = "window"
CONF_ON_SCAN_END = "on_scan_end"
CONF_HCI_DEVICE = "hci_device"

esp32_ble_tracker_ns = cg.esphome_ns.namespace("esp32_ble_tracker")
ESP32BLETracker = esp32_ble_tracker_ns.class_("ESP32BLETracker", cg.Component)
ESPBTDeviceListener = esp32_ble_tracker_ns.class_("ESPBTDeviceListener")
ESPBTDevice = esp32_ble_tracker_ns.class_("ESPBTDevice")
ESPBTDeviceConstRef = ESPBTDevice.operator("ref").operator("const")


def as_hex(value):
    return cg.RawExpression(f"0x{value}ULL")


def as_hex_array(value):
    value = value.replace("-", "")
    cpp_array = [
        f"0x{part}" for part in [value[i : i + 2] for i in range(0, len(value), 2)]
    ]
    return cg.RawExpression(f"(uint8_t*)(const uint8_t[16]){{{','.join(cpp_array)}}}")


def as_reversed_hex_array(value):
    value = value.replace("-", "")
    cpp_array = [
        f"0x{part}" for part in [value[i : i + 2] for i in range(0, len(value), 2)]
    ]
    return cg.RawExpression(
        f"(uint8_t*)(const uint8_t[16]){{{','.join(reversed(cpp_array))}}}"
    )


def _validate_scan_parameters(config):
    duration = config[CONF_DURATION]
    interval = config[CONF_INTERVAL]
    window = config[CONF_WINDOW]
    if window > interval:
        raise cv.Invalid(
            f"Scan window ({window}) needs to be smaller than scan interval ({interval})"
        )
    if interval.total_milliseconds * 3 > duration.total_milliseconds:
        raise cv.Invalid(
            "Scan duration needs to be at least three times the scan interval to "
            "cover all BLE channels."
        )
    return config


# Trigger placeholders so upstream-style YAML doesn't error out. On host we
# accept the schema but the triggers are unused (no advertisement automation
# is emitted yet).
ESPBTAdvertiseTrigger = esp32_ble_tracker_ns.class_(
    "ESPBTAdvertiseTrigger", automation.Trigger.template(ESPBTDeviceConstRef)
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ESP32BLETracker),
        cv.Optional(CONF_HCI_DEVICE, default="hci0"): cv.string,
        cv.Optional(CONF_SCAN_PARAMETERS, default={}): cv.All(
            cv.Schema(
                {
                    cv.Optional(
                        CONF_DURATION, default="5min"
                    ): cv.positive_time_period_seconds,
                    cv.Optional(
                        CONF_INTERVAL, default="320ms"
                    ): cv.positive_time_period_milliseconds,
                    cv.Optional(
                        CONF_WINDOW, default="30ms"
                    ): cv.positive_time_period_milliseconds,
                    cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
                    cv.Optional(CONF_CONTINUOUS, default=True): cv.boolean,
                }
            ),
            _validate_scan_parameters,
        ),
        cv.Optional(CONF_ON_BLE_ADVERTISE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ESPBTAdvertiseTrigger),
                cv.Optional(CONF_MAC_ADDRESS): cv.ensure_list(cv.mac_address),
            }
        ),
        cv.Optional(CONF_ON_BLE_SERVICE_DATA_ADVERTISE): cv.ensure_list(cv.Schema({})),
        cv.Optional(CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE): cv.ensure_list(
            cv.Schema({})
        ),
        cv.Optional(CONF_ON_SCAN_END): cv.ensure_list(cv.Schema({})),
    }
).extend(cv.COMPONENT_SCHEMA)


ESP_BLE_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ESP32_BLE_ID): cv.use_id(ESP32BLETracker),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_hci_device(config[CONF_HCI_DEVICE]))

    params = config[CONF_SCAN_PARAMETERS]
    cg.add(var.set_scan_duration(int(params[CONF_DURATION].total_seconds)))
    cg.add(var.set_scan_interval_ms(int(params[CONF_INTERVAL].total_milliseconds)))
    cg.add(var.set_scan_window_ms(int(params[CONF_WINDOW].total_milliseconds)))
    cg.add(var.set_scan_active(params[CONF_ACTIVE]))
    cg.add(var.set_scan_continuous(params[CONF_CONTINUOUS]))

    cg.add_define("USE_ESP32_BLE_DEVICE")
    cg.add_define("USE_ESP32_BLE_UUID")
    cg.add_global(esp32_ble_tracker_ns.using)
    if CORE.is_host:
        cg.add_build_flag("-pthread")
        _ensure_ble_patch_script()


def _ensure_ble_patch_script():
    """Copy patch_web_server.py.script into the build dir and register it as a
    pre-script. The script also patches USE_ESP32 guards in ble_presence /
    ble_rssi so they accept USE_HOST. Idempotent against web_server_base also
    registering the same script."""
    script_src = (
        Path(__file__).parent.parent
        / "web_server_base"
        / "patch_web_server.py.script"
    )
    if not script_src.exists():
        return
    script_dst = CORE.relative_build_path("patch_web_server.py")
    copy_file_if_changed(script_src, script_dst)
    existing = CORE.platformio_options.get("extra_scripts", []) or []
    if "pre:patch_web_server.py" in existing:
        return
    CORE.add_platformio_option("extra_scripts", ["pre:patch_web_server.py"])


async def register_ble_device(var, config):
    paren = await cg.get_variable(config[CONF_ESP32_BLE_ID])
    cg.add(paren.register_listener(var))
    return var


async def register_raw_ble_device(var, config):
    paren = await cg.get_variable(config[CONF_ESP32_BLE_ID])
    cg.add(paren.register_listener(var))
    return var
