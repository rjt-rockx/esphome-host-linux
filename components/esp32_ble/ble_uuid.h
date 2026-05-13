#pragma once

// Host stand-in for esphome/components/esp32_ble/ble_uuid.h. Provides just
// enough of the ESPBTUUID API for ble_presence, ble_rssi, and other consumers
// of esp32_ble_tracker to compile and operate on Linux.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace esphome {
namespace esp32_ble {

class ESPBTUUID {
 public:
  ESPBTUUID() : len_(0) { std::memset(this->raw_, 0, sizeof(this->raw_)); }

  static ESPBTUUID from_uint16(uint16_t v) {
    ESPBTUUID u;
    u.len_ = 2;
    u.raw_[0] = v & 0xff;
    u.raw_[1] = (v >> 8) & 0xff;
    return u;
  }
  static ESPBTUUID from_uint32(uint32_t v) {
    ESPBTUUID u;
    u.len_ = 4;
    u.raw_[0] = v & 0xff;
    u.raw_[1] = (v >> 8) & 0xff;
    u.raw_[2] = (v >> 16) & 0xff;
    u.raw_[3] = (v >> 24) & 0xff;
    return u;
  }
  static ESPBTUUID from_raw(const uint8_t *data) {
    ESPBTUUID u;
    u.len_ = 16;
    std::memcpy(u.raw_, data, 16);
    return u;
  }
  static ESPBTUUID from_raw_reversed(const uint8_t *data) {
    ESPBTUUID u;
    u.len_ = 16;
    for (int i = 0; i < 16; i++)
      u.raw_[i] = data[15 - i];
    return u;
  }
  static ESPBTUUID from_raw(const char *data, size_t length);
  static ESPBTUUID from_raw(const char *data) { return from_raw(data, std::strlen(data)); }
  static ESPBTUUID from_raw(const std::string &s) { return from_raw(s.c_str(), s.size()); }
  static ESPBTUUID from_raw(std::initializer_list<uint8_t> data) {
    ESPBTUUID u;
    u.len_ = static_cast<uint8_t>(data.size() <= 16 ? data.size() : 16);
    std::memcpy(u.raw_, data.begin(), u.len_);
    return u;
  }

  uint8_t length() const { return this->len_; }
  uint16_t get_16bit() const { return static_cast<uint16_t>(raw_[0]) | (static_cast<uint16_t>(raw_[1]) << 8); }
  uint32_t get_32bit() const {
    return static_cast<uint32_t>(raw_[0]) | (static_cast<uint32_t>(raw_[1]) << 8) |
           (static_cast<uint32_t>(raw_[2]) << 16) | (static_cast<uint32_t>(raw_[3]) << 24);
  }
  const uint8_t *raw() const { return this->raw_; }

  bool operator==(const ESPBTUUID &other) const {
    if (this->len_ != other.len_)
      return false;
    return std::memcmp(this->raw_, other.raw_, this->len_) == 0;
  }
  bool operator!=(const ESPBTUUID &other) const { return !(*this == other); }

  std::string to_string() const;

 protected:
  uint8_t raw_[16];
  uint8_t len_;
};

}  // namespace esp32_ble
}  // namespace esphome
