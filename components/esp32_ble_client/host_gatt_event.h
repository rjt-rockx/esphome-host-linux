#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

// Portable event/result structs marshalled from the BLEGattHost sd-bus worker
// thread to the ESPHome main thread. No ESP-IDF types — the host GATT client is
// native BlueZ, not an emulation of the IDF event model.

#include <cstdint>
#include <memory>

namespace esphome {
namespace esp32_ble_client {

// Short-lived views handed to node hooks; valid only for the duration of the
// hook call (they point into the owning HostGattEvent::data), mirroring the
// lifetime of ESP-IDF's param->read.value / param->notify.value.
struct BLEReadResult {
  uint16_t handle;
  int status;  // esp_gatt_status_t value
  const uint8_t *data;
  uint16_t len;
};
struct BLENotifyEvent {
  uint16_t handle;
  const uint8_t *data;
  uint16_t len;
};

struct HostGattEvent {
  enum class Kind : uint8_t {
    CONNECTED,
    DISCONNECTED,
    SERVICES_DISCOVERED,
    READ_COMPLETE,
    WRITE_COMPLETE,
    DESC_READ,
    DESC_WRITE,
    NOTIFY,
    NOTIFY_REGISTERED,
    RSSI,
    PASSKEY_REQUEST,
    PASSKEY_NOTIFY,
    NUMERIC_COMPARE,
    PAIRING_COMPLETE,
  } kind;
  uint16_t handle{0};
  int status{0};                     // esp_gatt_status_t value
  std::unique_ptr<uint8_t[]> data;   // OWNED copy (copied off the sd_bus_message
  uint16_t len{0};                   // on the worker thread before it is freed)
  int8_t rssi{0};
  uint32_t passkey{0};
  int disc_reason{0};                // esp_gatt_status_t / conn-reason
  uint16_t mtu{23};                  // carried on SERVICES_DISCOVERED
  bool pairing_success{false};
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
