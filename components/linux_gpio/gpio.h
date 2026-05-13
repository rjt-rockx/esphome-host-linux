#pragma once

#ifdef USE_HOST

#include <string>

#include "esphome/core/hal.h"

namespace esphome {
namespace linux_gpio {

class LinuxGPIOPin : public InternalGPIOPin {
 public:
  void set_pin(uint8_t pin) { pin_ = pin; }
  void set_chip_path(std::string path) { chip_path_ = std::move(path); }
  void set_inverted(bool inverted) { inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { flags_ = flags; }

  void setup() override;
  void pin_mode(gpio::Flags flags) override;
  bool digital_read() override;
  void digital_write(bool value) override;
  size_t dump_summary(char *buffer, size_t len) const override;
  void detach_interrupt() const override;
  ISRInternalGPIOPin to_isr() const override;
  uint8_t get_pin() const override { return pin_; }
  gpio::Flags get_flags() const override { return flags_; }
  bool is_inverted() const override { return inverted_; }

 protected:
  void attach_interrupt(void (*func)(void *), void *arg, gpio::InterruptType type) const override;

  static int open_chip(const std::string &path);

  uint8_t pin_{};
  std::string chip_path_;
  int chip_handle_{-1};
  bool inverted_{};
  gpio::Flags flags_{};
};

}  // namespace linux_gpio
}  // namespace esphome

#endif  // USE_HOST
