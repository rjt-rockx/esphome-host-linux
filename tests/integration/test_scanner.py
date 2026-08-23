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


def test_oneshot_scan_stops_and_restarts(bluez, run_host):
    # continuous: false with duration: 2s — the scanner must stop the backend
    # (StopDiscovery) after the first period instead of scanning forever, and
    # the on_boot start_scan() at ~8s must start a fresh period.
    import time

    host = run_host("ble-oneshot")
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "scan never started"

    # One-shot: the scan ends on its own after ~2s.
    assert bluez.wait_for_call("StopDiscovery", timeout=15), "one-shot scan never stopped"
    assert host.wait_for_log("Scan stopped", timeout=5), "no scan-stop log"
    # ...and stays stopped: no second StartDiscovery before the 8s restart.
    time.sleep(1.0)
    assert len(bluez.calls("StartDiscovery")) == 1, "one-shot scan restarted itself"

    # The delayed start_scan() lambda brings the scanner back for a new period.
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and len(bluez.calls("StartDiscovery")) < 2:
        time.sleep(0.2)
    assert len(bluez.calls("StartDiscovery")) >= 2, "start_scan() did not restart the scan"
    # The restarted one-shot period ends too.
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and len(bluez.calls("StopDiscovery")) < 2:
        time.sleep(0.2)
    assert len(bluez.calls("StopDiscovery")) >= 2, "restarted scan never stopped"
