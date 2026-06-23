"""ble_client/switch — enable/disable the BLE client connection."""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import ble_client, switch
import esphome.config_validation as cv
from esphome.const import ICON_BLUETOOTH

from .. import BLE_CLIENT_SCHEMA, ble_client_ns, register_ble_node

DEPENDENCIES = ["ble_client"]

BLEClientSwitch = ble_client_ns.class_("BLEClientSwitch", switch.Switch, cg.Component, ble_client.BLEClientNode)

CONFIG_SCHEMA = (
    switch.switch_schema(BLEClientSwitch, icon=ICON_BLUETOOTH, block_inverted=True)
    .extend(BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await register_ble_node(var, config)
