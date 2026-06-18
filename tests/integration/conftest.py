"""Fake-BlueZ integration harness for the esphome-host-linux BLE components.

Spins up a private D-Bus *system* bus with a python-dbusmock `org.bluez` mock, then
runs the REAL compiled esphome host binary against it (sd-bus honors
DBUS_SYSTEM_BUS_ADDRESS, which PrivateDBus sets — no code change to the binary).
This exercises the actual D-Bus code paths (scanner, GATT client/server, beacon,
BTHome advertiser) deterministically, with no Bluetooth hardware, and lets us
inject the non-happy-path events (disconnect, bluetoothd restart, adapter loss,
RegisterApplication/Advertisement errors, pre-connected device).

Boundary: dbusmock validates the D-Bus *contract* (what the binary offers/calls on
org.bluez). It does NOT exercise the radio — on-air bytes / legacy-vs-extended PDU
are proven separately by the unit encoder tests and the live btmon tier.

Requires: python3-dbusmock, python3-dbus, python3-gi, dbus (dbus-daemon). Build the
binary with esphome (set ESPHOME=/path/to/esphome if not on PATH).
"""
from __future__ import annotations

import os
import pathlib
import signal
import subprocess
import threading
import time

import dbus
import pytest
from dbusmock import MOCK_IFACE, BusType, PrivateDBus, SpawnedMock

REPO = pathlib.Path(__file__).resolve().parents[2]
EXAMPLES = REPO / "examples"
BUILD = EXAMPLES / ".esphome" / "build"
ESPHOME = os.environ.get("ESPHOME", "esphome")

ORG_BLUEZ = "org.bluez"
ADAPTER_PATH = "/org/bluez/hci0"
DEV_IFACE = "org.bluez.Device1"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"
PROPS_IFACE = "org.freedesktop.DBus.Properties"
LE_ADV = "org.bluez.LEAdvertisement1"


# ---------------------------------------------------------------------------
# Building / running the host binary
# ---------------------------------------------------------------------------

def program_path(example: str) -> pathlib.Path:
    """Built native binary for an example (the esphome name == file stem here)."""
    return BUILD / example / ".pioenvs" / example / "program"


def ensure_built(example: str) -> pathlib.Path:
    binpath = program_path(example)
    if binpath.exists():
        return binpath
    yaml = EXAMPLES / f"{example}.yaml"
    assert yaml.exists(), f"no such example: {yaml}"
    subprocess.run(
        [ESPHOME, "-s", "name_add_mac_suffix", "false", "compile", str(yaml)],
        check=True,
    )
    assert binpath.exists(), f"compile did not produce {binpath}"
    return binpath


