"""mcp23xxx_base shadow: adds a host-platform config check to the stock component.

MCP23XXXBase<N> declares pin_mode()/pin_interrupt_mode() virtual but leaves
them undefined; host builds keep RTTI on, so the base vtable is emitted and the
link fails unless `linux_compat:` supplies the definitions. The hint has to live
in a component that is loaded whenever an MCP23xxx expander is, and ESPHome only
runs FINAL_VALIDATE_SCHEMA for components present in the config, so it belongs
here rather than in linux_compat.

The Python below is the stock module executed verbatim; only FINAL_VALIDATE_SCHEMA
is added. mcp23xxx_base.h/.cpp are verbatim copies too — external components
replace a whole directory, so the C++ has to travel with the shadow. _check_drift
warns when the installed ESPHome copy no longer matches.
"""

import logging
from pathlib import Path

import esphome
from esphome import final_validate as fv
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

_UPSTREAM_DIR = Path(esphome.__file__).parent / "components" / "mcp23xxx_base"
_SHADOW_DIR = Path(__file__).parent

_upstream_init = _UPSTREAM_DIR / "__init__.py"
exec(compile(_upstream_init.read_text(), str(_upstream_init), "exec"), globals())  # noqa: S102

CODEOWNERS = ["@jesserockz", "@rjt-rockx"]


def _check_drift() -> None:
    for name in ("mcp23xxx_base.h", "mcp23xxx_base.cpp"):
        upstream = _UPSTREAM_DIR / name
        if upstream.is_file() and upstream.read_bytes() != (_SHADOW_DIR / name).read_bytes():
            _LOGGER.warning(
                "components/mcp23xxx_base/%s differs from the installed ESPHome copy; "
                "refresh the shadow from upstream",
                name,
            )


def _final_validate(config: ConfigType) -> ConfigType:
    """Fail with a clear hint rather than a link-time vtable error when an
    MCP23xxx expander is used on host without linux_compat."""
    if not CORE.is_host:
        return config
    _check_drift()
    if "linux_compat" not in fv.full_config.get():
        raise cv.Invalid(
            "MCP23xxx GPIO expanders on the host platform need the `linux_compat:` "
            "component loaded to supply missing upstream symbol definitions. "
            "Add an empty `linux_compat:` block to your config."
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate
