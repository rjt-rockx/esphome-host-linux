#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

// Portable event/result structs marshalled from the BLEGattHost sd-bus worker
// thread to the ESPHome main thread. No ESP-IDF types.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/esp32_ble/ble_uuid.h"

namespace esphome {
namespace esp32_ble_client {

// Use the concrete UUID type (esp32_ble::ESPBTUUID); the esp32_ble_tracker
// `espbt` alias resolves to the same type but isn't available from this header.
using BLEUUID = esphome::esp32_ble::ESPBTUUID;

// A snapshot of the discovered GATT tree, built by the worker from
// GetManagedObjects and carried on the SERVICES_DISCOVERED event. The main
// thread turns this into BLEService/BLECharacteristic/BLEDescriptor wrappers
// (so those stay main-thread-owned), while the worker keeps the handle→path
// maps it needs for read/write/notify routing.
struct DiscoveredDescriptor {
  BLEUUID uuid;
  uint16_t handle;
};
struct DiscoveredCharacteristic {
  BLEUUID uuid;
  uint16_t handle;
  uint8_t properties;  // esp_gatt_char_prop_t bitmask
  std::vector<DiscoveredDescriptor> descriptors;
};
struct DiscoveredService {
  BLEUUID uuid;
  uint16_t start_handle;
  uint16_t end_handle;
  std::vector<DiscoveredCharacteristic> characteristics;
};

// Short-lived views handed to node hooks; valid only for the duration of the
// hook call (they point into the owning HostGattEvent::data).
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
  // Populated only on SERVICES_DISCOVERED — the GATT tree snapshot the main
  // thread turns into wrapper objects.
  std::vector<DiscoveredService> services;
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
