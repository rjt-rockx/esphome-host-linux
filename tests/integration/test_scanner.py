"""Passive scan: the host's D-Bus scanner starts discovery and an injected advert
at the configured MAC drives ble_rssi (and ble_presence). Proves the core
gateway scenario: InterfacesAdded -> ESPBTDevice -> listener -> sensor publish."""

import dbus

SCAN_MAC = "A4:C1:38:00:00:10"


def test_scanner_starts_discovery_and_reports_rssi(bluez, run_host):
    host = run_host("ble-scanner")

    # The scanner must drive BlueZ discovery.
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "scanner never called StartDiscovery"

    # An advert appears for the watched device, carrying an RSSI.
    bluez.inject_device(SCAN_MAC, {"RSSI": dbus.Int16(-57)})

    # ble_rssi publishes the advertised RSSI (VERBOSE "'<name>' >> <value>").
    assert host.wait_for_log("scanner rssi' >> -57", timeout=10), (
        "RSSI not reported; log tail:\n" + "\n".join(host.snapshot()[-20:])
    )
    # ble_presence flips ON for the now-present device (VERBOSE "'<name>' >> ON").
    assert host.wait_for_log("scanner presence' >> ON", timeout=5), "presence not set"


def test_scanner_handles_advert_with_no_rssi(bluez, run_host):
    # A device with no RSSI key (BlueZ sometimes omits it) must not crash the
    # scanner; presence should still register.
    host = run_host("ble-scanner")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    bluez.inject_device(SCAN_MAC, {})  # no RSSI, no service data
    assert host.wait_for_log("scanner presence' >> ON", timeout=10), (
        "presence not set for RSSI-less advert; log tail:\n" + "\n".join(host.snapshot()[-20:])
    )
    import time

    time.sleep(0.5)
    assert host.proc.poll() is None, "host crashed on an advert with no RSSI"
