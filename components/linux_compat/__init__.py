"""Compatibility shims for running upstream ESPHome components on the host
platform.

Holds fixes that are too narrow to deserve their own external component but
don't belong inside any one of our linux_* protocol components: missing
upstream symbol definitions, header-only workarounds, etc. Each .cpp in this
directory guards itself with USE_HOST and __has_include so the file compiles
to nothing if the upstream component it patches over isn't in the build.

Auto-loaded by other linux_* components. Not meant to be configured directly.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@rjt"]
DEPENDENCIES = ["host"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(_config):
    pass
