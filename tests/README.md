# tests — esphome-host-linux BLE backend

A hardware-free test suite that keeps the Linux/BlueZ BLE backend reliable for the
real-world scenarios people use it for (see [SCENARIOS.md](SCENARIOS.md) for the
scenario → test map). Three tiers, all runnable on a machine with no Bluetooth
radio, plus a manual live tier for on-air / cross-radio parity.

```
tests/
  unit/         pure-C++ logic (g++; no esphome, no D-Bus, no hardware)
  config/       `esphome config` over every example + valid/invalid fixtures
  integration/  the REAL compiled host binary against a fake org.bluez
  run-all.sh    run all three tiers (each skips cleanly if its deps are absent)
```

## Quick start

```bash
# everything (integration auto-skips if python3-dbusmock isn't installed)
ESPHOME=/path/to/esphome tests/run-all.sh

# one tier at a time
tests/run-all.sh unit
ESPHOME=/path/to/esphome tests/run-all.sh config
ESPHOME=/path/to/esphome tests/run-all.sh integration
```

`ESPHOME` points at the esphome entrypoint (default: `esphome` on PATH). Point it
at a venv's `esphome` rather than activating the venv — the integration tier must
run under the **system** python3 (it imports `dbusmock`), while esphome itself can
live anywhere.

## Tier 1 — unit (`tests/unit`)

Pure logic with no dependencies beyond `g++` (C++20). Each `test_*.cpp` builds into
its own binary and runs; the tier fails if any check fails.

```bash
tests/unit/run.sh
```

- `test_ble_uuid.cpp` — `ESPBTUUID` parse/format/compare; pins that custom 128-bit
  UUIDs are hex-parsed and exported as bytes, not ASCII.
- `test_bthome_encoder.cpp` — BTHome v2 service-data payload bytes. Golden:
  temp 25.06 / humidity 50.55 / battery 93 → `40 01 5D 02 CA 09 03 BF 13`
  (device-info byte, objects sorted ascending, little-endian scaled values),
  plus negative temperature, range clamping, binary objects, empty (`40`), and the
  encrypted device-info flag (`41`).

## Tier 2 — config (`tests/config`)

Drives `esphome config` (schema + final validation; no compile) over every
`examples/*.yaml` and the curated fixtures. Needs only `pip install esphome`.

```bash
ESPHOME=/path/to/esphome python3 tests/config/run.py
```

- `fixtures/valid/*` — must validate.
- `fixtures/invalid/*` — must be **rejected for the right reason**: each carries a
  `# EXPECT_ERROR: <substring>` marker the runner checks against the error output,
  so a config that fails for an unrelated reason is itself a failure.

## Tier 3 — integration (`tests/integration`)

The highest-value tier: it runs the **actual compiled host binary** against a fake
`org.bluez` so the real sd-bus code paths (scanner, GATT client/server, beacon,
BTHome advertiser) execute deterministically with no Bluetooth hardware.

How it works (`conftest.py`):

- `PrivateDBus(BusType.SYSTEM)` spins up a throwaway system bus and exports
  `DBUS_SYSTEM_BUS_ADDRESS`. sd-bus inside the host binary honors that env var, so
  **the binary needs zero changes** — it just talks to the mock instead of the real
  bus.
- `python-dbusmock`'s `bluez5` template provides `org.bluez` + an `hci0` adapter;
  helpers add `GattManager1`/`LEAdvertisingManager1`, inject `Device1` adverts
  (`AddObject` + `InterfacesAdded`/`PropertiesChanged`), flip `Connected`,
  error-inject the `Register*` calls, and drop the daemon.
- The binary is launched under `stdbuf -oL` (its logger block-buffers stdout when
  piped) and a reader thread feeds `wait_for_log(substr)`.

**Boundary:** dbusmock validates the D-Bus *contract* (what the binary offers and
calls on org.bluez). It does **not** drive the radio — on-air bytes and
legacy-vs-extended PDU are proven by the unit encoder golden and the live tier.

Requirements (Debian/Ubuntu):

```bash
sudo apt-get install -y python3-dbusmock python3-dbus python3-gi python3-pytest dbus
```

Run (note: **system** python3, and clear any inherited bus address):

```bash
cd tests/integration
unset DBUS_SYSTEM_BUS_ADDRESS
ESPHOME=/path/to/esphome python3 -m pytest -v
```

Binaries are compiled on demand (`ensure_built`) the first time a test needs an
example; subsequent runs reuse them. The example's esphome `name:` must equal its
file stem (the harness derives the build path from the stem).

| File | Scenario |
|------|----------|
| `test_scanner.py` | passive scan → ble_rssi / ble_presence; RSSI-less advert |
| `test_bthome_rx.py` | inject 0xFCD2 advert → decoded temp/humidity/battery |
| `test_bthome_tx.py` | advertiser registers correct BTHome ServiceData bytes |
| `test_beacon.py` | iBeacon ManufacturerData (UUID/major/minor/power) bytes |
| `test_gatt_server.py` | RegisterApplication + advertise; connect/disconnect → re-advertise |
| `test_gatt_client.py` | auto_connect → Device1.Connect on the discovered target |
| `test_resilience.py` | the non-happy-path: registration errors, malformed adverts, pre-connected central, adapter power-cycle/removal, daemon loss |

## Live tier (manual, real radios)

Not in CI — needs Bluetooth hardware. Two-host (legion ↔ Pi) and ESP32-oracle
parity for both BTHome directions, GATT client/server, and beacon/scanner, with
`btmon`/`busctl` as ground truth. Procedure and evidence:
`references/ble-host/RUN-LOG.md`.

## CI

`.github/workflows/validate.yml` runs the unit, config, and integration tiers (plus
the existing host-compile job) on every push/PR. The live tier is manual.
