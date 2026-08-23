# esphome-host-linux

ESPHome external components that fill in the stubbed hardware HAL of ESPHome's `host` platform with Linux kernel interfaces, so the existing sensor and network catalog runs natively on a Raspberry Pi (or any Linux SBC).

## Why

ESPHome's `host` platform compiles configs to a native Linux binary but stubs out GPIO, I2C, SPI, UART, networking, and BLE. Roughly 300 sensor and display drivers are written against the platform-agnostic HAL (`I2CBus`, `SPIDelegate`, `InternalGPIOPin`, `UARTComponent`) and would Just Work if those surfaces were backed by Linux char devices. This project provides those backings, plus host shims for `web_server`, `mqtt`, and BLE so the upstream components run on host.

## Supported ESPHome versions

This repo targets **ESPHome 2026.8 or newer** (and current `dev`). Older releases are not supported: the BLE
components are built on the `ble_device_base` hub interface introduced in 2026.8, and the native host I2C the
examples use landed in 2026.6.

## What works

- **`linux_gpio`** -- GPIO via the kernel character-device v2 ABI (`<linux/gpio.h>`, **no external library**). Input, output, internal pulls, and edge interrupts. Per-pin `chip:` selects the gpiochip (defaults to `/dev/gpiochip0`); optional `debounce_us:` enables kernel-side debounce.
- **I2C** -- use upstream `i2c: { device: /dev/i2c-N }`; ESPHome **2026.6.0** added native host I2C ([#14489](https://github.com/esphome/esphome/pull/14489)). The old `linux_i2c` component was removed and now fails validation with the replacement snippet.
- **`linux_spi`** -- SPI via `/dev/spidev*` ioctls. Stock `spi:`-based sensors work (MAX31865, etc.). Upstream `spi:` has no host support, so this component is the only path. Upstream marks `spi::SPIComponent` `final`, so the build pre-script strips that keyword from the installed ESPHome copy before compiling.
- **`socketcan`** -- CAN bus via the kernel SocketCAN API (`<linux/can.h>`, no external library). A `canbus:` platform. Bring the interface up first with `ip link set canX up type can bitrate N` (the bitrate isn't settable from userspace); the compiled binary needs `cap_net_raw`.
- **`linux_sysfs_sensor`** -- publish any numeric sysfs attribute (SoC temperature, hwmon volts, IIO ADC) as a `sensor:` with an explicit `path:` + `scale:`. No auto-discovery -- `hwmonN`/`iio:deviceN` indices aren't stable across boots, so pin the exact path (check `cat /sys/class/hwmon/hwmon*/name` first).
- **`linux_w1`** -- 1-Wire via kernel `/sys/bus/w1/`. DS18B20 and friends.
- **`linux_time`** -- *(deprecated)* expose the kernel wall clock as a `time:` platform. Upstream `time: { platform: host }` does the same; `linux_time` still works and warns at build time.
- **UART** -- ESPHome's upstream `uart:` already handles `port: /dev/ttyXXX` on host.
- **`web_server`** -- upstream component's own source is used as-is; we ship a `web_server_base` shadow that provides an `AsyncWebServer` shim over POSIX sockets, plus a build pre-script that relaxes a few platform guards in the installed ESPHome copy. Basic auth (including the hashed form upstream now generates off-ESP32) and CORS work; digest auth is rejected at config time on host.
- **`mqtt`** -- upstream component's own source is used as-is (again with small pre-script guard relaxations); the host backend wraps libmosquitto.
- **BLE** -- the `esp32_ble_tracker` shadow is a real `ble_device_base` BLE hub for host, so stock BLE consumers (`ble_presence`, `ble_rssi`, `bthome_receiver`, `bluetooth_proxy`, the parsed `*_ble` sensors) register against it the same way they do on ESP32. Two backends: BlueZ over D-Bus (default; needs `bluetoothd`, no capabilities, coexists with other users of the adapter) and raw HCI sockets (`hci_backend: true`; byte-exact advertisements, needs `cap_net_admin,cap_net_raw` and an adapter not claimed by `bluetoothd`).

**Build patches:** the pre-script mentioned above (web_server guards, SPI `final`, mqtt host backend, plus
third-party fixes such as the inter-transaction delay ssieb's `seesaw` needs on the Pi) is registered by *every*
component in this repository, so any config that uses at least one of them gets the patches. A config that uses
**zero** repo components (e.g. only native `i2c:` plus third-party components like `seesaw`) never loads this
repository's Python and cannot be patched automatically -- add an explicit `host_patches:` block to opt in.

Tested on Pi 5 (Debian 13 trixie, kernel 6.12). CI also compiles every example on plain ubuntu-latest.

## Requirements

Pi OS based on Debian 13 (trixie), or any distro with Python >= 3.12 (ESPHome >= 2026.7 dropped
Python 3.11, so stock Bookworm/Debian 12 needs a newer Python first -- `pi-bootstrap.sh` checks and
tells you). The script handles the lot:

- `libmosquitto-dev`, `bluez`, `libcap2-bin`, `build-essential` (no lgpio needed -- `linux_gpio` talks to the kernel GPIO chardev directly)
- `libsystemd-dev` -- required for any BLE build: the D-Bus backend uses sd-bus, and it is compiled
  and linked even when the raw-HCI backend is selected at runtime.
- User in the `gpio`, `i2c`, `spi`, `dialout` groups
- ESPHome installed in a venv

For BLE, the compiled binary needs `cap_net_admin,cap_net_raw`. After every `esphome compile`:

```bash
scripts/pi-bless-binary.sh .esphome/build/<name>/.pioenvs/<name>/program
```

Pi 5 note: the GPIO header is on `/dev/gpiochip0` via the `pinctrl-rp1` driver (not `gpiochip4`, which some older guides cite).

## Over-the-air updates

ESPHome **2026.5** added native OTA on host: the `ota: - platform: esphome` block works, and the running process
stages the new binary, renames it over the running executable, and re-execs itself.

The catch is Linux file capabilities. They live on the executable's inode, so replacing the binary drops them: after
an OTA the raw-HCI BLE backend (and `socketcan`) will fail to open their sockets until `scripts/pi-bless-binary.sh`
is run against the new binary again. Two ways to avoid that:

- Use the BLE **D-Bus backend** (the default), which needs no capabilities at all.
- Run the binary from a systemd unit with ambient capabilities, which are granted per-process at start and so
  survive the swap:

  ```ini
  [Service]
  AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW
  CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_RAW
  ```

Host preferences also behave properly since **2026.7**, so `restore_value:` and friends persist across restarts.

## Usage

```yaml
external_components:
  - source:
      type: local
      path: components
    # or:
    # source: github://rjt-rockx/esphome-host-linux

esphome:
  name: pi-test

host:
linux_gpio:

api:
logger:

binary_sensor:
  - platform: gpio
    name: "Button"
    pin:
      number: 17
      chip: /dev/gpiochip0
      mode:
        input: true
        pullup: true
      inverted: true

switch:
  - platform: gpio
    name: "LED"
    pin: 27
```

See `examples/` for one runnable file per component.

## Pin numbering

BCM GPIO numbers (the same scheme `gpiozero` and `pinout.xyz` use), not physical header positions. GPIO17 is BCM 17, header pin 11.

## Development

The `Makefile` wraps the inner loop. Set `PI=user@host` once per shell.

```bash
export PI=pi@raspberrypi.local

make bootstrap                                  # apt deps + esphome venv + groups
make validate EXAMPLE=examples/01-gpio-button-led.yaml
make compile  EXAMPLE=examples/01-gpio-button-led.yaml
make run      EXAMPLE=examples/01-gpio-button-led.yaml
make logs
make info
make clean
make help
```

`make validate` runs locally on macOS (no Pi needed) and is the fastest feedback loop.

The repo and scripts are still named `pi-*` because the inner-loop tooling targets Pi conventions (`pi-bootstrap.sh`, `pi-info.sh`, `PI=user@host`). The components themselves are named `linux_*` because the implementations are Linux-generic.

## AI assistance

Substantial parts of this codebase were drafted with AI assistance (Claude Code). The author reviews, tests, and integrates every change on real Raspberry Pi hardware before it lands on `main`, but treat the resulting code with the usual scepticism you'd apply to any third-party component: read it before you ship it.

## License

MIT. See [`LICENSE`](LICENSE).
