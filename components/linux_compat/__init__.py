"""Compatibility shims for running upstream ESPHome components on the host
platform.

Holds fixes that are too narrow to deserve their own external component but
don't belong inside any one of our linux_* protocol components: missing
upstream symbol definitions, header-only workarounds, etc. Each .cpp in this
directory guards itself with USE_HOST and __has_include so the file compiles
to nothing if the upstream component it patches over isn't in the build.

Opt-in only: users add `linux_compat:` to their YAML when they hit a build
that needs the shims (e.g. MCP23xxx GPIO expanders on host). Not auto-loaded
because most setups don't need it and it shouldn't sit in the compile path
of users who don't use the patched-around components.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(_config):
    pass
