from pathlib import Path

import esphome.codegen as cg
from esphome.components import i2c as _upstream_i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SCAN
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.helpers import copy_file_if_changed

CODEOWNERS = ["@rjt"]
DEPENDENCIES = ["host"]
AUTO_LOAD = ["i2c"]
MULTI_CONF = True

CONF_DEVICE = "device"
DEFAULT_DEVICE = "/dev/i2c-1"

linux_i2c_ns = cg.esphome_ns.namespace("linux_i2c")
LinuxI2CBus = linux_i2c_ns.class_(
    "LinuxI2CBus",
    cg.esphome_ns.namespace("i2c").class_("I2CBus"),
    cg.Component,
)

# Teach the upstream i2c component about host. Three things have to change:
#   1. `_bus_declare_type` raises NotImplementedError on host.
#   2. The top-level CONFIG_SCHEMA gates on `cv.only_on([ESP32, ESP8266, ...])`.
#   3. `to_code` constructs an i2c bus from fields (CONF_ID, CONF_SDA, ...) that
#      we don't ask the user to provide; on host the bus lives under `linux_i2c:`
#      instead.
# Patches are guarded by CORE.is_host inside each wrapper so the upstream
# behavior on other platforms is unaffected.
_PATCHED_FLAG = "_linux_i2c_patched"
if not getattr(_upstream_i2c, _PATCHED_FLAG, False):
    _orig_bus_declare = _upstream_i2c._bus_declare_type

    def _bus_declare_type_host_aware(value):
        if CORE.is_host:
            return cv.declare_id(LinuxI2CBus)(value)
        return _orig_bus_declare(value)

    _upstream_i2c._bus_declare_type = _bus_declare_type_host_aware

    _orig_config_schema = _upstream_i2c.CONFIG_SCHEMA

    def _config_schema_host_aware(value):
        if CORE.is_host:
            # On host the upstream schema's only_on() check rejects the platform
            # and the CONF_SDA/CONF_SCL defaults reference pin number 0. The
            # AUTO_LOAD entry is just an empty dict (AutoLoad sentinel); pass it
            # through so MULTI_CONF iteration sees a dict.
            return value if isinstance(value, dict) else {}
        return _orig_config_schema(value)

    _upstream_i2c.CONFIG_SCHEMA = _config_schema_host_aware

    _orig_to_code = _upstream_i2c.to_code

    @coroutine_with_priority(CoroPriority.BUS)
    async def _to_code_host_aware(config):
        if CORE.is_host:
            cg.add_define("USE_I2C")
            cg.add_global(cg.esphome_ns.namespace("i2c").using)
            return
        await _orig_to_code(config)

    _upstream_i2c.to_code = _to_code_host_aware
    setattr(_upstream_i2c, _PATCHED_FLAG, True)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LinuxI2CBus),
        cv.Optional(CONF_DEVICE, default=DEFAULT_DEVICE): cv.string,
        cv.Optional(CONF_SCAN, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _ensure_host_patch_script():
    """Register the shared host-patch pre-script. Fixes upstream sources that
    misbehave on host (e.g. ssieb seesaw's missing inter-transaction delay)."""
    script_src = (
        Path(__file__).parent.parent / "web_server_base" / "patch_web_server.py.script"
    )
    if not script_src.exists():
        return
    script_dst = CORE.relative_build_path("patch_web_server.py")
    copy_file_if_changed(script_src, script_dst)
    existing = CORE.platformio_options.get("extra_scripts", []) or []
    if "pre:patch_web_server.py" in existing:
        return
    CORE.add_platformio_option("extra_scripts", ["pre:patch_web_server.py"])


@coroutine_with_priority(CoroPriority.BUS)
async def to_code(config):
    cg.add_define("USE_I2C")
    cg.add_global(cg.esphome_ns.namespace("i2c").using)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_device(config[CONF_DEVICE]))
    cg.add(var.set_scan(config[CONF_SCAN]))
    if CORE.is_host:
        _ensure_host_patch_script()
