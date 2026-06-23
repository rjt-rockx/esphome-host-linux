#ifdef USE_HOST
#if defined(__linux__)

#include "gpio.h"

#include "esphome/core/log.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace linux_gpio {

static const char *const TAG = "linux_gpio";

// Global chip fd cache -- linear scan over a small vector, not std::map.
static std::vector<ChipEntry> &chip_cache_() {
  static std::vector<ChipEntry> cache;
  return cache;
}

// Fills a (zero-initialized) line config for the single requested line: the
// flags, the driven output level (when output), and an optional kernel-side
// debounce. The OUTPUT_VALUES attribute is mandatory on every (re)configure of
// an output line -- without it a SET_CONFIG/GET_LINE reverts the line to the
// kernel default, glitching the output low between configs.
static void apply_line_config_(struct gpio_v2_line_config &cfg, uint64_t flags, uint32_t debounce_us,
                               bool output_value) {
  cfg.flags = flags;
  unsigned n = 0;
  if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
    cfg.attrs[n].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    cfg.attrs[n].attr.values = output_value ? 1ULL : 0ULL;
    cfg.attrs[n].mask = 1ULL;  // bit 0 -> offsets[0]
    n++;
  }
  if (debounce_us > 0) {
    cfg.attrs[n].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
    cfg.attrs[n].attr.debounce_period_us = debounce_us;
    cfg.attrs[n].mask = 1ULL;  // bit 0 -> offsets[0]
    n++;
  }
  cfg.num_attrs = n;
}

int LinuxGPIOPin::open_chip_(const std::string &path) {
  for (const auto &entry : chip_cache_()) {
    if (entry.path == path)
      return entry.fd;
  }
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (errno == EACCES) {
      ESP_LOGE(TAG, "open(%s) permission denied (add user to the 'gpio' group)", path.c_str());
    } else {
      ESP_LOGE(TAG, "open(%s) failed: %s", path.c_str(), strerror(errno));
    }
    return -1;
  }
  chip_cache_().push_back({path, fd});
  ESP_LOGD(TAG, "Opened %s (fd=%d)", path.c_str(), fd);
  return fd;
}

uint64_t LinuxGPIOPin::build_line_flags_(gpio::Flags flags) {
  uint64_t lf = 0;
  if (flags & gpio::FLAG_INPUT)
    lf |= GPIO_V2_LINE_FLAG_INPUT;
  if (flags & gpio::FLAG_OUTPUT)
    lf |= GPIO_V2_LINE_FLAG_OUTPUT;
  if (flags & gpio::FLAG_PULLUP)
    lf |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
  if (flags & gpio::FLAG_PULLDOWN)
    lf |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
  if (flags & gpio::FLAG_OPEN_DRAIN)
    lf |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
  return lf;
}

