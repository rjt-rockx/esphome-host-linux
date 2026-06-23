#pragma once

#ifdef USE_HOST

#include "esphome/core/hal.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace esphome {
namespace linux_gpio {

// Chip fd cache: each /dev/gpiochipN is opened once and shared across pins on
// that chip. Small set -> linear scan, not std::map.
struct ChipEntry {
  std::string path;
  int fd{-1};
};

// One edge-sensitive line fd plus the ESPHome callback to fire on an event.
struct AlertEntry {
  int line_fd{-1};
  void (*func)(void *){nullptr};
  void *arg{nullptr};
};

// A single background thread polls every edge-sensitive line fd. Pins register
// and deregister by posting to a self-pipe that wakes the poll loop, so adds
// and removes never race with a blocking poll().
class GPIOAlertThread {
 public:
  static GPIOAlertThread &instance();

  void add(int line_fd, void (*func)(void *), void *arg);
  void remove(int line_fd);

 protected:
  GPIOAlertThread();
  ~GPIOAlertThread();

  void run_();
  void wake_();

  std::thread thread_;
  std::atomic<bool> stop_{false};
  int wake_rfd_{-1};
  int wake_wfd_{-1};
  std::mutex mu_;
  std::vector<AlertEntry> entries_;  // protected by mu_
};

// Drop-in replacement for the host/gpio.h HostGPIOPin stub: drives real Linux
// GPIO via the character-device v2 ABI (<linux/gpio.h>), no external library.
// Kernel 5.10+ required for v2 (all Pi OS Bookworm+ hardware satisfies this).
class LinuxGPIOPin : public InternalGPIOPin {
 public:
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_chip_path(std::string path) { this->chip_path_ = std::move(path); }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { this->flags_ = flags; }
  void set_debounce_us(uint32_t debounce_us) { this->debounce_us_ = debounce_us; }

  void setup() override;
  void pin_mode(gpio::Flags flags) override;
  bool digital_read() override;
  void digital_write(bool value) override;
  size_t dump_summary(char *buffer, size_t len) const override;
  void detach_interrupt() const override;
  ISRInternalGPIOPin to_isr() const override;
  uint8_t get_pin() const override { return this->pin_; }
  gpio::Flags get_flags() const override { return this->flags_; }
  bool is_inverted() const override { return this->inverted_; }

 protected:
  void attach_interrupt(void (*func)(void *), void *arg, gpio::InterruptType type) const override;

  // Opens /dev/gpiochipN, caching the fd globally.
  static int open_chip_(const std::string &path);
  // Maps ESPHome gpio::Flags to GPIO_V2_LINE_FLAG_* bits.
  static uint64_t build_line_flags_(gpio::Flags flags);
  // Claims (or in-place reconfigures) the line for the given flags.
  void claim_line_(gpio::Flags flags);

  uint8_t pin_{};
  std::string chip_path_;
  mutable int chip_fd_{-1};
  mutable int line_fd_{-1};  // per-pin fd from GPIO_V2_GET_LINE_IOCTL
  bool inverted_{};
  gpio::Flags flags_{};
  uint32_t debounce_us_{0};
  bool output_value_{false};  // last raw level driven, restored across reconfigure
};

}  // namespace linux_gpio
}  // namespace esphome

#endif  // USE_HOST
