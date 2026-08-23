"""linux_i2c: removed, superseded by native host I2C in ESPHome 2026.6.0.

Upstream esphome/esphome#14489 added `i2c: { device: /dev/i2c-N }` on the host
platform with the same ioctl backend this component provided. Keeping a second
bus implementation alive means shadowing the upstream i2c component, which in
turn breaks the native bus. So this is now a migration stub that fails config
validation with the replacement snippet.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@rjt-rockx"]

CONF_DEVICE = "device"


def _removed(config):
    device = "/dev/i2c-1"
    if isinstance(config, dict):
        device = config.get(CONF_DEVICE, device)
    raise cv.Invalid(
        "linux_i2c was removed: ESPHome 2026.6.0 ships native host I2C "
        "(esphome/esphome#14489). Replace:\n"
        "  linux_i2c:\n"
        "    id: bus_a\n"
        f"    device: {device}\n"
        "with the upstream i2c component:\n"
        "  i2c:\n"
        "    id: bus_a\n"
        f"    device: {device}\n"
        "Device IDs referenced via i2c_id: stay the same. "
        "See https://esphome.io/components/i2c"
    )


CONFIG_SCHEMA = _removed
