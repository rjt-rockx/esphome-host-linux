import esphome.codegen as cg
from esphome.components import spi as _upstream_spi
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE, CoroPriority, coroutine_with_priority

CODEOWNERS = ["@rjt"]
DEPENDENCIES = ["host"]
AUTO_LOAD = ["spi", "linux_compat"]
MULTI_CONF = True

CONF_DEVICE = "device"
DEFAULT_DEVICE = "/dev/spidev0.0"

linux_spi_ns = cg.esphome_ns.namespace("linux_spi")
spi_ns = cg.esphome_ns.namespace("spi")
LinuxSPIComponent = linux_spi_ns.class_(
    "LinuxSPIComponent",
    spi_ns.class_("SPIComponent"),
    cg.Component,
)

# Teach upstream's spi component about host. The top-level CONFIG_SCHEMA
# requires clk/miso/mosi pins and restricts the platform list; the to_code
# builds a hardware bus from those pins. On host the kernel manages the
# controller via /dev/spidev*, so neither applies. Patches are gated by
# CORE.is_host so other platforms are untouched.
_PATCHED_FLAG = "_linux_spi_patched"
if not getattr(_upstream_spi, _PATCHED_FLAG, False):
    _orig_config_schema = _upstream_spi.CONFIG_SCHEMA

    def _config_schema_host_aware(value):
        if CORE.is_host:
            # AUTO_LOAD'd empty list, an explicit user entry, or the AutoLoad
            # sentinel: pass them through. The host to_code is a no-op so the
            # shape doesn't matter beyond "iterable".
            if not value:
                return []
            if isinstance(value, list):
                return value
            return [value]
        return _orig_config_schema(value)

    _upstream_spi.CONFIG_SCHEMA = _config_schema_host_aware

    _orig_to_code = _upstream_spi.to_code

    @coroutine_with_priority(CoroPriority.BUS)
    async def _to_code_host_aware(configs):
        if CORE.is_host:
            cg.add_define("USE_SPI")
            cg.add_global(spi_ns.using)
            return
        await _orig_to_code(configs)

    _upstream_spi.to_code = _to_code_host_aware
    setattr(_upstream_spi, _PATCHED_FLAG, True)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LinuxSPIComponent),
        cv.Optional(CONF_DEVICE, default=DEFAULT_DEVICE): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


@coroutine_with_priority(CoroPriority.BUS)
async def to_code(config):
    cg.add_define("USE_SPI")
    cg.add_global(spi_ns.using)
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_device(config[CONF_DEVICE]))
