"""Shadow web_server_base for host.

Replaces web_server_base on host so the C++ header gets a USE_HOST branch
(an AsyncWebServer shim backed by POSIX sockets).

Also neutralizes the cv.only_on(...) gate in `web_server` so the stock
`web_server:` config block works unchanged on host.
"""

from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.helpers import copy_file_if_changed

CODEOWNERS = ["@rjt-rockx", "@esphome/core"]
DEPENDENCIES = ["network"]


def AUTO_LOAD():
    if CORE.is_host:
        # Our shim is bundled here; no extra components needed.
        return []
    if CORE.is_esp32:
        return ["web_server_idf"]
    if CORE.using_arduino:
        return ["async_tcp"]
    return []


web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"


def _consume_web_server_base_sockets(config):
    from esphome.components import socket

    socket.consume_sockets(1, "web_server_base", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebServerBase),
        }
    ),
    _consume_web_server_base_sockets,
)


# --------------------------------------------------------------------------
# Patch cv.only_on at import time so any CONFIG_SCHEMA built afterwards (e.g.
# web_server, mqtt, sntp) does not reject the host platform. On host the
# wrapper returns a no-op validator; on other platforms behavior is unchanged.
# The patch is process-global (cv is a singleton module).
# --------------------------------------------------------------------------
if not getattr(cv, "_pi_only_on_patched", False):
    _original_only_on = cv.only_on

    def _only_on_host_aware(platforms):
        if CORE.is_host:
            # The validator must still pass through the value untouched.
            def _passthrough(value):
                return value

            return _passthrough
        return _original_only_on(platforms)

    cv.only_on = _only_on_host_aware
    cv._pi_only_on_patched = True


@coroutine_with_priority(CoroPriority.WEB_SERVER_BASE)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(cg.RawExpression(f"{web_server_base_ns}::global_web_server_base = {var}"))

    if CORE.is_host:
        cg.add_define("WEB_SERVER_DEFAULT_HEADERS_COUNT", 1)
        # pthread for the std::thread accept loop.
        cg.add_build_flag("-pthread")
        # Pre-script that injects USE_HOST branches into web_server.h /
        # list_entities.{h,cpp} before PIO compiles them. Idempotent.
        script_dst = CORE.relative_build_path("patch_web_server.py")
        copy_file_if_changed(
            Path(__file__).parent / "patch_web_server.py.script", script_dst
        )
        existing = CORE.platformio_options.get("extra_scripts", []) or []
        if "pre:patch_web_server.py" not in existing:
            CORE.add_platformio_option(
                "extra_scripts", ["pre:patch_web_server.py"]
            )
        return

    if CORE.is_esp32:
        cg.add_define("WEB_SERVER_DEFAULT_HEADERS_COUNT", 1)
        return

    if CORE.using_arduino:
        if CORE.is_esp8266:
            cg.add_library("ESP8266WiFi", None)
        if CORE.is_libretiny:
            CORE.add_platformio_option("lib_ignore", ["ESPAsyncTCP", "RPAsyncTCP"])
        if CORE.is_rp2:
            CORE.add_platformio_option(
                "lib_ignore", ["ESPAsyncTCP", "AsyncTCP", "AsyncTCP_RP2040W"]
            )
            cg.add_library("Hash", None)
            copy_file_if_changed(
                Path(__file__).parent / "fix_rp2040_hash.py.script",
                CORE.relative_build_path("fix_rp2040_hash.py"),
            )
            cg.add_platformio_option("extra_scripts", ["pre:fix_rp2040_hash.py"])
        cg.add_library("ESP32Async/ESPAsyncWebServer", "3.9.6")
