#!/usr/bin/env bash
# Grant a compiled esphome-host-linux binary the Linux capabilities it needs
# for raw HCI socket access (BLE scanning). Run this once after each compile.
#
# Usage:
#   scripts/pi-bless-binary.sh path/to/program
#
# The capabilities (cap_net_admin, cap_net_raw) are inherited+effective on
# exec, so the binary can `socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)` and
# issue LE scan HCI commands without sudo.

set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 path/to/program" >&2
  exit 2
fi

PROG="$1"
if [ ! -x "$PROG" ]; then
  echo "Not an executable: $PROG" >&2
  exit 1
fi

sudo setcap 'cap_net_admin,cap_net_raw+eip' "$PROG"
echo "Granted cap_net_admin,cap_net_raw to: $PROG"
getcap "$PROG"
