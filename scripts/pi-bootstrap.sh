#!/usr/bin/env bash
# One-time setup on a fresh Raspberry Pi for esphome-host-linux.
# Idempotent: safe to re-run.

set -euo pipefail

echo "=== apt deps ==="
sudo apt-get update
sudo apt-get install -y \
  liblgpio-dev \
  libgpiod-dev \
  libmosquitto-dev \
  bluez \
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
if [ ! -d "$HOME/esphome-venv" ]; then
  python3 -m venv "$HOME/esphome-venv"
fi
"$HOME/esphome-venv/bin/pip" install --upgrade pip wheel
"$HOME/esphome-venv/bin/pip" install --upgrade esphome

echo "=== versions ==="
"$HOME/esphome-venv/bin/esphome" version
# ldconfig usually lives in /sbin which isn't in a non-root user's PATH on
# Debian; reach for it explicitly so the lib check still runs.
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig -p | grep -E 'lgpio|gpiod|mosquitto' || true
elif [ -x /sbin/ldconfig ]; then
  /sbin/ldconfig -p | grep -E 'lgpio|gpiod|mosquitto' || true
fi

echo "=== done ==="
echo "If group membership was added, log out and back in before running 'make run'."
