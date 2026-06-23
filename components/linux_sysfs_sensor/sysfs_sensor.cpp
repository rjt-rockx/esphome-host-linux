#ifdef USE_HOST
#if defined(__linux__)

#include "sysfs_sensor.h"

#include "esphome/core/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace linux_sysfs_sensor {

static const char *const TAG = "linux_sysfs_sensor";

void LinuxSysfsSensor::setup() {
  // Probe readability at boot so config errors surface early rather than as
  // silent NANs at the first poll. O_NONBLOCK guards against a broken driver
  // wedging on open.
  int fd = ::open(this->path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    ESP_LOGE(TAG, "Cannot open %s: %s", this->path_.c_str(), strerror(errno));
    this->mark_failed();
    return;
  }
  ::close(fd);
}

void LinuxSysfsSensor::update() {
  // O_NONBLOCK + a single read: some hwmon drivers return EAGAIN indefinitely,
  // and a blocking read would stall loop() and hang the device. A sysfs
  // attribute is one short line, so 128 bytes is ample.
  int fd = ::open(this->path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    ESP_LOGW(TAG, "Cannot open %s: %s", this->path_.c_str(), strerror(errno));
    this->publish_state(NAN);
    return;
  }
  char buf[128];
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) {
    ESP_LOGW(TAG, "Failed to read a value from %s", this->path_.c_str());
    this->publish_state(NAN);
    return;
  }
  buf[n] = '\0';

  // sysfs attributes hold a single decimal integer (e.g. millidegrees C).
  // strtol skips leading whitespace and stops at the trailing newline.
  errno = 0;
  char *end = nullptr;
  long raw = strtol(buf, &end, 10);
  if (end == buf || errno != 0) {
    ESP_LOGW(TAG, "Failed to parse a value from %s", this->path_.c_str());
    this->publish_state(NAN);
    return;
  }
  this->publish_state(static_cast<float>(raw) * this->scale_);
}

void LinuxSysfsSensor::dump_config() {
  LOG_SENSOR("", "Linux Sysfs Sensor", this);
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Scale: %.6f", this->scale_);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace linux_sysfs_sensor
}  // namespace esphome

#endif  // defined(__linux__)
#endif  // USE_HOST
