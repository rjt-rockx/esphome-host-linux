#pragma once

#ifdef USE_HOST

#include <string>

#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace linux_spi {

class LinuxSPIDelegate : public spi::SPIDelegate {
 public:
  LinuxSPIDelegate(int fd, uint32_t data_rate, spi::SPIBitOrder bit_order, spi::SPIMode mode, GPIOPin *cs_pin);
  ~LinuxSPIDelegate() override = default;

  // begin/end_transaction use the base class behavior: it toggles cs_pin_ on
  // every transaction (no-op for NULL_PIN). On Pi the kernel already drives
  // CE0 for /dev/spidev0.0, so double-toggling a user-supplied cs_pin tied to
  // CE0 is harmless; a separate user CS pin works as software-managed CS.

  uint8_t transfer(uint8_t data) override;
  void transfer(uint8_t *ptr, size_t length) override;
  void transfer(const uint8_t *txbuf, uint8_t *rxbuf, size_t length) override;
  void write_array(const uint8_t *ptr, size_t length) override;
  void read_array(uint8_t *ptr, size_t length) override;
  void write16(uint16_t data) override;

 protected:
  void do_transfer_(const uint8_t *tx, uint8_t *rx, size_t length);
  int fd_{-1};
  uint8_t kernel_mode_{0};
  uint8_t bits_per_word_{8};
};

class LinuxSPIBus : public spi::SPIBus {
 public:
  explicit LinuxSPIBus(int fd) : spi::SPIBus(), fd_(fd) {}

  spi::SPIDelegate *get_delegate(uint32_t data_rate, spi::SPIBitOrder bit_order, spi::SPIMode mode, GPIOPin *cs_pin,
                                 bool release_device, bool write_only) override {
    return new LinuxSPIDelegate(this->fd_, data_rate, bit_order, mode, cs_pin);
  }

  bool is_hw() override { return true; }

 protected:
  int fd_{-1};
};

class LinuxSPIComponent : public spi::SPIComponent {
 public:
  void set_device(std::string device) { device_path_ = std::move(device); }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

 protected:
  std::string device_path_;
  int fd_{-1};
};

}  // namespace linux_spi
}  // namespace esphome

#endif  // USE_HOST
