"""Host esp32_ble_tracker component.

A Linux BLE-scanning implementation (BlueZ D-Bus or raw HCI socket) that
implements the platform-neutral ble_device_base BLEHub contract, so the stock
BLE consumers (ble_presence, ble_rssi, ble_scanner, bthome_mithermometer,
xiaomi_*, ...) bind to it through cv.use_id(BLEHub) with no host-specific code.

The component keeps the esp32_ble_tracker name deliberately: that is the name
core's BLEHub alias ladder (ble_device_base/ble_hub_impl.h) and the
missing-tracker registry key on, so both work unmodified against this shadow.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_device_base
from esphome.components.ble_device_base import automation as ble_automation
from esphome.components.host_patches import ensure_patch_script
from esphome.components.const import (
    CONF_ON_SCAN_END,
    CONF_SCAN_PARAMETERS,
    CONF_WINDOW,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTIVE,
    CONF_CONTINUOUS,
    CONF_DURATION,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MANUFACTURER_ID,
    CONF_ON_BLE_ADVERTISE,
    CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE,
    CONF_ON_BLE_SERVICE_DATA_ADVERTISE,
    CONF_SERVICE_UUID,
)
from esphome.core import CORE
from esphome.types import ConfigType

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = []
AUTO_LOAD = ["ble_device_base", "esp32_ble"]

ble_device_base.register_hub_provider("esp32_ble_tracker")

CONF_ESP32_BLE_ID = "esp32_ble_id"
CONF_HCI_DEVICE = "hci_device"
CONF_HCI_BACKEND = "hci_backend"

esp32_ble_tracker_ns = cg.esphome_ns.namespace("esp32_ble_tracker")
ESP32BLETracker = esp32_ble_tracker_ns.class_(
    "ESP32BLETracker", ble_device_base.BLEHub, cg.Component
)
ESPBTClient = esp32_ble_tracker_ns.class_("ESPBTClient")

# The advertisement types are the neutral ones now; re-exported so the repo's
# own GATT family keeps importing them from here.
ESPBTDeviceListener = ble_device_base.ESPBTDeviceListener
ESPBTDeviceConstRef = ble_automation.ESPBTDeviceConstRef

# UUID validation/codegen helpers live in ble_device_base; re-exported under the
# historical names the repo's ble_client platforms use.
bt_uuid = ble_device_base.bt_uuid
bt_uuid16_format = ble_device_base.BT_UUID16_FORMAT
bt_uuid32_format = ble_device_base.BT_UUID32_FORMAT
bt_uuid128_format = ble_device_base.BT_UUID128_FORMAT
as_hex = ble_device_base.as_hex
as_hex_array = ble_device_base.as_hex_array
as_reversed_hex_array = ble_device_base.as_reversed_hex_array

ESPBTAdvertiseTrigger = ble_automation.ESPBTAdvertiseTrigger
BLEServiceDataAdvertiseTrigger = ble_automation.BLEServiceDataAdvertiseTrigger
BLEManufacturerDataAdvertiseTrigger = ble_automation.BLEManufacturerDataAdvertiseTrigger
BLEEndOfScanTrigger = ble_automation.BLEEndOfScanTrigger

# Listeners registered through the repo's own helpers (GATT clients, the
# bluetooth_proxy) share the codegen-sized StaticVector with the neutral
# register_ble_device(), so they must claim a slot the same way.
_count_listener = cg.slot_counter(ble_device_base.LISTENER_COUNT_DEFINE)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ESP32BLETracker),
        cv.Optional(CONF_HCI_DEVICE, default="hci0"): cv.string,
        # Default backend is BlueZ D-Bus, which coexists with bluetoothd. Set
        # hci_backend: true for the raw-HCI scanner, which needs an adapter not
        # owned by bluetoothd but yields byte-exact advertisements.
        cv.Optional(CONF_HCI_BACKEND, default=False): cv.boolean,
        cv.Optional(
            CONF_SCAN_PARAMETERS, default={}
        ): ble_device_base.scan_parameters_schema("320ms"),
        cv.Optional(CONF_ON_BLE_ADVERTISE): ble_automation.advertise_trigger_schema(
            ESPBTAdvertiseTrigger
        ),
        cv.Optional(
            CONF_ON_BLE_SERVICE_DATA_ADVERTISE
        ): ble_automation.uuid_trigger_schema(
            BLEServiceDataAdvertiseTrigger,
            {cv.Required(CONF_SERVICE_UUID): ble_device_base.bt_uuid},
        ),
        cv.Optional(
            CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE
        ): ble_automation.uuid_trigger_schema(
            BLEManufacturerDataAdvertiseTrigger,
            {cv.Required(CONF_MANUFACTURER_ID): ble_device_base.bt_uuid},
        ),
        cv.Optional(CONF_ON_SCAN_END): ble_automation.scan_end_trigger_schema(
            BLEEndOfScanTrigger
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


# Binding schema for the repo's own GATT family (ble_client, bluetooth_proxy),
# which still keys on esp32_ble_id rather than the neutral ble_hub_id.
ESP_BLE_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ESP32_BLE_ID): cv.use_id(ESP32BLETracker),
    }
)


async def to_code(config: ConfigType) -> None:
    # Selects the BLEHub alias arm in ble_device_base/ble_hub_impl.h.
    cg.add_define("USE_ESP32_BLE_TRACKER")
    # Compiles the shared adv + scan-response merge: the raw-HCI backend sees
    # advertisement and scan response as separate reports.
    cg.add_define("USE_BLE_SCAN_RESPONSE_MERGER")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_hci_device(config[CONF_HCI_DEVICE]))
    cg.add(var.set_use_hci_backend(config[CONF_HCI_BACKEND]))

    scan = config[CONF_SCAN_PARAMETERS]
    cg.add(var.set_scan_duration(int(scan[CONF_DURATION].total_seconds)))
    cg.add(var.set_scan_interval_ms(int(scan[CONF_INTERVAL].total_milliseconds)))
    cg.add(var.set_scan_window_ms(int(scan[CONF_WINDOW].total_milliseconds)))
    cg.add(var.set_scan_active(scan[CONF_ACTIVE]))
    cg.add(var.set_scan_continuous(scan[CONF_CONTINUOUS]))

    for conf in config.get(CONF_ON_BLE_ADVERTISE, []):
        await ble_automation.advertise_trigger_to_code(conf, var)

    for trigger_key, uuid_key, setter_prefix in (
        (CONF_ON_BLE_SERVICE_DATA_ADVERTISE, CONF_SERVICE_UUID, "set_service_uuid"),
        (
            CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE,
            CONF_MANUFACTURER_ID,
            "set_manufacturer_uuid",
        ),
    ):
        for conf in config.get(trigger_key, []):
            await ble_automation.uuid_trigger_to_code(
                conf, var, uuid_key, setter_prefix
            )

    for conf in config.get(CONF_ON_SCAN_END, []):
        await ble_automation.scan_end_trigger_to_code(conf, var)

    cg.add_global(esp32_ble_tracker_ns.using)
    if CORE.is_host:
        cg.add_build_flag("-pthread")
        # libsystemd provides sd-bus for the D-Bus backend. (The raw-HCI path
        # needs no extra libs.)
        cg.add_build_flag("-lsystemd")
        ensure_patch_script()


async def register_ble_device(var, config):
    paren = await cg.get_variable(config[CONF_ESP32_BLE_ID])
    cg.add(paren.register_listener(var))
    _count_listener()
    return var


async def register_raw_ble_device(var, config):
    return await register_ble_device(var, config)


async def register_client(var, config):
    paren = await cg.get_variable(config[CONF_ESP32_BLE_ID])
    cg.add(paren.register_client(var))
    _count_listener()
    return var
