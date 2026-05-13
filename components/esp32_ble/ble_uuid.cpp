#include "ble_uuid.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace esp32_ble {

// from_raw(string) — parse a hex-stringified UUID like "12345678-1234-1234-1234-123456789abc"
// or a length-based raw byte stream. We mirror what's needed by ble_presence /
// ble_rssi which only call from_raw(const char *) on already-parsed binary
// blobs.
ESPBTUUID ESPBTUUID::from_raw(const char *data, size_t length) {
  ESPBTUUID u;
  size_t n = length <= 16 ? length : 16;
  u.len_ = static_cast<uint8_t>(n);
  std::memcpy(u.raw_, data, n);
  return u;
}

std::string ESPBTUUID::to_string() const {
  char buf[64];
  if (this->len_ == 2) {
    std::snprintf(buf, sizeof(buf), "0x%04X", this->get_16bit());
  } else if (this->len_ == 4) {
    std::snprintf(buf, sizeof(buf), "0x%08X", this->get_32bit());
  } else {
    std::snprintf(buf, sizeof(buf),
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  this->raw_[0], this->raw_[1], this->raw_[2], this->raw_[3], this->raw_[4], this->raw_[5],
                  this->raw_[6], this->raw_[7], this->raw_[8], this->raw_[9], this->raw_[10], this->raw_[11],
                  this->raw_[12], this->raw_[13], this->raw_[14], this->raw_[15]);
  }
  return std::string(buf);
}

}  // namespace esp32_ble
}  // namespace esphome