void LinuxGPIOPin::claim_line_(gpio::Flags flags) {
  if (this->chip_fd_ < 0)
    this->chip_fd_ = open_chip_(this->chip_path_);
  if (this->chip_fd_ < 0)
    return;

  // Already claimed: reconfigure in place, falling back to re-request.
  if (this->line_fd_ >= 0) {
    struct gpio_v2_line_config cfg{};
    apply_line_config_(cfg, build_line_flags_(flags), this->debounce_us_, this->output_value_);
    if (ioctl(this->line_fd_, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) == 0) {
      this->flags_ = flags;
      return;
    }
    // Reconfigure failed (e.g. transitioning to/from edge-detect); fall through
    // to close + re-request.
    ::close(this->line_fd_);
    this->line_fd_ = -1;
  }

  struct gpio_v2_line_request req{};
  req.offsets[0] = this->pin_;
  req.num_lines = 1;
  apply_line_config_(req.config, build_line_flags_(flags), this->debounce_us_, this->output_value_);
  strncpy(req.consumer, "esphome", sizeof(req.consumer) - 1);

  if (ioctl(this->chip_fd_, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
    ESP_LOGE(TAG, "GPIO_V2_GET_LINE_IOCTL pin=%u failed: %s", this->pin_, strerror(errno));
    return;
  }
  this->line_fd_ = req.fd;
  this->flags_ = flags;
}

void LinuxGPIOPin::setup() { this->pin_mode(this->flags_); }

void LinuxGPIOPin::pin_mode(gpio::Flags flags) { this->claim_line_(flags); }

bool LinuxGPIOPin::digital_read() {
  if (this->line_fd_ < 0)
    return this->inverted_;

  struct gpio_v2_line_values vals{};
  vals.mask = 1;  // bit 0 = line index 0 of our single-line request
  if (ioctl(this->line_fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
    ESP_LOGW(TAG, "GET_VALUES pin=%u failed: %s", this->pin_, strerror(errno));
    return this->inverted_;
  }
  return bool(vals.bits & 1) != this->inverted_;
}

void LinuxGPIOPin::digital_write(bool value) {
  if (this->line_fd_ < 0)
    return;

  // Remember the raw (post-inversion) level so a later reconfigure restores it
  // via the OUTPUT_VALUES attr instead of glitching to the kernel default.
  this->output_value_ = (value != this->inverted_);

  struct gpio_v2_line_values vals{};
  vals.mask = 1;
  vals.bits = this->output_value_ ? 1 : 0;
  if (ioctl(this->line_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0)
    ESP_LOGW(TAG, "SET_VALUES pin=%u failed: %s", this->pin_, strerror(errno));
}

void LinuxGPIOPin::attach_interrupt(void (*func)(void *), void *arg, gpio::InterruptType type) const {
  if (this->chip_fd_ < 0)
    this->chip_fd_ = open_chip_(this->chip_path_);
  if (this->chip_fd_ < 0) {
    ESP_LOGW(TAG, "attach_interrupt: pin %u has no open chip", this->pin_);
    return;
  }

  // Release any existing I/O claim before re-requesting the line with edge flags.
  if (this->line_fd_ >= 0) {
    GPIOAlertThread::instance().remove(this->line_fd_);
    ::close(this->line_fd_);
    this->line_fd_ = -1;
  }

  // Edge detection requires an input line; drop any output bit from the
  // pin's configured flags before adding edge flags.
  uint64_t lf = build_line_flags_(this->flags_);
  lf &= ~GPIO_V2_LINE_FLAG_OUTPUT;
  lf |= GPIO_V2_LINE_FLAG_INPUT;
  switch (type) {
    case gpio::INTERRUPT_RISING_EDGE:
      lf |= GPIO_V2_LINE_FLAG_EDGE_RISING;
      break;
    case gpio::INTERRUPT_FALLING_EDGE:
      lf |= GPIO_V2_LINE_FLAG_EDGE_FALLING;
      break;
    case gpio::INTERRUPT_ANY_EDGE:
      lf |= GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
      break;
    default:
      // The GPIO character device has no level-triggered mode; use both edges.
      lf |= GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
      ESP_LOGW(TAG, "Level-triggered interrupt not supported on linux_gpio; using both edges");
      break;
  }

  struct gpio_v2_line_request req{};
  req.offsets[0] = this->pin_;
  req.num_lines = 1;
  // Deep event FIFO so bursts aren't dropped before the poll thread drains
  // them (kernel default is num_lines*16; honored only for edge requests).
  req.event_buffer_size = 1024;
  apply_line_config_(req.config, lf, this->debounce_us_, false);  // input line: no output value
  strncpy(req.consumer, "esphome-irq", sizeof(req.consumer) - 1);

  if (ioctl(this->chip_fd_, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
    ESP_LOGE(TAG, "GPIO_V2_GET_LINE_IOCTL (edge) pin=%u failed: %s", this->pin_, strerror(errno));
    return;
  }
  this->line_fd_ = req.fd;
  // Non-blocking so run_() can drain to EAGAIN without blocking.
  int fl = fcntl(this->line_fd_, F_GETFL, 0);
  fcntl(this->line_fd_, F_SETFL, fl | O_NONBLOCK);
  GPIOAlertThread::instance().add(this->line_fd_, func, arg);
  ESP_LOGD(TAG, "Attached interrupt on pin=%u (type=%d)", this->pin_, (int) type);
}

void LinuxGPIOPin::detach_interrupt() const {
  if (this->line_fd_ < 0)
    return;
  GPIOAlertThread::instance().remove(this->line_fd_);
  ::close(this->line_fd_);
  this->line_fd_ = -1;
}

size_t LinuxGPIOPin::dump_summary(char *buffer, size_t len) const {
  return snprintf(buffer, len, "GPIO%u (%s)", this->pin_, this->chip_path_.c_str());
}

struct ISRPinArg {
  uint8_t pin;
  bool inverted;
};

ISRInternalGPIOPin LinuxGPIOPin::to_isr() const {
  // No ISR context on the host; the arg only carries pin/inverted for readback.
  auto *arg = new ISRPinArg{};  // NOLINT(cppcoreguidelines-owning-memory)
  arg->pin = this->pin_;
  arg->inverted = this->inverted_;
  return ISRInternalGPIOPin((void *) arg);
}

// --- GPIOAlertThread: one poll thread for every edge-sensitive line fd -------
GPIOAlertThread &GPIOAlertThread::instance() {
  static GPIOAlertThread inst;
  return inst;
}

GPIOAlertThread::GPIOAlertThread() {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
    ESP_LOGE(TAG, "pipe2 failed: %s", strerror(errno));
    return;
  }
  this->wake_rfd_ = fds[0];
  this->wake_wfd_ = fds[1];
  this->thread_ = std::thread([this] { this->run_(); });
}

GPIOAlertThread::~GPIOAlertThread() {
  this->stop_ = true;
  this->wake_();
  if (this->thread_.joinable())
    this->thread_.join();
  if (this->wake_rfd_ >= 0)
    ::close(this->wake_rfd_);
  if (this->wake_wfd_ >= 0)
    ::close(this->wake_wfd_);
}

void GPIOAlertThread::add(int line_fd, void (*func)(void *), void *arg) {
  {
    std::lock_guard<std::mutex> lock(this->mu_);
    this->entries_.push_back({line_fd, func, arg});
  }
  this->wake_();
}

void GPIOAlertThread::remove(int line_fd) {
  {
    std::lock_guard<std::mutex> lock(this->mu_);
    this->entries_.erase(std::remove_if(this->entries_.begin(), this->entries_.end(),
                                        [line_fd](const AlertEntry &e) { return e.line_fd == line_fd; }),
                         this->entries_.end());
  }
  this->wake_();
}

void GPIOAlertThread::wake_() {
  if (this->wake_wfd_ >= 0) {
    uint8_t b = 1;
    (void) write(this->wake_wfd_, &b, 1);
  }
}

void GPIOAlertThread::run_() {
  std::vector<struct pollfd> pfds;
  std::vector<AlertEntry> snapshot;

  while (!this->stop_) {
    // Snapshot under the lock so poll() runs without holding it.
    {
      std::lock_guard<std::mutex> lock(this->mu_);
      snapshot = this->entries_;
    }

    pfds.clear();
    pfds.push_back({this->wake_rfd_, POLLIN, 0});
    for (const auto &e : snapshot)
      pfds.push_back({e.line_fd, POLLIN, 0});

    int ret = poll(pfds.data(), pfds.size(), -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      ESP_LOGE(TAG, "poll() failed: %s", strerror(errno));
      break;
    }

    if (pfds[0].revents & POLLIN) {
      uint8_t buf[16];
      (void) read(this->wake_rfd_, buf, sizeof(buf));
    }

    for (size_t i = 1; i < pfds.size(); i++) {
      if (!(pfds[i].revents & POLLIN))
        continue;
      // Drain the whole FIFO per wakeup: reading one event per poll round lets
      // bursts overflow the kernel FIFO and drop edges. fd is non-blocking.
      struct gpio_v2_line_event evs[16];
      ssize_t n;
      while ((n = read(pfds[i].fd, evs, sizeof(evs))) > 0) {
        size_t count = n / sizeof(evs[0]);
        for (size_t k = 0; k < count; k++) {
          if (snapshot[i - 1].func != nullptr)
            snapshot[i - 1].func(snapshot[i - 1].arg);
        }
        if (count < sizeof(evs) / sizeof(evs[0]))
          break;  // partial batch -> FIFO drained
      }
    }
  }
}

}  // namespace linux_gpio
}  // namespace esphome

#endif  // defined(__linux__)
#endif  // USE_HOST
