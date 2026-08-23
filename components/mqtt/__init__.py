"""Shadow esphome.components.mqtt for host.

The stock mqtt component restricts its schema to ESP32 / ESP8266 / BK72XX /
RTL87XX via `cv.only_on(...)` and statically picks an MQTTBackend per platform.
This shadow:

1. Monkey-patches cv.only_on at import time so host isn't rejected.
2. Re-executes the stock __init__.py inside this module so its schema, helpers,
   and to_code coroutine pass through verbatim.
3. Wraps to_code so on host it also copies mqtt_backend_host.{cpp,h} into the
   build and registers a pre-script that patches mqtt_client.{h,cpp} to use the
   host backend.
"""

from __future__ import annotations

from pathlib import Path

import esphome as _esphome
import esphome.codegen as cg
from esphome.components.host_patches import ensure_patch_script
import esphome.config_validation as cv
from esphome.core import CORE

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


# ESPHome ships only files found in the active component's package directory
# (via importlib.resources). This shadow dir holds just the host backend and
# this __init__.py, so the stock mqtt_sensor.h / mqtt_client.cpp / etc. would
# never reach the build. Mirror them in at module-load time so resource
# discovery and esphome.h codegen find them.
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


@coroutine_with_priority(CoroPriority.WEB)  # noqa: F821 — from exec'd upstream.
async def to_code(config):  # noqa: F811 — override upstream coroutine.
    await _upstream_to_code(config)

    if not CORE.is_host:
        return

    cg.add_build_flag("-pthread")
    # Link libmosquitto. add_library() resolves against PIO's registry, which
    # has no "mosquitto" package, so pass the system lib via a raw -l flag.
    CORE.add_platformio_option("build_flags", ["-lmosquitto"])

    # The pre-script is what actually drops upstream-mqtt sources and our
    # host backend into the build dir, after copy_src_tree has settled. It
    # also patches mqtt_client.{h,cpp} to use MQTTBackendHost.
    ensure_patch_script()
