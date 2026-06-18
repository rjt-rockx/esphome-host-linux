"""Non-happy-path resilience: the host must degrade gracefully (never crash) when
BlueZ does something unwelcome — rejects a registration, hands us a malformed
advert, has a central already connected at startup, power-cycles the adapter,
removes the adapter, or disappears entirely.

The core guarantee asserted here is "no crash / keeps running". Automatic
re-registration after bluetoothd *restarts* (or the adapter is re-added) is a
documented known gap (see tests/SCENARIOS.md): in production a service manager
restarts the host binary. These tests assert it survives the event itself.
"""

import time

import dbus

FCD2 = "0000fcd2-0000-1000-8000-00805f9b34fb"
RX_MAC = "A4:C1:38:00:00:07"
SCAN_MAC = "A4:C1:38:00:00:10"


def _alive(host, settle=1.0):
    time.sleep(settle)
    return host.proc.poll() is None


def _service_data(payload):
    return {
        "ServiceData": dbus.Dictionary(
            {dbus.String(FCD2): dbus.Array([dbus.Byte(b) for b in payload], signature="y")},
            signature="sv",
        )
    }


# --- registration rejected ------------------------------------------------

def test_register_application_error_is_graceful(bluez, run_host):
    bluez.set_register_application_error("org.bluez.Error.Failed")
    host = run_host("ble-server")
    assert host.wait_for_log("RegisterApplication failed", timeout=20), (
        "server did not report the injected RegisterApplication error; log:\n"
        + "\n".join(host.snapshot()[-20:])
    )
    assert _alive(host), "server crashed after a RegisterApplication error"


def test_register_advertisement_already_exists_is_graceful(bluez, run_host):
    # AlreadyExists is the classic "stale instance from a previous run" failure.
    bluez.set_advertise_error("org.bluez.Error.AlreadyExists")
    host = run_host("ble-beacon")
    assert host.wait_for_log("RegisterAdvertisement failed", timeout=20)
    assert _alive(host), "beacon crashed on AlreadyExists"


# --- malformed inbound data -----------------------------------------------

def test_malformed_bthome_servicedata_does_not_crash(bluez, run_host):
    host = run_host("ble-bthome-receiver")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    # device-info says objects follow, but the temperature object (0x02, 2 bytes)
    # is truncated to one byte -> parser must bail, not read past the buffer.
    bluez.inject_device(RX_MAC, _service_data([0x40, 0x02, 0xCA]))
    assert _alive(host), "receiver crashed on a truncated BTHome payload"
    # and it must not have fabricated a reading
    assert host.wait_for_log("BTHome temperature' >>", timeout=2) is None, "decoded a value from a truncated frame"


def test_empty_bthome_servicedata_does_not_crash(bluez, run_host):
    host = run_host("ble-bthome-receiver")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    bluez.inject_device(RX_MAC, _service_data([]))  # zero-length service data
    assert _alive(host), "receiver crashed on empty service data"


# --- already-connected central at startup ---------------------------------

def test_pre_connected_central_is_enumerated(bluez, run_host):
    # BlueZ keeps the ACL link across our process restarts: a central can already
    # be Connected=true before the server installs its watch. do_start_ must
    # enumerate it (GetManagedObjects) and emit a synthetic connect.
    bluez.inject_device("AA:BB:CC:DD:EE:FF", {"Connected": dbus.Boolean(True)})
    host = run_host("ble-server")
    assert host.wait_for_log("central connected: conn_id=", timeout=20), (
        "pre-connected central was not enumerated at startup; log:\n"
        + "\n".join(host.snapshot()[-25:])
    )


# --- adapter / daemon disruption (no-crash guarantee) ---------------------

def test_adapter_power_cycle_does_not_crash(bluez, run_host):
    host = run_host("ble-scanner")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    bluez.set_powered(False)
    time.sleep(0.5)
    bluez.set_powered(True)
    assert _alive(host), "scanner crashed when the adapter power-cycled"


def test_adapter_removed_does_not_crash(bluez, run_host):
    host = run_host("ble-scanner")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    bluez.remove_adapter()
    assert _alive(host, settle=1.5), "scanner crashed when the adapter was removed"


def test_bluetoothd_disappearing_does_not_crash(bluez, run_host):
    host = run_host("ble-scanner")
    assert bluez.wait_for_call("StartDiscovery", timeout=20)
    bluez.kill_daemon()
    assert _alive(host, settle=1.5), "scanner crashed when bluetoothd disappeared"
