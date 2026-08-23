# esphome-host-linux

ESPHome external components that fill in the stubbed hardware HAL of ESPHome's `host` platform with Linux kernel interfaces, so the existing sensor and network catalog runs natively on a Raspberry Pi (or any Linux SBC).

Aligned with **ESPHome 2026.8.x**. Requires ESPHome ≥ 2026.8.0.

## Why

ESPHome's `host` platform compiles configs to a native Linux binary but stubs out GPIO, SPI, 1-Wire, CAN, and BLE. Roughly 300 sensor and display drivers are written against the platform-agnostic HAL and would Just Work if those surfaces were backed by Linux char devices. This project provides those backings, plus host shims for `web_server` and `mqtt`, and a BlueZ/HCI `esp32_ble_tracker` that registers as the host `BLEHub` so stock 2026.8 BLE sensors work unmodified.

## What upstream now covers (do not shadow)

| Area | Since | Upstream |
|---|---|---|
| **I2C** | 2026.6.0 (#14489) | Stock `i2c: { device: /dev/i2c-N }` — `linux_i2c` removed from this repo |
| **UART** | earlier | Stock `uart: { port: /dev/tty… }` |
| **Time** | earlier | Stock `time: { platform: host }` — prefer this over any local clock shim |
| **BLE sensor platforms** | 2026.8.0 (#17150+) | Stock `ble_presence` / `ble_rssi` / BTHome / Xiaomi / … bind via `ble_hub_id` to any `BLEHub` — we only supply the Linux hub |
| **ccache for host** | 2026.8.0 (#17728) | Automatic when `ccache` is on `PATH` |

## What this repo still owns (Linux-specific HAL)

- **`linux_gpio`** — GPIO via the kernel character-device v2 ABI (`<linux/gpio.h>`, **no external library**). Input, output, internal pulls, and edge interrupts. Per-pin `chip:` selects the gpiochip (defaults to `/dev/gpiochip0`); optional `debounce_us:` enables kernel-side debounce.
- **`linux_spi`** — SPI via `/dev/spidev*` ioctls. Upstream `spi:` has no host support.
- **`socketcan`** — CAN via the kernel SocketCAN API. Bring the interface up first (`ip link set canX up type can bitrate N`); binary needs `cap_net_raw`.
- **`linux_sysfs_sensor`** — publish any numeric sysfs attribute as a `sensor:` (`path:` + `scale:`).
- **`linux_w1`** — 1-Wire via kernel `/sys/bus/w1/`.
- **`linux_compat`** — opt-in linker shims (e.g. MCP23xxx vtable instantiations on host).
- **`web_server_base`** — AsyncWebServer shim over POSIX sockets so stock `web_server:` runs on host (auth setters aligned with 2026.8 `#18237`).
- **`mqtt`** — host backend wrapping libmosquitto.
- **`esp32_ble` / `esp32_ble_tracker` / `esp32_ble_client` / `esp32_ble_server` / `esp32_ble_beacon` / `ble_client` / `bluetooth_proxy` / `bthome_advertiser`** — Linux BlueZ/HCI HAL. Upstream 2026.8's platform-neutral BLE layer still has **no host tracker** (hubs: esp32, bk72xx, ln882x, rp2 only). Our tracker registers as the host `BLEHub` so stock advertisement sensors compile and run.

Tested on Pi 5 (Debian 13 trixie, kernel 6.12). CI compiles every example on plain ubuntu-latest against ESPHome 2026.8.

## Requirements

Pi OS Bookworm, Debian 12, or any distro with the same toolchain. The `scripts/pi-bootstrap.sh` script handles the lot:

- `libmosquitto-dev`, `bluez`, `libcap2-bin`, `build-essential` (no lgpio needed — `linux_gpio` talks to the kernel GPIO chardev directly)
- User in the `gpio`, `i2c`, `spi`, `dialout` groups
- ESPHome ≥ 2026.8.0 installed in a venv

For BLE, the compiled binary needs `cap_net_admin,cap_net_raw` when using the raw HCI backend. After every `esphome compile`:

```bash
scripts/pi-bless-binary.sh .esphome/build/<name>/.pioenvs/<name>/program
```

Pi 5 note: the GPIO header is on `/dev/gpiochip0` via the `pinctrl-rp1` driver (not `gpiochip4`, which some older guides cite).

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

I2C (stock upstream since 2026.6):

```yaml
i2c:
  device: /dev/i2c-1
  scan: true
```

BLE (stock sensors + our Linux hub):

```yaml
esp32_ble_tracker:
  hci_device: hci0

binary_sensor:
  - platform: ble_presence
    mac_address: AA:BB:CC:DD:EE:FF
```

Explicit `esp32_ble_id:` on sensors is renamed to `ble_hub_id:` in 2026.8 (warns until 2027.2.0). Most configs never set it.

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

`make validate` runs locally (no Pi needed) and is the fastest feedback loop.

The repo and scripts are still named `pi-*` because the inner-loop tooling targets Pi conventions (`pi-bootstrap.sh`, `pi-info.sh`, `PI=user@host`). The components themselves are named `linux_*` because the implementations are Linux-generic.

## AI assistance

Substantial parts of this codebase were drafted with AI assistance (Claude Code). The author reviews, tests, and integrates every change on real Raspberry Pi hardware before it lands on `main`, but treat the resulting code with the usual scepticism you'd apply to any third-party component: read it before you ship it.

## License

MIT. See [`LICENSE`](LICENSE).
