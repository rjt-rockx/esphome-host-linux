#ifdef USE_HOST

#include "linux_w1.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#include "esphome/core/log.h"

namespace esphome {
namespace linux_w1 {

static const char *const TAG = "linux_w1";

void LinuxW1Sensor::setup() {
  this->sysfs_path_ = "/sys/bus/w1/devices/" + this->address_ + "/temperature";
  std::ifstream probe(this->sysfs_path_);
  this->path_ok_ = probe.good();
  if (!this->path_ok_) {
    ESP_LOGW(TAG, "Sysfs path not readable: %s. Is w1-gpio loaded? Is the device present?",
             this->sysfs_path_.c_str());
  } else {
    ESP_LOGI(TAG, "Reading 1-Wire device at %s", this->sysfs_path_.c_str());
  }
}

void LinuxW1Sensor::update() {
  std::ifstream f(this->sysfs_path_);
  if (!f.is_open()) {
    ESP_LOGW(TAG, "Failed to open %s: %s", this->sysfs_path_.c_str(), strerror(errno));
    this->publish_state(NAN);
    this->status_set_warning();
    return;
  }
  std::string line;
  if (!std::getline(f, line) || line.empty()) {
    ESP_LOGW(TAG, "Empty read from %s", this->sysfs_path_.c_str());
    this->publish_state(NAN);
    this->status_set_warning();
    return;
  }
  // Kernel reports millidegrees Celsius as a signed integer. ESPHome compiles
  // without exceptions, so use strtol and check end-of-conversion + errno.
  errno = 0;
  char *endptr = nullptr;
  long milli = std::strtol(line.c_str(), &endptr, 10);
  if (errno != 0 || endptr == line.c_str()) {
    ESP_LOGW(TAG, "Could not parse '%s' from %s", line.c_str(), this->sysfs_path_.c_str());
    this->publish_state(NAN);
    this->status_set_warning();
    return;
  }
  // 85000 (=85.0 C) is the DS18B20 power-on default and almost always a bad read.
  if (milli == 85000) {
    ESP_LOGW(TAG, "Got 85.0 C from %s, treating as bad read", this->address_.c_str());
    this->publish_state(NAN);
    this->status_set_warning();
    return;
  }
  float temp_c = milli / 1000.0f;
  this->publish_state(temp_c);
  this->status_clear_warning();
}

void LinuxW1Sensor::dump_config() {
  ESP_LOGCONFIG(TAG, "1-Wire (kernel w1) Sensor:");
  ESP_LOGCONFIG(TAG, "  Address: %s", this->address_.c_str());
  ESP_LOGCONFIG(TAG, "  Sysfs path: %s", this->sysfs_path_.c_str());
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace linux_w1
}  // namespace esphome

#endif  // USE_HOST
