#pragma once

// Host-side esp32_ble_tracker replacement: scans BLE advertisements via a
// Linux HCI raw socket and dispatches to registered listeners using the same
// API surface that upstream ble_presence / ble_rssi consume.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/esp32_ble/ble_uuid.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace esphome {
namespace esp32_ble_tracker {

// Re-export so downstream code can refer to esp32_ble_tracker::ESPBTUUID just
// like the upstream tracker does via `using namespace esp32_ble;`.
using ESPBTUUID = esp32_ble::ESPBTUUID;

struct ServiceData {
  ESPBTUUID uuid;
  std::vector<uint8_t> data;
};

class ESPBLEiBeacon {
 public:
  ESPBLEiBeacon() = default;
  explicit ESPBLEiBeacon(const uint8_t *data) { std::memcpy(&this->beacon_data_, data, sizeof(this->beacon_data_)); }
  static optional<ESPBLEiBeacon> from_manufacturer_data(const ServiceData &data);

  uint16_t get_major() const {
    return static_cast<uint16_t>((this->beacon_data_.major >> 8) | ((this->beacon_data_.major & 0xff) << 8));
  }
  uint16_t get_minor() const {
    return static_cast<uint16_t>((this->beacon_data_.minor >> 8) | ((this->beacon_data_.minor & 0xff) << 8));
  }
  int8_t get_signal_power() const { return this->beacon_data_.signal_power; }
  ESPBTUUID get_uuid() const { return ESPBTUUID::from_raw_reversed(this->beacon_data_.proximity_uuid); }

 protected:
  struct __attribute__((packed)) {
    uint8_t sub_type;
    uint8_t length;
    uint8_t proximity_uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t signal_power;
  } beacon_data_{};
};

class ESPBTDevice {
 public:
  void set_address(const uint8_t addr[6]) { std::memcpy(this->address_, addr, 6); }
  void set_rssi(int8_t rssi) { this->rssi_ = rssi; }
  void set_name(std::string name) { this->name_ = std::move(name); }
  void add_service_uuid(const ESPBTUUID &uuid) { this->service_uuids_.push_back(uuid); }
  void add_manufacturer_data(ServiceData sd) { this->manufacturer_datas_.push_back(std::move(sd)); }
  void add_service_data(ServiceData sd) { this->service_datas_.push_back(std::move(sd)); }
  void set_appearance(uint16_t a) { this->appearance_ = a; }
  void set_ad_flag(uint8_t f) { this->ad_flag_ = f; }
  void add_tx_power(int8_t p) { this->tx_powers_.push_back(p); }

  std::string address_str() const;
  uint64_t address_uint64() const;
  const uint8_t *address() const { return this->address_; }
  int get_rssi() const { return this->rssi_; }
  const std::string &get_name() const { return this->name_; }
  const std::vector<int8_t> &get_tx_powers() const { return this->tx_powers_; }
  const optional<uint16_t> &get_appearance() const { return this->appearance_; }
  const optional<uint8_t> &get_ad_flag() const { return this->ad_flag_; }
  const std::vector<ESPBTUUID> &get_service_uuids() const { return this->service_uuids_; }
  const std::vector<ServiceData> &get_manufacturer_datas() const { return this->manufacturer_datas_; }
  const std::vector<ServiceData> &get_service_datas() const { return this->service_datas_; }

  optional<ESPBLEiBeacon> get_ibeacon() const {
    for (const auto &it : this->manufacturer_datas_) {
      auto res = ESPBLEiBeacon::from_manufacturer_data(it);
      if (res.has_value())
        return res;
    }
    return {};
  }

  // Linux build does not implement Resolvable Private Address resolution
  // (would need AES-128 via mbedtls/OpenSSL). Always returns false; address
  // matching by MAC still works.
  bool resolve_irk(const uint8_t * /*irk*/) const { return false; }

 protected:
  uint8_t address_[6]{};
  int rssi_{0};
  std::string name_;
  std::vector<int8_t> tx_powers_;
  optional<uint16_t> appearance_;
  optional<uint8_t> ad_flag_;
  std::vector<ESPBTUUID> service_uuids_;
  std::vector<ServiceData> manufacturer_datas_;
  std::vector<ServiceData> service_datas_;
};

class ESP32BLETracker;

class ESPBTDeviceListener {
 public:
  virtual ~ESPBTDeviceListener() = default;
  virtual bool parse_device(const ESPBTDevice &device) = 0;
  virtual void on_scan_end() {}
  void set_parent(ESP32BLETracker *parent) { this->parent_ = parent; }

 protected:
  ESP32BLETracker *parent_{nullptr};
};

class ESP32BLETracker : public Component {
 public:
  void set_hci_device(std::string name) { this->hci_device_name_ = std::move(name); }
  void set_scan_duration(uint32_t seconds) { this->scan_duration_s_ = seconds; }
  void set_scan_interval_ms(uint32_t ms) { this->scan_interval_ms_ = ms; }
  void set_scan_window_ms(uint32_t ms) { this->scan_window_ms_ = ms; }
  void set_scan_active(bool active) { this->scan_active_ = active; }
  void set_scan_continuous(bool cont) { this->scan_continuous_ = cont; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void register_listener(ESPBTDeviceListener *listener) {
    listener->set_parent(this);
    this->listeners_.push_back(listener);
  }

 protected:
  void scanner_thread_main_();
  bool open_hci_();
  void close_hci_();
  bool send_le_set_scan_params_();
  bool send_le_set_scan_enable_(bool enable);
  void handle_le_meta_event_(const uint8_t *evt, size_t len);
  void deliver_device_(ESPBTDevice device);

  std::string hci_device_name_{"hci0"};
  uint32_t scan_duration_s_{300};
  uint32_t scan_interval_ms_{320};
  uint32_t scan_window_ms_{30};
  bool scan_active_{true};
  bool scan_continuous_{true};

  std::vector<ESPBTDeviceListener *> listeners_;

  std::thread scanner_thread_;
  std::atomic<bool> stop_thread_{false};
  std::atomic<bool> hci_ok_{false};
  int hci_fd_{-1};
  int hci_dev_id_{-1};

  std::mutex queue_mu_;
  std::deque<ESPBTDevice> queue_;
  static constexpr size_t QUEUE_MAX = 128;
};

}  // namespace esp32_ble_tracker
}  // namespace esphome
