#!/usr/bin/env bash
# Print the Pi-side info we need for debugging.

set -u

section() { echo; echo "=== $1 ==="; }

section "model"
tr -d '\0' </proc/device-tree/model 2>/dev/null && echo

section "kernel"
uname -a

section "os-release"
grep -E '^(NAME|VERSION_ID|PRETTY_NAME)=' /etc/os-release 2>/dev/null

section "cpuinfo revision"
grep -E '^(Revision|Model|Hardware)' /proc/cpuinfo | sort -u

section "gpiochips"
for c in /dev/gpiochip*; do
  [ -e "$c" ] || continue
  base=$(basename "$c")
  label=$(cat "/sys/bus/gpio/devices/$base/label" 2>/dev/null || echo "?")
  lines=$(cat "/sys/bus/gpio/devices/$base/chip/ngpio" 2>/dev/null || echo "?")
  echo "$c  label=$label  lines=$lines"
done

section "i2c buses"
ls -1 /dev/i2c-* 2>/dev/null || echo "(none enabled)"

section "spi devices"
ls -1 /dev/spidev* 2>/dev/null || echo "(none enabled)"

section "groups"
id

section "lgpio"
ldconfig -p | grep lgpio || echo "(not installed)"

section "esphome"
[ -x "$HOME/esphome-venv/bin/esphome" ] && "$HOME/esphome-venv/bin/esphome" version || echo "(not installed in ~/esphome-venv)"
