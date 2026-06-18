"""GATT server (host as a BLE peripheral): registers its application + advertises,
and reacts to a central connecting/disconnecting. The disconnect must trigger a
re-advertise (UnregisterAdvertisement + RegisterAdvertisement) — the bluez#644
workaround for the controller not auto-resuming ext-adv after a central drops."""

import time

import dbus

CENTRAL = "11:22:33:44:55:66"
CENTRAL_PATH = "/org/bluez/hci0/dev_11_22_33_44_55_66"


def test_server_registers_application_and_advertises(bluez, run_host):
    host = run_host("ble-server")

    assert host.wait_for_log("GATT application registered", timeout=20), (
        "server never registered its GATT app; log:\n" + "\n".join(host.snapshot()[-20:])
    )
    assert bluez.wait_for_call("RegisterApplication", timeout=5), "GattManager1.RegisterApplication not called"

    # It also advertises so a central can find it.
    assert host.wait_for_log("LE advertisement registered", timeout=10)
    assert bluez.wait_for_call("RegisterAdvertisement", timeout=5), "LEAdvertisingManager1.RegisterAdvertisement not called"


def test_server_connect_disconnect_triggers_readvertise(bluez, run_host):
    host = run_host("ble-server")
    assert host.wait_for_log("LE advertisement registered", timeout=20)
    reg_before = len(bluez.calls("RegisterAdvertisement"))

    # A central connects: Device1 appears, then its Connected flips true.
    bluez.inject_device(CENTRAL, {"Connected": dbus.Boolean(False)})
    bluez.update_device(CENTRAL_PATH, {"Connected": dbus.Boolean(True)})
    assert host.wait_for_log("central connected: conn_id=", timeout=10), (
        "no on_connect; log tail:\n" + "\n".join(host.snapshot()[-20:])
    )

    # The central drops.
    bluez.update_device(CENTRAL_PATH, {"Connected": dbus.Boolean(False)})
    assert host.wait_for_log("central disconnected: conn_id=", timeout=10), "no on_disconnect"

    # The disconnect must force a fresh advertising set (bluez#644 workaround):
    # UnregisterAdvertisement, then another RegisterAdvertisement.
    assert bluez.wait_for_call("UnregisterAdvertisement", timeout=10), "did not re-advertise (no Unregister)"
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if len(bluez.calls("RegisterAdvertisement")) > reg_before:
            break
        time.sleep(0.05)
    assert len(bluez.calls("RegisterAdvertisement")) > reg_before, "advertisement not re-registered after disconnect"
    assert host.proc.poll() is None, "server crashed on the connect/disconnect cycle"
