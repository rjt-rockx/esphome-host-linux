#ifdef USE_HOST

#include "linux_i2c.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace linux_i2c {

static const char *const TAG = "linux_i2c";

void LinuxI2CBus::setup() {
  this->fd_ = ::open(this->device_path_.c_str(), O_RDWR);
  if (this->fd_ < 0) {
    ESP_LOGE(TAG, "open(%s) failed: %s", this->device_path_.c_str(), strerror(errno));
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Opened %s (fd=%d)", this->device_path_.c_str(), this->fd_);
  if (this->scan_) {
    ESP_LOGV(TAG, "Scanning bus...");
    this->i2c_scan_();
  }
}

void LinuxI2CBus::dump_config() {
  ESP_LOGCONFIG(TAG, "Linux I2C Bus:");
  ESP_LOGCONFIG(TAG, "  Device: %s", this->device_path_.c_str());
  ESP_LOGCONFIG(TAG, "  Scan: %s", YESNO(this->scan_));
  if (this->scan_) {
    ESP_LOGI(TAG, "Results from i2c bus scan:");
    if (scan_results_.empty()) {
      ESP_LOGI(TAG, "Found no i2c devices!");
    } else {
      for (const auto &s : scan_results_) {
        if (s.second) {
          ESP_LOGI(TAG, "Found i2c device at address 0x%02X", s.first);
        } else {
          ESP_LOGE(TAG, "Unknown error at address 0x%02X", s.first);
        }
      }
    }
  }
}

i2c::ErrorCode LinuxI2CBus::write_readv(uint8_t address, const uint8_t *write_buffer, size_t write_count,
                                       uint8_t *read_buffer, size_t read_count) {
  if (this->fd_ < 0)
    return i2c::ERROR_NOT_INITIALIZED;

  struct i2c_msg msgs[2];
  unsigned int n = 0;

  if (write_count > 0 && write_buffer != nullptr) {
    msgs[n].addr = address;
    msgs[n].flags = 0;
    msgs[n].len = static_cast<uint16_t>(write_count);
    msgs[n].buf = const_cast<uint8_t *>(write_buffer);
    n++;
  }
  if (read_count > 0 && read_buffer != nullptr) {
    msgs[n].addr = address;
    msgs[n].flags = I2C_M_RD;
    msgs[n].len = static_cast<uint16_t>(read_count);
    msgs[n].buf = read_buffer;
    n++;
  }

  // Zero-length transactions (probe / scan): use a single empty write.
  if (n == 0) {
    msgs[0].addr = address;
    msgs[0].flags = 0;
    msgs[0].len = 0;
    msgs[0].buf = nullptr;
    n = 1;
  }

  struct i2c_rdwr_ioctl_data data {};
  data.msgs = msgs;
  data.nmsgs = n;

  if (::ioctl(this->fd_, I2C_RDWR, &data) < 0) {
    int err = errno;
    switch (err) {
      case ENXIO:
      case EREMOTEIO:
        return i2c::ERROR_NOT_ACKNOWLEDGED;
      case ETIMEDOUT:
        return i2c::ERROR_TIMEOUT;
      case EINVAL:
        return i2c::ERROR_INVALID_ARGUMENT;
      default:
        ESP_LOGVV(TAG, "ioctl(I2C_RDWR) addr=0x%02X failed: %s", address, strerror(err));
        return i2c::ERROR_UNKNOWN;
    }
  }
  return i2c::ERROR_OK;
}

}  // namespace linux_i2c
}  // namespace esphome

#endif  // USE_HOST
