"""GATT client (host connects out to a peripheral): with auto_connect, once the
scanner discovers the configured MAC the host must call Device1.Connect on it.
Proves the discover -> connect contract (the full read/write/notify chain is
hardware-verified separately against the C6 oracle)."""

TARGET = "40:4C:CA:57:8B:76"  # ble-client-connect.yaml auto_connect target
TARGET_PATH = "/org/bluez/hci0/dev_40_4C_CA_57_8B_76"


def test_client_connects_to_discovered_target(bluez, run_host):
    host = run_host("ble-client-connect")

    # The client drives discovery to find its target.
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "client never called StartDiscovery"

    # The target peripheral appears (connectable: Connect flips Connected=true).
    bluez.inject_connectable_device(TARGET)

    # auto_connect -> the host issues Device1.Connect on the matched device.
    assert bluez.wait_for_device_call(TARGET_PATH, "Connect", timeout=15), (
        "host did not call Device1.Connect on the discovered target; log tail:\n"
        + "\n".join(host.snapshot()[-25:])
    )
    assert host.proc.poll() is None, "client crashed during connect"
