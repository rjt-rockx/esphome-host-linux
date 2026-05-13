#ifdef USE_HOST

#include "gpio.h"

#include <lgpio.h>

#include <cstdlib>
#include <map>

#include "esphome/core/log.h"

namespace esphome {
namespace linux_gpio {

static const char *const TAG = "linux_gpio";

static std::map<std::string, int> &chip_handles() {
  static std::map<std::string, int> handles;
  return handles;
}

struct AlertContext {
  void (*user_func)(void *);
  void *user_arg;
};

// lgpio registers callbacks per-gpio via lgGpioSetAlertsFunc; once a pin is
// claimed for alerts, every edge event for that pin invokes this trampoline.
// We just forward to the ESPHome callback once per event.
static void alert_trampoline(int e_count, lgGpioAlert_p /*alerts*/, void *userdata) {
  auto *ctx = static_cast<AlertContext *>(userdata);
  if (ctx == nullptr || ctx->user_func == nullptr)
    return;
  for (int i = 0; i < e_count; i++) {
    ctx->user_func(ctx->user_arg);
  }
}

int LinuxGPIOPin::open_chip(const std::string &path) {
  auto &cache = chip_handles();
  auto it = cache.find(path);
  if (it != cache.end())
    return it->second;

  auto pos = path.find("gpiochip");
  if (pos == std::string::npos) {
    ESP_LOGE(TAG, "Invalid chip path '%s' (expected /dev/gpiochipN)", path.c_str());
    return -1;
  }
  int n = std::atoi(path.c_str() + pos + 8);
  int h = lgGpiochipOpen(n);
  if (h < 0) {
    ESP_LOGE(TAG, "lgGpiochipOpen(%d) failed: %d", n, h);
    return -1;
  }
  cache[path] = h;
  ESP_LOGI(TAG, "Opened %s (handle=%d)", path.c_str(), h);
  return h;
}

void LinuxGPIOPin::setup() { this->pin_mode(this->flags_); }

void LinuxGPIOPin::pin_mode(gpio::Flags flags) {
  if (this->chip_handle_ < 0)
    this->chip_handle_ = open_chip(this->chip_path_);
  if (this->chip_handle_ < 0)
    return;

  lgGpioFree(this->chip_handle_, this->pin_);

  int line_flags = 0;
  if (flags & gpio::FLAG_PULLUP)
    line_flags |= LG_SET_PULL_UP;
  else if (flags & gpio::FLAG_PULLDOWN)
    line_flags |= LG_SET_PULL_DOWN;
  if (flags & gpio::FLAG_OPEN_DRAIN)
    line_flags |= LG_SET_OPEN_DRAIN;

  int rc;
  if (flags & gpio::FLAG_OUTPUT) {
    rc = lgGpioClaimOutput(this->chip_handle_, line_flags, this->pin_, this->inverted_ ? 1 : 0);
  } else {
    rc = lgGpioClaimInput(this->chip_handle_, line_flags, this->pin_);
  }
  if (rc < 0) {
    ESP_LOGE(TAG, "Claim pin %u failed: %d", this->pin_, rc);
    return;
  }
  this->flags_ = flags;
}

bool LinuxGPIOPin::digital_read() {
  if (this->chip_handle_ < 0)
    return this->inverted_;
  int level = lgGpioRead(this->chip_handle_, this->pin_);
  if (level < 0) {
    ESP_LOGW(TAG, "Read pin %u failed: %d", this->pin_, level);
    return this->inverted_;
  }
  return bool(level) != this->inverted_;
}

void LinuxGPIOPin::digital_write(bool value) {
  if (this->chip_handle_ < 0)
    return;
  int rc = lgGpioWrite(this->chip_handle_, this->pin_, (value != this->inverted_) ? 1 : 0);
  if (rc < 0)
    ESP_LOGW(TAG, "Write pin %u failed: %d", this->pin_, rc);
}

size_t LinuxGPIOPin::dump_summary(char *buffer, size_t len) const {
  return snprintf(buffer, len, "GPIO%u (%s)", this->pin_, this->chip_path_.c_str());
}

// Edge-triggered interrupts via lgGpioClaimAlert. The user callback runs on
// lgpio's internal worker thread, NOT in an interrupt context. ESPHome's
// IRAM_ATTR / lock-free assumptions are not honored on this platform. For
// pulse_counter and binary_sensor edge dispatch this is fine.
void LinuxGPIOPin::attach_interrupt(void (*func)(void *), void *arg, gpio::InterruptType type) const {
  if (this->chip_handle_ < 0) {
    ESP_LOGW(TAG, "attach_interrupt: pin %u has no open chip", this->pin_);
    return;
  }

  int eFlags;
  switch (type) {
    case gpio::INTERRUPT_RISING_EDGE:
      eFlags = LG_RISING_EDGE;
      break;
    case gpio::INTERRUPT_FALLING_EDGE:
      eFlags = LG_FALLING_EDGE;
      break;
    case gpio::INTERRUPT_ANY_EDGE:
      eFlags = LG_BOTH_EDGES;
      break;
    default:
      ESP_LOGW(TAG, "attach_interrupt: level-triggered not supported on linux_gpio, using BOTH_EDGES");
      eFlags = LG_BOTH_EDGES;
      break;
  }

  int line_flags = 0;
  if (this->flags_ & gpio::FLAG_PULLUP)
    line_flags |= LG_SET_PULL_UP;
  else if (this->flags_ & gpio::FLAG_PULLDOWN)
    line_flags |= LG_SET_PULL_DOWN;

  lgGpioFree(this->chip_handle_, this->pin_);

  // Register the callback BEFORE claiming the pin for alerts; otherwise early
  // edge events between Claim and SetAlertsFunc would be dropped.
  auto *ctx = new AlertContext{func, arg};  // NOLINT(cppcoreguidelines-owning-memory)
  int rc = lgGpioSetAlertsFunc(this->chip_handle_, this->pin_, &alert_trampoline, ctx);
  if (rc < 0) {
    ESP_LOGE(TAG, "lgGpioSetAlertsFunc(pin=%u) failed: %d", this->pin_, rc);
    delete ctx;
    return;
  }

  rc = lgGpioClaimAlert(this->chip_handle_, line_flags, eFlags, this->pin_, -1);
  if (rc < 0) {
    ESP_LOGE(TAG, "lgGpioClaimAlert(pin=%u) failed: %d", this->pin_, rc);
    return;
  }
  ESP_LOGD(TAG, "Attached interrupt on pin %u (type=%d)", this->pin_, (int) type);
}

void LinuxGPIOPin::detach_interrupt() const {
  if (this->chip_handle_ < 0)
    return;
  // lgpio has no explicit "unregister alerts func"; freeing the line stops
  // events from being delivered. The AlertContext leaks here, which is fine
  // for our usage (interrupts are configured once at boot, not torn down).
  lgGpioFree(this->chip_handle_, this->pin_);
}

struct ISRPinArg {
  uint8_t pin;
  bool inverted;
};

ISRInternalGPIOPin LinuxGPIOPin::to_isr() const {
  auto *arg = new ISRPinArg{};  // NOLINT(cppcoreguidelines-owning-memory)
  arg->pin = pin_;
  arg->inverted = inverted_;
  return ISRInternalGPIOPin((void *) arg);
}

}  // namespace linux_gpio
}  // namespace esphome

#endif  // USE_HOST