class HostProcess:
    """A running host binary with a background stdout reader + log matchers."""

    def __init__(self, proc: subprocess.Popen):
        self.proc = proc
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()

    def _pump(self):
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\n"))

    def snapshot(self) -> list[str]:
        with self._lock:
            return list(self.lines)

    def wait_for_log(self, substr: str, timeout: float = 10.0) -> str | None:
        """Return the first log line containing substr within timeout, else None."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for ln in self.snapshot():
                if substr in ln:
                    return ln
            time.sleep(0.05)
        return None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)


# ---------------------------------------------------------------------------
# The org.bluez mock + injection helpers
# ---------------------------------------------------------------------------

class BluezMock:
    def __init__(self, mock: SpawnedMock):
        self.mock = mock
        self.bus = BusType.SYSTEM.get_connection()

    # -- adapter / managers ---------------------------------------------------
    def adapter(self):
        return self.bus.get_object(ORG_BLUEZ, ADAPTER_PATH)

    def root(self):
        return self.bus.get_object(ORG_BLUEZ, "/")

    def add_gatt_manager(self, register_error: str | None = None):
        """Add GattManager1 (the bluez5 template lacks it). register_error, if set,
        makes RegisterApplication raise that org.bluez.Error.* name."""
        code = ""
        if register_error:
            code = f'raise dbus.exceptions.DBusException("injected", name="{register_error}")'
        self.adapter().AddMethods(
            "org.bluez.GattManager1",
            [
                ("RegisterApplication", "oa{sv}", "", code),
                ("UnregisterApplication", "o", "", ""),
            ],
            interface_name=MOCK_IFACE,
        )

    def stub_advertising_manager(self):
        """Override LEAdvertisingManager1.Register/UnregisterAdvertisement with clean
        recorded no-ops. The bluez5 template ships its own, but its bookkeeping
        raises a KeyError in 0.31.1; we only need deterministic success here (on-air
        behavior is covered by the live btmon tier)."""
        for name, sig in (("RegisterAdvertisement", "oa{sv}"), ("UnregisterAdvertisement", "o")):
            self.adapter().AddMethod(
                "org.bluez.LEAdvertisingManager1", name, sig, "", "", interface_name=MOCK_IFACE
            )

    def set_register_application_error(self, error: str):
        """Force GattManager1.RegisterApplication to raise error (override the no-op)."""
        self.adapter().AddMethod(
            "org.bluez.GattManager1",
            "RegisterApplication",
            "oa{sv}",
            "",
            f'raise dbus.exceptions.DBusException("injected", name="{error}")',
            interface_name=MOCK_IFACE,
        )

    def kill_daemon(self):
        """Simulate bluetoothd vanishing: drop the org.bluez name owner. The private
        dbus-daemon stays up, so the host's bus stays connected but org.bluez calls
        start failing with NameHasNoOwner."""
        self.mock.terminate()

    def set_advertise_error(self, error: str):
        """Force LEAdvertisingManager1.RegisterAdvertisement to raise error."""
        self.adapter().AddMethod(
            "org.bluez.LEAdvertisingManager1",
            "RegisterAdvertisement",
            "oa{sv}",
            "",
            f'raise dbus.exceptions.DBusException("injected", name="{error}")',
            interface_name=MOCK_IFACE,
        )

    # -- call log -------------------------------------------------------------
    def calls(self, method: str) -> list:
        return self.adapter().GetMethodCalls(method, interface_name=MOCK_IFACE)

    def wait_for_call(self, method: str, timeout: float = 10.0) -> list:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            c = self.calls(method)
            if c:
                return c
            time.sleep(0.05)
        return []

    # -- device / advert injection -------------------------------------------
    def inject_device(self, addr: str, props: dict) -> str:
        """Create a Device1 (with container props like ServiceData) and emit
        InterfacesAdded so the host scanner's signal handler fires."""
        path = ADAPTER_PATH + "/dev_" + addr.replace(":", "_")
        full = {
            "Address": dbus.String(addr),
            "Adapter": dbus.ObjectPath(ADAPTER_PATH),
        }
        full.update(props)
        d = dbus.Dictionary(full, signature="sv")
        # AddObject is container-safe (UpdateProperties is not, in dbusmock 0.31).
        self.mock.obj.AddObject(
            path, DEV_IFACE, d, dbus.Array([], signature="(ssss)")
        )
        self.root().EmitSignal(
            OM_IFACE,
            "InterfacesAdded",
            "oa{sa{sv}}",
            [dbus.ObjectPath(path), dbus.Dictionary({DEV_IFACE: d}, signature="sa{sv}")],
            dbus_interface=MOCK_IFACE,
        )
        return path

    def inject_connectable_device(self, addr: str, props: dict | None = None) -> str:
        """Like inject_device, but the Device1 exposes Connect/Disconnect methods
        that flip its own Connected property (so a GATT client that calls Connect
        observes a real link coming up). Returns the device object path."""
        path = ADAPTER_PATH + "/dev_" + addr.replace(":", "_")
        full = {
            "Address": dbus.String(addr),
            "Adapter": dbus.ObjectPath(ADAPTER_PATH),
            "Connected": dbus.Boolean(False),
            "ServicesResolved": dbus.Boolean(False),
        }
        full.update(props or {})
        d = dbus.Dictionary(full, signature="sv")
        methods = dbus.Array(
            [
                ("Connect", "", "", 'self.Set("org.bluez.Device1", "Connected", True)'),
                ("Disconnect", "", "", 'self.Set("org.bluez.Device1", "Connected", False)'),
            ],
            signature="(ssss)",
        )
        self.mock.obj.AddObject(path, DEV_IFACE, d, methods)
        self.root().EmitSignal(
            OM_IFACE,
            "InterfacesAdded",
            "oa{sa{sv}}",
            [dbus.ObjectPath(path), dbus.Dictionary({DEV_IFACE: d}, signature="sa{sv}")],
            dbus_interface=MOCK_IFACE,
        )
        return path

    def device_calls(self, path: str, method: str) -> list:
        dev = self.bus.get_object(ORG_BLUEZ, path)
        return dev.GetMethodCalls(method, interface_name=MOCK_IFACE)

    def wait_for_device_call(self, path: str, method: str, timeout: float = 10.0) -> list:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                c = self.device_calls(path, method)
            except dbus.exceptions.DBusException:
                c = []
            if c:
                return c
            time.sleep(0.05)
        return []

    def update_device(self, path: str, props: dict):
        """Update scalar Device1 props (e.g. Connected) -> emits PropertiesChanged."""
        dev = self.bus.get_object(ORG_BLUEZ, path)
        dev.UpdateProperties(
            DEV_IFACE, dbus.Dictionary(props, signature="sv"), interface_name=MOCK_IFACE
        )

    def remove_adapter(self):
        self.mock.obj.RemoveAdapter("hci0")

    def set_powered(self, on: bool):
        self.adapter().UpdateProperties(
            "org.bluez.Adapter1",
            dbus.Dictionary({"Powered": dbus.Boolean(on)}, signature="sv"),
            interface_name=MOCK_IFACE,
        )

    # -- read an advertisement the host exported (its real getter runs) -------
    def read_advertisement(self, adv_path: str) -> dict | None:
        """Find whichever bus name owns adv_path and read its LEAdvertisement1
        properties — invokes the host's actual property getters over D-Bus."""
        dbus_obj = self.bus.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus")
        names = dbus_obj.ListNames(dbus_interface="org.freedesktop.DBus")
        for name in names:
            if not name.startswith(":"):
                continue
            try:
                obj = self.bus.get_object(name, adv_path)
                props = obj.GetAll(LE_ADV, dbus_interface=PROPS_IFACE)
                return dict(props)
            except dbus.exceptions.DBusException:
                continue
        return None

    def wait_for_advertisement(self, adv_path: str, timeout: float = 10.0) -> dict | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            p = self.read_advertisement(adv_path)
            if p is not None:
                return p
            time.sleep(0.05)
        return None


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def private_system_bus():
    with PrivateDBus(BusType.SYSTEM) as addr:
        yield addr


@pytest.fixture
def bluez(private_system_bus):
    """A fresh org.bluez mock with hci0 + GattManager1, on a private system bus."""
    mock = SpawnedMock.spawn_with_template("bluez5", {}, BusType.SYSTEM)
    try:
        mock.obj.AddAdapter("hci0", "mock-host")
        bz = BluezMock(mock)
        bz.add_gatt_manager()
        bz.stub_advertising_manager()
        yield bz
    finally:
        # A resilience test may have already killed the daemon; terminate is then a no-op.
        try:
            mock.terminate()
        except Exception:
            pass


@pytest.fixture
def run_host(bluez):
    """Factory: launch a compiled example binary against the mock bus."""
    started: list[HostProcess] = []

    def _run(example: str) -> HostProcess:
        binpath = ensure_built(example)
        # stdbuf -oL: the host logger block-buffers stdout when it's a pipe (not a
        # tty), so line-buffer it or wait_for_log would never see anything.
        proc = subprocess.Popen(
            ["stdbuf", "-oL", str(binpath)],
            env={**os.environ},  # carries DBUS_SYSTEM_BUS_ADDRESS to sd-bus
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        hp = HostProcess(proc)
        started.append(hp)
        return hp

    yield _run
    for hp in started:
        hp.stop()
