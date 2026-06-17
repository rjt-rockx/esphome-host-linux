#pragma once

// Host stand-in for esphome/components/esp32_ble/ble_uuid.h. Provides just
// enough of the ESPBTUUID API for ble_presence, ble_rssi, and other consumers
// of esp32_ble_tracker to compile and operate on Linux.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>

namespace esphome {
namespace esp32_ble {

// Minimal stand-in for ESP-IDF's esp_bt_uuid_t, so consumers (e.g. thermopro_ble)
// that call ESPBTUUID::get_uuid() and read uuid.uuid.uuid16 compile on host.
constexpr uint16_t ESP_UUID_LEN_16 = 2;
constexpr uint16_t ESP_UUID_LEN_32 = 4;
constexpr uint16_t ESP_UUID_LEN_128 = 16;

struct esp_bt_uuid_t {
  uint16_t len;
  union {
    uint16_t uuid16;
    uint32_t uuid32;
    uint8_t uuid128[16];
  } uuid;
};

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
  // Parse a canonical UUID string as BlueZ reports it, e.g.
  // "0000fdf7-0000-1000-8000-00805f9b34fb" (always 128-bit form). Stored
  // reversed (LSB-first), matching from_raw_reversed / the as_reversed_hex_array
  // codegen path so service-UUID comparisons line up.
  static ESPBTUUID from_uuid_str(const char *s);
  static ESPBTUUID from_uuid_str(const std::string &s) { return from_uuid_str(s.c_str()); }
  static ESPBTUUID from_raw(const std::string &s) { return from_raw(s.c_str(), s.size()); }
  // Build from the esp_bt_uuid_t union form (GATT server / advertising codegen).
  static ESPBTUUID from_uuid(const esp_bt_uuid_t &uuid) {
    if (uuid.len == ESP_UUID_LEN_16)
      return from_uint16(uuid.uuid.uuid16);
    if (uuid.len == ESP_UUID_LEN_32)
      return from_uint32(uuid.uuid.uuid32);
    return from_raw(uuid.uuid.uuid128);  // already LSB-first like raw_
  }
  static ESPBTUUID from_raw(std::initializer_list<uint8_t> data) {
    ESPBTUUID u;
    u.len_ = static_cast<uint8_t>(data.size() <= 16 ? data.size() : 16);
    std::memcpy(u.raw_, data.begin(), u.len_);
    return u;
  }

  // Build an esp_bt_uuid_t view (used by parsers like thermopro_ble that read
  // .uuid.uuid16). raw_ is stored LSB-first, matching the union's native order.
  esp_bt_uuid_t get_uuid() const {
    esp_bt_uuid_t u{};
    u.len = this->len_;
    if (this->len_ == ESP_UUID_LEN_16) {
      u.uuid.uuid16 = this->get_16bit();
    } else if (this->len_ == ESP_UUID_LEN_32) {
      u.uuid.uuid32 = this->get_32bit();
    } else {
      std::memcpy(u.uuid.uuid128, this->raw_, 16);
    }
    return u;
  }

  // Expand to the full 128-bit form using the Bluetooth Base UUID
  // (0000xxxx-0000-1000-8000-00805F9B34FB) for 16/32-bit UUIDs. Result raw_ is
  // LSB-first (matching from_raw). Used by GATT-server advertising + UUID blobs.
  ESPBTUUID as_128bit() const {
    if (this->len_ == ESP_UUID_LEN_128)
      return *this;
    // Base UUID in LSB-first byte order (reverse of 00000000-0000-1000-8000-00805F9B34FB):
    static const uint8_t base_lsb[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                         0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ESPBTUUID u;
    u.len_ = 16;
    std::memcpy(u.raw_, base_lsb, 16);
    // The 16/32-bit value occupies bytes 12..15 (the big-endian high word),
    // which in LSB-first storage are raw_[12..15] = value little-endian.
    uint32_t v = (this->len_ == ESP_UUID_LEN_16) ? this->get_16bit() : this->get_32bit();
    u.raw_[12] = v & 0xff;
    u.raw_[13] = (v >> 8) & 0xff;
    u.raw_[14] = (v >> 16) & 0xff;
    u.raw_[15] = (v >> 24) & 0xff;
    return u;
  }

  // Canonical lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into a caller
  // buffer (no heap). Used to emit the BlueZ UUID property string.
  static constexpr size_t UUID_STR_LEN = 37;  // 36 chars + NUL
  const char *to_str(std::span<char, UUID_STR_LEN> output) const {
    ESPBTUUID full = this->as_128bit();
    // full.raw_ is LSB-first; canonical string is big-endian (raw_[15] first).
    const uint8_t *r = full.raw_;
    std::snprintf(output.data(), output.size(),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[15], r[14], r[13],
                  r[12], r[11], r[10], r[9], r[8], r[7], r[6], r[5], r[4], r[3], r[2], r[1], r[0]);
    return output.data();
  }

  uint8_t length() const { return this->len_; }
  uint16_t get_16bit() const { return static_cast<uint16_t>(raw_[0]) | (static_cast<uint16_t>(raw_[1]) << 8); }
  uint32_t get_32bit() const {
    return static_cast<uint32_t>(raw_[0]) | (static_cast<uint32_t>(raw_[1]) << 8) |
           (static_cast<uint32_t>(raw_[2]) << 16) | (static_cast<uint32_t>(raw_[3]) << 24);
  }
  const uint8_t *raw() const { return this->raw_; }

  // Returns true if the byte pair (data1, data2) appears contiguously in the
  // UUID, little-endian. Mirrors upstream ESPBTUUID::contains, used by service-
  // data parsers (atc/pvvx/ruuvi) to match e.g. 0x181A regardless of length.
  bool contains(uint8_t data1, uint8_t data2) const {
    if (this->len_ == 2) {
      return this->raw_[0] == data1 && this->raw_[1] == data2;
    }
    if (this->len_ < 2)
      return false;
    for (uint8_t i = 0; i + 1 < this->len_; i++) {
      if (this->raw_[i] == data1 && this->raw_[i + 1] == data2)
        return true;
    }
    return false;
  }

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
