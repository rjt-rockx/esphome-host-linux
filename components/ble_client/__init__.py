"""Shadow ble_client for the host platform (native BlueZ GATT client).

Step 1 scope: a connectable BLEClient with auto_connect + node registration.
Triggers/actions (on_connect/on_disconnect/ble_write/pairing) are added in the
later build steps. Drops the esp32 dependency.
"""

from __future__ import annotations

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32_ble_client, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import (
    CONF_CHARACTERISTIC_UUID,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_NAME,
    CONF_ON_CONNECT,
    CONF_ON_DISCONNECT,
    CONF_SERVICE_UUID,
    CONF_TRIGGER_ID,
    CONF_VALUE,
)

AUTO_LOAD = ["esp32_ble_client"]
CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["esp32_ble_tracker"]

ble_client_ns = cg.esphome_ns.namespace("ble_client")
BLEClient = ble_client_ns.class_("BLEClient", esp32_ble_client.BLEClientBase)
BLEClientNode = ble_client_ns.class_("BLEClientNode")

BLEClientConnectTrigger = ble_client_ns.class_("BLEClientConnectTrigger", automation.Trigger.template())
BLEClientDisconnectTrigger = ble_client_ns.class_("BLEClientDisconnectTrigger", automation.Trigger.template())
BLEWriteAction = ble_client_ns.class_("BLEClientWriteAction", automation.Action)
BLEConnectAction = ble_client_ns.class_("BLEClientConnectAction", automation.Action)
BLEDisconnectAction = ble_client_ns.class_("BLEClientDisconnectAction", automation.Action)

CONF_AUTO_CONNECT = "auto_connect"

MULTI_CONF = True

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BLEClient),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_NAME): cv.string,
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
            cv.Optional(CONF_ON_CONNECT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(BLEClientConnectTrigger)}
            ),
            cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(BLEClientDisconnectTrigger)}
            ),
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


def _uuid_setters(var, prefix, val):
    if len(val) == len(esp32_ble_tracker.bt_uuid16_format):
        cg.add(getattr(var, prefix + "16")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid32_format):
        cg.add(getattr(var, prefix + "32")(esp32_ble_tracker.as_hex(val)))
    elif len(val) == len(esp32_ble_tracker.bt_uuid128_format):
        cg.add(getattr(var, prefix + "128")(esp32_ble_tracker.as_reversed_hex_array(val)))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    await esp32_ble_tracker.register_client(var, config)

    for conf in config.get(CONF_ON_CONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_DISCONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


BLE_WRITE_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(BLEClient),
        cv.Required(CONF_SERVICE_UUID): esp32_ble_tracker.bt_uuid,
        cv.Required(CONF_CHARACTERISTIC_UUID): esp32_ble_tracker.bt_uuid,
        cv.Required(CONF_VALUE): cv.templatable(cv.ensure_list(cv.hex_uint8_t)),
    }
)


@automation.register_action("ble_client.ble_write", BLEWriteAction, BLE_WRITE_ACTION_SCHEMA)
async def ble_write_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    _uuid_setters(var, "set_service_uuid", config[CONF_SERVICE_UUID])
    _uuid_setters(var, "set_char_uuid", config[CONF_CHARACTERISTIC_UUID])
    value = config[CONF_VALUE]
    if cg.is_template(value):
        templ = await cg.templatable(value, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_value_template(templ))
    else:
        cg.add(var.set_value_simple(value))
    return var


BLE_CONNECT_ACTION_SCHEMA = cv.Schema({cv.Required(CONF_ID): cv.use_id(BLEClient)})


@automation.register_action("ble_client.connect", BLEConnectAction, BLE_CONNECT_ACTION_SCHEMA)
async def ble_connect_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action("ble_client.disconnect", BLEDisconnectAction, BLE_CONNECT_ACTION_SCHEMA)
async def ble_disconnect_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
