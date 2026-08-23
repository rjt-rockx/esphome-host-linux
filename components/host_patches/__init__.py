"""Shared registration of the host build patch pre-script.

The PlatformIO pre-script (web_server_base/patch_web_server.py.script) rewrites
upstream sources after ESPHome copies them into the build tree: web_server
USE_HOST branches, the mqtt host backend swap, upstream SPI's `final` removal,
and third-party fixes such as the seesaw readbuf inter-transaction delay.

Every component in this repository calls ensure_patch_script() from its
to_code/registration path, so any host config that contains at least one repo
component gets the patches. A config that uses ZERO repo components (e.g. only
native `i2c:` plus third-party components) never imports this repository's
Python, so it cannot receive the patches; such configs can add an explicit
`host_patches:` block to opt in.

This module must stay free of import side effects: it is imported by every
repo component, and notably must NOT import web_server_base/__init__.py
(which monkeypatches cv.only_on at import time). The script file itself stays
in web_server_base/ and is resolved by path only.
"""

from __future__ import annotations

from pathlib import Path

import esphome.config_validation as cv
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed
from esphome.types import ConfigType

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

_SCRIPT_NAME = "patch_web_server.py"
_SCRIPT_SRC = Path(__file__).parent.parent / "web_server_base" / f"{_SCRIPT_NAME}.script"

CONFIG_SCHEMA = cv.Schema({})


def ensure_patch_script() -> None:
    """Copy the patch pre-script into the build dir and register it with
    PlatformIO. Idempotent: safe to call from any number of components."""
    if not CORE.is_host or not _SCRIPT_SRC.exists():
        return
    copy_file_if_changed(_SCRIPT_SRC, CORE.relative_build_path(_SCRIPT_NAME))
    existing = CORE.platformio_options.get("extra_scripts", []) or []
    if f"pre:{_SCRIPT_NAME}" not in existing:
        CORE.add_platformio_option("extra_scripts", [f"pre:{_SCRIPT_NAME}"])


async def to_code(_config: ConfigType) -> None:
    # Explicit `host_patches:` block: opt-in for configs with no other repo
    # component.
    ensure_patch_script()
