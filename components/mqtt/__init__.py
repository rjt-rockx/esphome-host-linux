"""Shadow esphome.components.mqtt for host.

Upstream mqtt restricts its schema to ESP32 / ESP8266 / BK72XX / RTL87XX via
`cv.only_on(...)` and statically picks an MQTTBackend implementation per
platform. We:

1. Monkey-patch cv.only_on at import time so the host platform isn't rejected.
2. Re-execute upstream's __init__.py inside this module so the schema, schema
   helpers, and to_code coroutine come from upstream verbatim (any new keys
   the upstream adds upstream-side flow through automatically).
3. Wrap to_code so on host we also copy mqtt_backend_host.{cpp,h} into the
   build's component dir and register a pre-script that patches
   mqtt_client.{h,cpp} to use the host backend.
"""

from __future__ import annotations

from pathlib import Path

import esphome as _esphome
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.helpers import copy_file_if_changed

CODEOWNERS = ["@rjt-rockx"]

# ---------------------------------------------------------------------------
# Defang cv.only_on for host before upstream's CONFIG_SCHEMA is built.
# ---------------------------------------------------------------------------
if not getattr(cv, "_pi_only_on_patched", False):
    _original_only_on = cv.only_on

    def _only_on_host_aware(platforms):
        if CORE.is_host:

            def _passthrough(value):
                return value

            return _passthrough
        return _original_only_on(platforms)

    cv.only_on = _only_on_host_aware
    cv._pi_only_on_patched = True


# ---------------------------------------------------------------------------
# Locate upstream mqtt component and route submodule imports to it. Then
# exec upstream's __init__.py inside our globals so its schema and to_code
# become ours.
# ---------------------------------------------------------------------------
_upstream_dir = Path(_esphome.__file__).parent / "components" / "mqtt"
__path__ = [str(_upstream_dir)]


# ESPHome's source-tree copy only ships files declared as resources by the
# *active* component module (`importlib.resources.files(package).iterdir()`).
# Since our shadow's package directory only holds the host backend + this
# __init__.py, upstream's mqtt_sensor.h / mqtt_client.cpp / etc. never make
# it into the build. Mirror them into our shadow dir at module-load time so
# resource discovery and esphome.h codegen pick them up.
_shadow_dir = Path(__file__).parent
_upstream_marker = _shadow_dir / ".upstream_synced"
import shutil as _shutil  # noqa: E402
for _entry in _upstream_dir.iterdir():
    if not _entry.is_file():
        continue
    if _entry.suffix in (".py",):
        continue
    _dst = _shadow_dir / _entry.name
    if not _dst.exists() or _dst.stat().st_mtime < _entry.stat().st_mtime:
        _shutil.copy2(_entry, _dst)
_upstream_marker.write_text(str(_esphome.__file__))

_upstream_init = _upstream_dir / "__init__.py"
exec(  # noqa: S102 — sanctioned execution of trusted upstream code path.
    compile(_upstream_init.read_text(), str(_upstream_init), "exec"), globals()
)

# After exec, our globals contain CONFIG_SCHEMA, to_code, FILTER_SOURCE_FILES,
# mqtt_ns, MQTTClientComponent, ... captured from upstream.

_upstream_to_code = to_code  # noqa: F821 — defined by exec above.


async def to_code(config):  # noqa: F811 — override upstream coroutine.
    await _upstream_to_code(config)

    if not CORE.is_host:
        return

    cg.add_build_flag("-pthread")
    # Link with libmosquitto. add_library() targets PIO's registry which has
    # no "mosquitto" package; raw -l flag through PIO build_flags is the
    # idiomatic way for native deps.
    CORE.add_platformio_option("build_flags", ["-lmosquitto"])

    # The pre-script is what actually drops upstream-mqtt sources and our
    # host backend into the build dir, after copy_src_tree has settled. It
    # also patches mqtt_client.{h,cpp} to use MQTTBackendHost.
    script_dst = CORE.relative_build_path("patch_web_server.py")
    script_src = (
        Path(__file__).parent.parent
        / "web_server_base"
        / "patch_web_server.py.script"
    )
    if script_src.exists():
        copy_file_if_changed(script_src, script_dst)
        existing = CORE.platformio_options.get("extra_scripts", []) or []
        if "pre:patch_web_server.py" not in existing:
            CORE.add_platformio_option("extra_scripts", ["pre:patch_web_server.py"])
