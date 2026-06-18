"""BTHome transmit: the host registers an LEAdvertisement1 whose ServiceData is a
correct BTHome v2 payload. Exercises the real D-Bus advertising path + the host's
own ServiceData property getter (read back over the bus)."""

BTHOME_ADV_PATH = "/org/esphome/host/ble/bthome/advertisement0"
FCD2 = "0000fcd2-0000-1000-8000-00805f9b34fb"


def test_advertiser_registers_correct_bthome_servicedata(bluez, run_host):
    host = run_host("ble-bthome-advertiser")

    # The host logs success once BlueZ (the mock) accepts the registration.
    assert host.wait_for_log("BTHome advertisement registered", timeout=20), (
        "host never registered its advertisement; log:\n" + "\n".join(host.snapshot()[-20:])
    )

    # Read the advertisement object the host exported and invoke its real getters.
    props = bluez.wait_for_advertisement(BTHOME_ADV_PATH, timeout=10)
    assert props is not None, "no LEAdvertisement1 found at the BTHome advert path"

    assert str(props["Type"]) == "broadcast"
    assert FCD2 in [str(u) for u in props["ServiceUUIDs"]]

    payload = bytes(props["ServiceData"][FCD2])
    # temp 25.06 / hum 50.55 / batt 93, device-info 0x40, sorted by object id.
    assert payload.hex() == "40015d02ca0903bf13", payload.hex()


def test_advertiser_handles_register_error(bluez, run_host):
    # If BlueZ rejects RegisterAdvertisement, the host must log the failure and not
    # crash (graceful degradation, not a hang or abort).
    bluez.set_advertise_error("org.bluez.Error.Failed")
    host = run_host("ble-bthome-advertiser")
    assert host.wait_for_log("RegisterAdvertisement failed", timeout=20), (
        "host did not report the injected RegisterAdvertisement error"
    )
    # process still alive a moment later (didn't crash on the error path)
    import time

    time.sleep(1.0)
    assert host.proc.poll() is None, "host crashed after a RegisterAdvertisement error"
