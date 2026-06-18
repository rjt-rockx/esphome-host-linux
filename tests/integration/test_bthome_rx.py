"""BTHome receive: inject a Service Data 0xFCD2 advert on the fake bus and assert
the host's stock bthome_mithermometer decodes temperature/humidity/battery.

Exercises the full receive chain on host: D-Bus scanner (StartDiscovery) ->
InterfacesAdded -> parse_device1_props_ -> ESPBTDevice -> bthome_mithermometer ->
sensor publish. The receiver (examples/ble-bthome-receiver.yaml) is keyed to
mac A4:C1:38:00:00:07 with logger level DEBUG."""

import dbus

RX_MAC = "A4:C1:38:00:00:07"
FCD2 = "0000fcd2-0000-1000-8000-00805f9b34fb"
# BTHome v2: battery 93%, temp 25.06 C, humidity 50.55 % (same golden as the encoder test).
PAYLOAD = [0x40, 0x01, 0x5D, 0x02, 0xCA, 0x09, 0x03, 0xBF, 0x13]


def _service_data_props(payload):
    return {
        "RSSI": dbus.Int16(-55),
        "ServiceData": dbus.Dictionary(
            {dbus.String(FCD2): dbus.Array([dbus.Byte(b) for b in payload], signature="y")},
            signature="sv",
        ),
    }


def test_host_decodes_injected_bthome_advert(bluez, run_host):
    host = run_host("ble-bthome-receiver")

    # The D-Bus scanner must drive discovery before adverts can arrive.
    assert bluez.wait_for_call("StartDiscovery", timeout=20), "scanner never called StartDiscovery"

    # Inject the BTHome advert as a freshly-appeared device.
    bluez.inject_device(RX_MAC, _service_data_props(PAYLOAD))

    # The decoded sensor values are logged at VERBOSE as "'<name>' >> <value> <unit>".
    assert host.wait_for_log("BTHome temperature' >> 25.06", timeout=10), (
        "temperature not decoded; log tail:\n" + "\n".join(host.snapshot()[-20:])
    )
    assert host.wait_for_log("BTHome humidity' >> 50.55", timeout=5), "humidity not decoded"
    assert host.wait_for_log("BTHome battery' >> 93", timeout=5), "battery not decoded"
    # Parser also confirms it accepted the frame for the configured MAC.
    assert host.wait_for_log("BTHome data from " + RX_MAC, timeout=5)
