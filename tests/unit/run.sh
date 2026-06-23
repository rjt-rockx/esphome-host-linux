#!/usr/bin/env bash
# Compile + run the standalone C++ unit tests for the BLE host backend.
#
# These cover the pure-logic units (no D-Bus, no hardware, no esphome core) so
# they run anywhere g++ is present, including CI. Each test_*.cpp is built into
# its own binary and run; the script exits non-zero if any test fails.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="$here/.build"
mkdir -p "$out"

CXX="${CXX:-g++}"
# std::span (used by ble_uuid.h) requires C++20.
CXXFLAGS="${CXXFLAGS:--std=c++20 -O1 -Wall -Wextra -DUSE_HOST}"

fail=0

build_and_run() {
  local name="$1"; shift
  echo "::group::unit $name"
  echo "+ building $name"
  # shellcheck disable=SC2086
  "$CXX" $CXXFLAGS "$@" -o "$out/$name"
  echo "+ running $name"
  if ! "$out/$name"; then
    fail=1
  fi
  echo "::endgroup::"
}

# test_ble_uuid: ESPBTUUID parse/format/compare contract.
build_and_run test_ble_uuid \
  -I"$repo/components/esp32_ble" \
  "$here/test_ble_uuid.cpp" \
  "$repo/components/esp32_ble/ble_uuid.cpp"

# test_bthome_encoder: BTHome v2 service-data payload bytes.
build_and_run test_bthome_encoder \
  -I"$repo/components/bthome_advertiser" \
  "$here/test_bthome_encoder.cpp" \
  "$repo/components/bthome_advertiser/bthome_encoder.cpp"

if [[ "$fail" -ne 0 ]]; then
  echo "UNIT TESTS FAILED"
  exit 1
fi
echo "ALL UNIT TESTS PASSED"
