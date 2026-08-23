"""Compatibility shims for running ESPHome components on the host platform.

Holds narrow fixes that don't belong inside any one linux_* component:
missing symbol definitions, header-only workarounds, etc. Each .cpp guards
itself with USE_HOST and __has_include, so it compiles to nothing unless the
component it patches over is in the build.

Opt-in: add `linux_compat:` to YAML when a build needs the shims (e.g.
MCP23xxx GPIO expanders on host). Not auto-loaded, so it stays out of the
compile path for setups that don't need it.
"""

import esphome.config_validation as cv
from esphome import final_validate as fv
from esphome.core import CORE

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

CONFIG_SCHEMA = cv.Schema({})


def _final_validate(config):
    """Fail with a clear hint rather than a link-time vtable error when the
    upstream components needing our shims are present without linux_compat."""
    if not CORE.is_host:
        return config
    full = fv.full_config.get()
    if "mcp23xxx_base" in CORE.loaded_integrations and "linux_compat" not in full:
        raise cv.Invalid(
            "MCP23xxx GPIO expanders on the host platform need the `linux_compat:` "
            "component loaded to supply missing upstream symbol definitions. "
            "Add an empty `linux_compat:` block to your config."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(_config):
    pass
