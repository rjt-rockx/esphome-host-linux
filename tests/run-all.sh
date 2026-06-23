#!/usr/bin/env bash
# Run every hardware-free test tier for the esphome-host-linux BLE backend:
#
#   1. unit         pure-C++ logic (g++; no esphome, no D-Bus, no hardware)
#   2. config       `esphome config` over all examples + valid/invalid fixtures
#   3. integration  the real compiled host binary against a fake org.bluez
#                   (python-dbusmock on a private system bus; no hardware)
#
# The live two-host / ESP-oracle tier needs real radios and is run by hand
# (see tests/README.md + references/ble-host/RUN-LOG.md) — not here.
#
# Usage:
#   tests/run-all.sh              # all tiers (integration auto-skips if deps absent)
#   tests/run-all.sh unit         # one or more specific tiers
#   tests/run-all.sh unit config
#
# Env:
#   ESPHOME=/path/to/esphome   esphome entrypoint (default: `esphome` on PATH)
#                              Tip: the integration tier must run under the SYSTEM
#                              python3 (it imports dbusmock), so point ESPHOME at a
#                              venv esphome rather than activating that venv.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPHOME="${ESPHOME:-esphome}"

tiers=("$@")
[[ ${#tiers[@]} -eq 0 ]] && tiers=(unit config integration)

rc=0
ran=()
skipped=()

run_unit() {
  echo "===> UNIT"
  if bash "$here/unit/run.sh"; then ran+=("unit"); else rc=1; fi
}

run_config() {
  echo "===> CONFIG"
  if ! command -v "$ESPHOME" >/dev/null 2>&1 && [[ ! -x "$ESPHOME" ]]; then
    echo "SKIP config: esphome not found (set ESPHOME=/path/to/esphome)"
    skipped+=("config"); return
  fi
  if ESPHOME="$ESPHOME" python3 "$here/config/run.py"; then ran+=("config"); else rc=1; fi
}

run_integration() {
  echo "===> INTEGRATION"
  # sd-bus in the host binary must reach the PRIVATE bus the harness spins up, not
  # whatever DBUS_SYSTEM_BUS_ADDRESS the caller's shell may already export.
  unset DBUS_SYSTEM_BUS_ADDRESS
  # The harness imports dbusmock/dbus/gi and runs pytest under the system python3.
  if ! python3 -c 'import dbusmock, dbus, gi, pytest' >/dev/null 2>&1; then
    echo "SKIP integration: needs python3-dbusmock python3-dbus python3-gi python3-pytest"
    skipped+=("integration"); return
  fi
  if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "SKIP integration: dbus-daemon not on PATH (install the 'dbus' package)"
    skipped+=("integration"); return
  fi
  if ESPHOME="$ESPHOME" python3 -m pytest "$here/integration" -p no:cacheprovider -q; then
    ran+=("integration")
  else
    rc=1
  fi
}

for t in "${tiers[@]}"; do
  case "$t" in
    unit) run_unit ;;
    config) run_config ;;
    integration) run_integration ;;
    *) echo "unknown tier: $t (want: unit config integration)"; rc=1 ;;
  esac
done

echo
echo "===> SUMMARY"
[[ ${#ran[@]} -gt 0 ]] && echo "  ran:     ${ran[*]}"
[[ ${#skipped[@]} -gt 0 ]] && echo "  skipped: ${skipped[*]}"
if [[ $rc -eq 0 ]]; then echo "  result:  PASS"; else echo "  result:  FAIL"; fi
exit $rc
