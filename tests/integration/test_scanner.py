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


def test_startup_failure_reports_failed_not_completed(bluez, run_host):
    # No adapter: StartDiscovery fails and the worker exits during startup. The
    # scanner must report FAILED — not a "Scan stopped" / on_scan_end sequence
    # that would look like a successful completed scan.
    import time

    bluez.remove_adapter()
    host = run_host("ble-scanner")
    assert host.wait_for_log("StartDiscovery failed", timeout=20), "no startup failure logged"
    assert host.wait_for_log("scanner FAILED", timeout=10), "FAILED state never reported"
    time.sleep(1.0)
    joined = "\n".join(host.snapshot())
    assert "Scan stopped" not in joined, "startup failure was reported as a completed scan"
    assert host.proc.poll() is None, "host crashed on scanner startup failure"


def test_on_scan_end_can_restart_one_shot(bluez, run_host):
    # The on_scan_end automation in ble-oneshot-chain calls start_scan() while
    # the trigger runs inside the stop path. IDLE must be published before the
    # trigger fires, or the reentrant restart sees RUNNING and is refused —
    # with the fix, scan periods chain back to back.
    import time

    run_host("ble-oneshot-chain")
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "scan never started"
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline and len(bluez.calls("StartDiscovery")) < 3:
        time.sleep(0.2)
    assert len(bluez.calls("StartDiscovery")) >= 3, "on_scan_end start_scan() did not chain scan periods"
    assert len(bluez.calls("StopDiscovery")) >= 2, "chained periods did not stop the backend in between"


def test_stop_from_on_scan_end_fires_once_per_boundary(bluez, run_host):
    # ble-continuous-stop: a continuous scan whose on_scan_end automation logs
    # and calls stop_scan(). The trigger fires from the period timer while the
    # backend still runs, so the stop re-enters the stop path — the automation
    # must fire exactly once for that boundary (pre-fix it fired twice), and
    # the stopped scanner must stay stopped.
    import time

    host = run_host("ble-continuous-stop")
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "scan never started"
    assert host.wait_for_log("scan end fired", timeout=15), "on_scan_end never fired"
    assert bluez.wait_for_call("StopDiscovery", timeout=10), "stop_scan() did not stop the backend"
    time.sleep(2.5)  # past another would-be period boundary
    fired = sum("scan end fired" in line for line in host.snapshot())
    assert fired == 1, f"on_scan_end fired {fired} times for one scan boundary"
    assert len(bluez.calls("StartDiscovery")) == 1, "stopped scanner restarted itself"
