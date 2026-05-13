"""Host system-clock time platform.

Exposes the Linux wall clock (kept in sync by systemd-timesyncd / chrony /
ntpd on the OS side) as an ESPHome time:platform. No NTP work happens in
ESPHome itself: we just trust the kernel's ::time(nullptr).
"""

import esphome.codegen as cg
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = []

linux_time_ns = cg.esphome_ns.namespace("linux_time")
LinuxTime = linux_time_ns.class_("LinuxTime", time_.RealTimeClock)

CONFIG_SCHEMA = time_.TIME_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(LinuxTime),
    }
).extend(cv.polling_component_schema("never"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await time_.register_time(var, config)
