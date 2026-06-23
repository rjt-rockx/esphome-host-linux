#include "ble_uuid.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace esp32_ble {

// Build from up to 16 already-binary bytes (no parsing); excess is truncated.
ESPBTUUID ESPBTUUID::from_raw(const char *data, size_t length) {
  ESPBTUUID u;
  size_t n = length <= 16 ? length : 16;
  u.len_ = static_cast<uint8_t>(n);
  std::memcpy(u.raw_, data, n);
  return u;
}

ESPBTUUID ESPBTUUID::from_uuid_str(const char *s) {
  // Collect hex nibbles, ignoring dashes. Expect 32 hex chars (128-bit).
  uint8_t bytes[16];
  int nbytes = 0;
  int hi = -1;
  for (const char *p = s; *p != '\0' && nbytes < 16; p++) {
    char c = *p;
    if (c == '-')
      continue;
    int nib;
    if (c >= '0' && c <= '9')
      nib = c - '0';
    else if (c >= 'a' && c <= 'f')
      nib = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      nib = c - 'A' + 10;
    else
      break;  // unexpected char
    if (hi < 0) {
      hi = nib;
    } else {
      bytes[nbytes++] = static_cast<uint8_t>((hi << 4) | nib);
      hi = -1;
    }
  }
  if (nbytes == 16)
    return from_raw_reversed(bytes);
  // Shorter/odd input: store what we have, big-endian, len = nbytes.
  ESPBTUUID u;
  u.len_ = static_cast<uint8_t>(nbytes);
  std::memcpy(u.raw_, bytes, nbytes);
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
