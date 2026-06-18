#pragma once
// Host (Linux/BlueZ) shadow of the esp32_ble `ESP32BLE` component. Upstream this
// owns the IDF Bluedroid stack + BLEAdvertising; on host it is a thin advertising
// state holder. The GATT server (esp32_ble_server) and beacon (esp32_ble_beacon)
// are Parented<ESP32BLE> and drive advertising through the advertising_* methods
// below. The actual org.bluez LEAdvertisingManager1.RegisterAdvertisement is done
// by an AdvertisingBackend the GATT server registers — keeps esp32_ble free of any
// sd-bus dependency and of a hard link to esp32_ble_server.
#ifdef USE_HOST

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/core/component.h"

namespace esphome {
namespace esp32_ble {

// The advertising payload the server/beacon assemble. The backend translates this
// into a single org.bluez LEAdvertisement1 object's properties.
struct HostAdvertisement {
  std::vector<uint8_t> manufacturer_data;  // raw bytes; backend wraps per company below
  uint16_t manufacturer_company{0x0000};   // company id key for ManufacturerData (0 = none/use raw)
  std::vector<uint8_t> service_data;
  std::vector<ESPBTUUID> service_uuids;
  uint16_t appearance{0};
  std::string local_name;
  bool include_name{false};
  bool active{false};
};

// Implemented by BLEGattServer (or the beacon's own exporter). esp32_ble forwards
// advertising changes here; the backend registers/refreshes the LEAdvertisement1.
class AdvertisingBackend {
 public:
  virtual ~AdvertisingBackend() = default;
  virtual void on_advertising_changed(const HostAdvertisement &adv) = 0;
};

class ESP32BLE : public Component {
 public:
  void setup() override {}
  void loop() override {}
  void dump_config() override {}
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  // Advertising API — byte-compatible with the IDF ESP32BLE so the server/beacon
  // compile unchanged. Each mutator updates adv_ and pushes to the backend.
  void advertising_start();
  void advertising_set_service_data(const std::vector<uint8_t> &data);
  void advertising_set_manufacturer_data(const std::vector<uint8_t> &data);
  void advertising_set_appearance(uint16_t appearance) { this->adv_.appearance = appearance; }
  void advertising_set_service_data_and_name(std::span<const uint8_t> data, bool include_name);
  void advertising_add_service_uuid(ESPBTUUID uuid);
  void advertising_remove_service_uuid(ESPBTUUID uuid);
  void advertising_register_raw_advertisement_callback(std::function<void(bool)> &&callback);

  uint32_t get_advertising_cycle_time() const { return 0; }

  // Backend wiring (called by BLEGattServer::setup / the beacon).
  void set_advertising_backend(AdvertisingBackend *backend) { this->adv_backend_ = backend; }
  HostAdvertisement &advertisement() { return this->adv_; }

 protected:
  void push_advertising_();
  HostAdvertisement adv_;
  AdvertisingBackend *adv_backend_{nullptr};
  std::vector<std::function<void(bool)>> raw_adv_callbacks_;
};

}  // namespace esp32_ble
}  // namespace esphome

#endif  // USE_HOST
