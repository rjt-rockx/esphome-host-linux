#!/usr/bin/env bash
# One-time setup on a fresh Raspberry Pi for esphome-host-linux.
# Idempotent: safe to re-run.

set -euo pipefail

echo "=== apt deps ==="
sudo apt-get update
sudo apt-get install -y \
  libmosquitto-dev \
  bluez \
  libsystemd-dev \
  libcap2-bin \
  python3-pip \
  python3-venv \
  python3-dev \
  build-essential \
  git

echo "=== gpio group ==="
if ! id -nG "$USER" | grep -qw gpio; then
  sudo usermod -aG gpio "$USER"
  echo "Added $USER to gpio group. Log out and back in for it to take effect."
fi

echo "=== esphome venv ==="
# ESPHome >= 2026.7 requires Python >= 3.12; on an older interpreter pip
# silently resolves ESPHome 2026.6.5, which predates the ble_device_base
# interface this repo targets. Pick the newest supported interpreter.
PYTHON=""
for candidate in python3.13 python3.12 python3; do
  if command -v "$candidate" >/dev/null 2>&1 &&
     "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)'; then
    PYTHON="$candidate"
    break
  fi
done
if [ -z "$PYTHON" ]; then
  echo "ERROR: no Python >= 3.12 found. ESPHome >= 2026.7 (required by this repo)" >&2
  echo "does not support Python 3.11. On Pi OS Bookworm / Debian 12, upgrade to" >&2
  echo "Debian 13 (trixie), or install a newer Python (e.g. via pyenv) and re-run." >&2
  exit 1
fi
if [ -d "$HOME/esphome-venv" ] &&
   ! "$HOME/esphome-venv/bin/python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' 2>/dev/null; then
  echo "Existing venv uses an unsupported Python; recreating with $PYTHON."
  rm -rf "$HOME/esphome-venv"
fi
if [ ! -d "$HOME/esphome-venv" ]; then
  "$PYTHON" -m venv "$HOME/esphome-venv"
fi
"$HOME/esphome-venv/bin/pip" install --upgrade pip wheel
"$HOME/esphome-venv/bin/pip" install --upgrade esphome

echo "=== versions ==="
"$HOME/esphome-venv/bin/esphome" version
# ldconfig usually lives in /sbin which isn't in a non-root user's PATH on
# Debian; reach for it explicitly so the lib check still runs.
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig -p | grep -E 'mosquitto|systemd' || true
elif [ -x /sbin/ldconfig ]; then
  /sbin/ldconfig -p | grep -E 'mosquitto|systemd' || true
fi

echo "=== done ==="
echo "If group membership was added, log out and back in before running 'make run'."
