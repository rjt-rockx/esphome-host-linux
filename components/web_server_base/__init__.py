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
import esphome.final_validate as fv
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.helpers import copy_file_if_changed

CODEOWNERS = ["@rjt-rockx", "@esphome/core"]
DEPENDENCIES = ["network"]


def AUTO_LOAD():
    # Host-only shadow: on any other platform the stock component must be used
    # (see to_code), so nothing to auto-load here.
    return []


web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"


def _consume_web_server_base_sockets(config):
    from esphome.components import socket

    socket.consume_sockets(1, "web_server_base", socket.SocketType.TCP_LISTEN)(config)
    return config


def _host_only(config):
    if not CORE.is_host:
        raise cv.Invalid(
            "This web_server_base is a host-only shadow of the upstream "
            "component. Restrict this repository's external_components entry to "
            "host builds to use the stock web_server_base elsewhere."
        )
    return config


CONFIG_SCHEMA = cv.All(
    _host_only,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebServerBase),
        }
    ),
    _consume_web_server_base_sockets,
)


def _final_validate(config):
    # The host AsyncWebServer shim implements Basic auth only; there is no MD5
    # digest challenge/response path (upstream's lives in web_server_idf).
    auth = fv.full_config.get().get("web_server", {}).get("auth") or {}
    if auth.get("type") == "digest":
        raise cv.Invalid(
            "web_server 'auth: type: digest' is not supported on host; use "
            "'type: basic' (the default).",
            path=["web_server", "auth", "type"],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


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

    cg.add_define("WEB_SERVER_DEFAULT_HEADERS_COUNT", 1)
    # pthread for the std::thread accept loop.
    cg.add_build_flag("-pthread")
    # Pre-script that injects USE_HOST branches into web_server.h /
    # list_entities.{h,cpp} before PIO compiles them. Idempotent.
    script_dst = CORE.relative_build_path("patch_web_server.py")
    copy_file_if_changed(Path(__file__).parent / "patch_web_server.py.script", script_dst)
    existing = CORE.platformio_options.get("extra_scripts", []) or []
    if "pre:patch_web_server.py" not in existing:
        CORE.add_platformio_option("extra_scripts", ["pre:patch_web_server.py"])
