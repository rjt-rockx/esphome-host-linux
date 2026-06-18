# Usage scenarios → test traceability

Who reaches for esphome-host-linux's BLE backend on Linux, and which automated
test keeps each scenario reliable. Every scenario maps to at least one
hardware-free test (unit / config / integration); the cross-radio properties
that can only be proven on real hardware are listed under **Live**.

Tiers: **U** = `tests/unit` (C++), **C** = `tests/config` (esphome validation),
**I** = `tests/integration` (real host binary vs. fake org.bluez), **L** = live
hardware (manual; `references/ble-host/RUN-LOG.md`).

| # | Scenario (who / what) | Tests | Tier |
|---|-----------------------|-------|------|
| 1 | **BLE sensor gateway** — passive scan, presence + RSSI for a watched device | `test_scanner.py::test_scanner_starts_discovery_and_reports_rssi` | I |
| 2 | **BTHome sensor receiver** — decode temp/humidity/battery from a 0xFCD2 advert | `test_bthome_rx.py`; `test_bthome_encoder.cpp` (byte golden); `bthome_receiver.yaml` fixture | U,C,I |
| 3 | **BTHome publisher** — Linux box broadcasts its own readings as BTHome v2 | `test_bthome_tx.py`; `test_bthome_encoder.cpp`; `bthome_advertiser.yaml` fixture | U,C,I |
| 4 | **iBeacon** — Linux box advertises an iBeacon (asset tag / room beacon) | `test_beacon.py`; `beacon_full.yaml` fixture | C,I |
| 5 | **GATT server** — Linux as a BLE peripheral (read/write/notify, advertise) | `test_gatt_server.py`; `server_multi_service.yaml` fixture | C,I |
| 6 | **GATT client** — Linux connects out to a peripheral (read/write/notify/RSSI/pair) | `test_gatt_client.py`; `ble-client-*.yaml` examples | C,I,L |
| 7 | **HA bluetooth_proxy** on a Linux box — forward adverts + active connections | `proxy_explicit_connections.yaml` fixture; live proxy parity | C,L |
| 8 | **Dev / CI without hardware** — validate + smoke-test on a machine with no radio | whole `tests/` suite runs hardware-free | U,C,I |
| 9 | **UUID export correctness** (custom 128-bit service/char UUIDs) | `test_ble_uuid.cpp` (regression pin behind the UUID-export fix) | U |
| 10 | **Config mistakes caught early** — bad MAC/UUID/range, missing required keys | `tests/config/fixtures/invalid/*` (each rejected for the right reason) | C |

## Non-happy-path (reliability under adverse BlueZ behavior) — `test_resilience.py`

| Adverse event | Expected behavior | Test |
|---------------|-------------------|------|
| `GattManager1.RegisterApplication` rejected | log + stay up | `test_register_application_error_is_graceful` |
| `RegisterAdvertisement` → AlreadyExists (stale instance) | log + stay up | `test_register_advertisement_already_exists_is_graceful` |
| Truncated BTHome ServiceData | bail parse, no over-read, no bogus reading | `test_malformed_bthome_servicedata_does_not_crash` |
| Empty BTHome ServiceData | no crash | `test_empty_bthome_servicedata_does_not_crash` |
| Central already connected at server start | synthetic connect (enumerate at startup) | `test_pre_connected_central_is_enumerated` |
| Adapter `Powered` off→on | no crash | `test_adapter_power_cycle_does_not_crash` |
| Adapter removed | no crash (graceful) | `test_adapter_removed_does_not_crash` |
| `bluetoothd` disappears | no crash (graceful) | `test_bluetoothd_disappearing_does_not_crash` |
| Central disconnects (bluez#644: ext-adv doesn't auto-resume) | force re-advertise (Unregister+Register) | `test_gatt_server.py::test_server_connect_disconnect_triggers_readvertise` |
| Advertiser teardown | unregisters its advertisement (no leaked instance) | covered by the TX/beacon register/teardown path |

## Known gaps (documented, not silently passing)

- **Auto re-registration after `bluetoothd` restart or adapter re-add is NOT
  implemented.** The host survives the event (no crash — tests above), but does
  not by itself re-`StartDiscovery` / re-`RegisterApplication` once org.bluez
  returns. In production a service manager (systemd) restarts the host binary,
  which re-registers cleanly. The components do not yet watch
  `org.freedesktop.DBus` `NameOwnerChanged` for org.bluez. Adding that watch to
  the scanner/server/advertisers is the natural follow-up; it was scoped out as
  not "contained" to a single component this pass.

## Live tier (real radios, manual) — see `references/ble-host/RUN-LOG.md`

On-air byte exactness (legacy ADV_NONCONN_IND vs. extended PDU), controller
variance (Intel AX211 ↔ Pi CYW43 ↔ ESP32 C6/C5/S3/D0WD), and full
connect→discover→read/write/notify chains are proven on hardware, not in the
fake-bus tier (dbusmock validates the D-Bus *contract*, not the radio):

- **BTHome** both directions: legion `bthome_advertiser` → second host decodes, and back.
- **GATT** client↔server both directions (swap roles).
- **Beacon/scanner** both directions.
- Cross-check against stock-esphome ESP32 boards as an independent oracle.
