"""iBeacon transmit: the host registers an LEAdvertisement1 whose ManufacturerData
under Apple's company id (0x004C) is a well-formed iBeacon frame. Exercises the
real advertising path + the host's ManufacturerData property getter."""

import dbus

BEACON_ADV_PATH = "/org/esphome/host/ble/beacon/advertisement0"
APPLE = 0x004C
# examples/ble-beacon.yaml: uuid a1b2c3d4-..., major 100, minor 200, power -59.
UUID_BYTES = bytes.fromhex("a1b2c3d40000100080000080" "5f9b34fb")


def test_beacon_registers_ibeacon_manufacturer_data(bluez, run_host):
    host = run_host("ble-beacon")

    assert host.wait_for_log("iBeacon advertisement registered", timeout=20), (
        "beacon never registered; log:\n" + "\n".join(host.snapshot()[-20:])
    )

    props = bluez.wait_for_advertisement(BEACON_ADV_PATH, timeout=10)
    assert props is not None, "no LEAdvertisement1 at the beacon path"
    assert str(props["Type"]) == "broadcast"

    md = props["ManufacturerData"]
    keys = [int(k) for k in md.keys()]
    assert APPLE in keys, f"no Apple company id in ManufacturerData ({keys})"
    data = bytes(md[dbus.UInt16(APPLE)])

    # iBeacon frame: 0x02 0x15 | 16B UUID | major(BE) | minor(BE) | measured power
    assert data[0:2] == b"\x02\x15", data.hex()
    assert data[2:18] == UUID_BYTES, data[2:18].hex()
    assert int.from_bytes(data[18:20], "big") == 100  # major
    assert int.from_bytes(data[20:22], "big") == 200  # minor
    assert data[22] == (-59 & 0xFF)  # measured power, 0xC5


def test_beacon_handles_register_error(bluez, run_host):
    bluez.set_advertise_error("org.bluez.Error.Failed")
    host = run_host("ble-beacon")
    assert host.wait_for_log("RegisterAdvertisement failed", timeout=20), (
        "beacon did not report the injected RegisterAdvertisement error"
    )
    import time

    time.sleep(1.0)
    assert host.proc.poll() is None, "host crashed after a RegisterAdvertisement error"
