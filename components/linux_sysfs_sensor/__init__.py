"""linux_sysfs_sensor: publish a numeric Linux sysfs attribute as a sensor.

This component only provides a `sensor` platform; the schema and codegen live
in sensor.py. This file marks the package so ESPHome's loader can enumerate the
C++ sources.
"""

import esphome.codegen as cg

CODEOWNERS = ["@rjt-rockx"]
DEPENDENCIES = ["host"]

linux_sysfs_sensor_ns = cg.esphome_ns.namespace("linux_sysfs_sensor")
