"""Compatibility shims for running ESPHome components on the host platform.

Holds narrow fixes that don't belong inside any one linux_* component:
missing symbol definitions, header-only workarounds, etc. Each .cpp guards
itself with USE_HOST and __has_include, so it compiles to nothing unless the
component it patches over is in the build.

Opt-in: add `linux_compat:` to YAML when a build needs the shims (e.g.
MCP23xxx GPIO expanders on host). Not auto-loaded, so it stays out of the
compile path for setups that don't need it.
"""

from esphome.components.host_patches import ensure_patch_script
import esphome.config_validation as cv

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

CONFIG_SCHEMA = cv.Schema({})

# The "you forgot linux_compat" hint lives in the mcp23xxx_base shadow: ESPHome
# only runs FINAL_VALIDATE_SCHEMA for components present in the config, so a
# check here could never fire for the configs that need it.


async def to_code(_config):
    ensure_patch_script()
