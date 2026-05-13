# esphome-host-linux

ESPHome external components that fill in the stubbed hardware HAL of ESPHome's `host` platform with Linux kernel interfaces, so the existing sensor and network catalog runs natively on a Raspberry Pi (or any Linux SBC).

## Why

ESPHome's `host` platform compiles configs to a native Linux binary but stubs out GPIO, I2C, SPI, UART, networking, and BLE. Roughly 300 sensor and display drivers are written against the platform-agnostic HAL (`I2CBus`, `SPIDelegate`, `InternalGPIOPin`, `UARTComponent`) and would Just Work if those surfaces were backed by Linux char devices. This project provides those backings, plus host shims for `web_server`, `mqtt`, and `esp32_ble_tracker` so the upstream components run unmodified.

## What works

- **`linux_gpio`** -- GPIO via lgpio + `/dev/gpiochip*`. Input, output, internal pulls, edge interrupts.
- **`linux_i2c`** -- I2C via `/dev/i2c-N` ioctls. Stock `i2c:`-based sensors work unmodified (BME280, etc.).
- **`linux_spi`** -- SPI via `/dev/spidev*` ioctls. Stock `spi:`-based sensors work (MAX31865, etc.).
- **`linux_w1`** -- 1-Wire via kernel `/sys/bus/w1/`. DS18B20 and friends.
- **`linux_time`** -- expose the kernel wall clock as a `time:` platform.
- **UART** -- ESPHome's upstream `uart:` already handles `port: /dev/ttyXXX` on host.
- **`web_server`** -- upstream component runs unmodified; we ship a `web_server_base` shadow that provides an `AsyncWebServer` shim over POSIX sockets.
- **`mqtt`** -- upstream component runs unmodified; the host backend wraps libmosquitto.
- **`esp32_ble_tracker`** -- shadowed for host; backed by a Linux HCI raw socket scanner. Stock `ble_presence` / `ble_rssi` sensors work.

Tested on Pi 5 (Debian 13 trixie, kernel 6.12). CI also compiles every example on plain ubuntu-latest.

## Requirements

Pi OS Bookworm, Debian 12, or any distro with the same toolchain. The `scripts/pi-bootstrap.sh` script handles the lot:

- `liblgpio-dev`, `libmosquitto-dev`, `bluez`, `libcap2-bin`, `build-essential`
- User in the `gpio`, `i2c`, `spi`, `dialout` groups
- ESPHome installed in a venv

For BLE, the compiled binary needs `cap_net_admin,cap_net_raw`. After every `esphome compile`:

```bash
scripts/pi-bless-binary.sh .esphome/build/<name>/.pioenvs/<name>/program
```

Pi 5 note: the GPIO header is on `/dev/gpiochip0` via the `pinctrl-rp1` driver. The old "gpiochip4" advice is stale.

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
