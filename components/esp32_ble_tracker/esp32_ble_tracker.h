#pragma once

// Host-side esp32_ble_tracker: scans BLE advertisements (via BlueZ D-Bus or a
// raw HCI socket) and dispatches them to registered listeners.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/ble_device_base/ble_client_state.h"
#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/ble_device_base/ble_hub.h"
#include "esphome/components/esp32_ble/ble_uuid.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// sd-bus types appear in the D-Bus backend method signatures below. Include the
// real header so they resolve to the genuine ::sd_bus_message / ::sd_bus_error
// (this component only builds for the host platform).
#include <systemd/sd-bus.h>

// Expose esp_bt_uuid_t at global scope: some consumers reference it unqualified,
// as it is a global C typedef on ESP-IDF.
using esp_bt_uuid_t = esphome::esp32_ble::esp_bt_uuid_t;

// --- Stand-ins for ESP-IDF GATT symbols that appear in GATT consumer
// signatures. Numeric values MUST match ESP-IDF's so bit-tests and
// (== ESP_GATT_OK) comparisons in those consumers stay correct. Global scope +
// per-symbol #ifndef so they coexist with anything else providing them.
#ifndef ESP_OK
#define ESP_OK 0
#endif
using esp_err_t = int;
namespace esphome {
namespace esp32_ble_tracker {
const char *esp_err_to_name(esp_err_t err);  // small table; falls back to "host-err(N)"
}  // namespace esp32_ble_tracker
}  // namespace esphome
// esp_err_to_name is also referenced unqualified by some consumers.
using esphome::esp32_ble_tracker::esp_err_to_name;

// esp_gatt_status_t — numeric values MUST equal ESP-IDF's (forwarded verbatim
// over the wire to clients).
using esp_gatt_status_t = int;
enum {
  ESP_GATT_OK = 0x0,
  ESP_GATT_INVALID_HANDLE = 0x01,
  ESP_GATT_READ_NOT_PERMIT = 0x02,
  ESP_GATT_WRITE_NOT_PERMIT = 0x03,
  ESP_GATT_INSUF_AUTHENTICATION = 0x05,
  ESP_GATT_REQ_NOT_SUPPORTED = 0x06,
  ESP_GATT_INVALID_OFFSET = 0x07,
  ESP_GATT_INSUF_AUTHORIZATION = 0x08,
  ESP_GATT_NOT_FOUND = 0x0a,
  ESP_GATT_INVALID_ATTR_LEN = 0x0d,
  ESP_GATT_INSUF_ENCRYPTION = 0x0f,
  ESP_GATT_NO_RESOURCES = 0x80,
  ESP_GATT_ERROR = 0x85,
  ESP_GATT_CONN_TIMEOUT = 0x93,
  ESP_GATT_NOT_CONNECTED = 0x9f,
  ESP_GATT_MAX_ATTR_LEN = 600,
};

// esp_gatt_char_prop_t — characteristic-property bitmask; values == ESP-IDF.
using esp_gatt_char_prop_t = uint8_t;
enum {
  ESP_GATT_CHAR_PROP_BIT_BROADCAST = 0x01,
  ESP_GATT_CHAR_PROP_BIT_READ = 0x02,
  ESP_GATT_CHAR_PROP_BIT_WRITE_NR = 0x04,
  ESP_GATT_CHAR_PROP_BIT_WRITE = 0x08,
  ESP_GATT_CHAR_PROP_BIT_NOTIFY = 0x10,
  ESP_GATT_CHAR_PROP_BIT_INDICATE = 0x20,
  ESP_GATT_CHAR_PROP_BIT_AUTH = 0x40,
  ESP_GATT_CHAR_PROP_BIT_EXT_PROP = 0x80,
};

// Write-type + auth-req enums named in some GATT-client consumer bodies.
using esp_gatt_write_type_t = int;
enum { ESP_GATT_WRITE_TYPE_NO_RSP = 1, ESP_GATT_WRITE_TYPE_RSP = 2 };
using esp_gatt_auth_req_t = int;
enum { ESP_GATT_AUTH_REQ_NONE = 0 };

namespace esphome {
namespace esp32_ble_tracker {

// Host-flavoured UUID used by the GATT client/server shims (esp_bt_uuid_t
// emulation). Advertisement types below use ble_device_base::ESPBTUUID.
using ESPBTUUID = esp32_ble::ESPBTUUID;

// Advertisement types are owned by upstream's platform-neutral ble_device_base
// layer (ESPHome 2026.8.0+); re-exported here so host code keeps its names.
using adv_data_t = ble_device_base::adv_data_t;
using ServiceData = ble_device_base::ServiceData;
using ESPBLEiBeacon = ble_device_base::ESPBLEiBeacon;
using ESPBTDevice = ble_device_base::ESPBTDevice;
using ClientState = ble_device_base::ClientState;
using ConnectionType = ble_device_base::ConnectionType;
using ble_device_base::client_state_to_string;
using ScannerState = ble_device_base::ScannerState;

static constexpr size_t MAC_ADDRESS_PRETTY_BUFFER_SIZE = esphome::MAC_ADDRESS_PRETTY_BUFFER_SIZE;

class ESP32BLETracker;

// Host listener: the neutral parse_device/on_scan_end come from ble_device_base;
// this adds the tracker back-pointer the host GATT shims use.
class ESPBTDeviceListener : public ble_device_base::ESPBTDeviceListener {
 public:
  void set_parent(ESP32BLETracker *parent) { this->parent_ = parent; }

 protected:
  ESP32BLETracker *parent_{nullptr};
};

// Listener for scanner state changes (bluetooth_proxy implements this).
class BLEScannerStateListener {
 public:
  virtual void on_scanner_state(ScannerState state) = 0;
};

/// Base class for GATT clients the tracker drives; the host transport is BlueZ
/// D-Bus. The tracker only needs the connection-lifecycle contract below: it
/// promotes a DISCOVERED client to CONNECTING by calling connect(), and reads
/// state().
class ESPBTClient : public ESPBTDeviceListener {
 public:
  virtual void connect() = 0;
  virtual void disconnect() = 0;
  bool disconnect_pending() const { return this->want_disconnect_; }
  void cancel_pending_disconnect() { this->want_disconnect_ = false; }

  virtual void set_state(ClientState st) {
    this->set_state_internal_(st);
    if (st == ClientState::IDLE) {
      this->want_disconnect_ = false;
    }
  }
  ClientState state() const { return this->state_; }

  void set_tracker_state_version(uint8_t *version) { this->tracker_state_version_ = version; }

  uint8_t app_id;

 protected:
  void set_state_internal_(ClientState st) {
    this->state_ = st;
    if (this->tracker_state_version_ != nullptr) {
      (*this->tracker_state_version_)++;
    }
  }

  bool want_disconnect_{false};
  ClientState state_{ClientState::INIT};
  uint8_t *tracker_state_version_{nullptr};
};

/// The host BLE tracker. Satisfies upstream's ble_device_base::BLEHub contract
/// (ble_hub.h): ble_hub_impl.h aliases BLEHub to this class because the
/// codegen emits USE_ESP32_BLE_TRACKER, so stock BLE sensors bind to it.
class ESP32BLETracker : public Component {
 public:
  void set_hci_device(std::string name) { this->hci_device_name_ = std::move(name); }
  void set_scan_duration(uint32_t seconds) { this->scan_duration_s_ = seconds; }
  void set_scan_interval_ms(uint32_t ms) { this->scan_interval_ms_ = ms; }
  void set_scan_window_ms(uint32_t ms) { this->scan_window_ms_ = ms; }
  void set_scan_active(bool active) { this->scan_active_ = active; }
  void set_scan_continuous(bool cont) { this->scan_continuous_ = cont; }
  // Backend select: default is BlueZ D-Bus (coexists with bluetoothd).
  // hci_backend=true uses the raw-HCI scanner, which needs an adapter the
  // daemon isn't managing.
  void set_use_hci_backend(bool use_hci) { this->use_hci_backend_ = use_hci; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // Host listeners (GATT shims) also get the tracker back-pointer.
  void register_listener(ESPBTDeviceListener *listener) {
    listener->set_parent(this);
    this->listeners_.push_back(listener);
  }

  // ---- ble_device_base::BLEHub ----
  void register_listener(ble_device_base::ESPBTDeviceListener *listener) { this->listeners_.push_back(listener); }
  void set_raw_advertisement_callback(ble_device_base::RawAdvertisementCallback callback) {
    this->raw_advertisement_callback_ = callback;
  }
  static constexpr ble_device_base::HubCapabilities get_capabilities() {
    // BlueZ merges advertisement + scan response per device; the D-Bus backend
    // only sees decoded fields, which the tracker re-encodes as AD structures.
    return {/* active_scan = */ true, /* merges_scan_response = */ true, /* gatt = */ true,
            /* scan_mode_switch = */ false};
  }
  void get_adapter_mac(uint8_t *out) {
    std::lock_guard<std::mutex> g(this->queue_mu_);
    std::memcpy(out, this->adapter_mac_, 6);
  }
  bool scan_running() { return this->hci_ok_.load(); }
  bool scan_active() { return this->scan_active_; }
  // The scan mode is fixed at setup on host.
  bool request_scan_mode(bool /*active*/) { return false; }

  // On host the scan is always-on once started, so RUNNING is reported; listeners
  // are notified at setup.
  void add_scanner_state_listener(BLEScannerStateListener *l) { this->scanner_state_listeners_.push_back(l); }
  ScannerState get_scanner_state() const { return ScannerState::RUNNING; }
  bool get_scan_active() const { return this->scan_active_; }

  // Register a GATT client so the tracker delivers scan results to it and
  // promotes it DISCOVERED→CONNECTING. BlueZ can connect while discovering, so
  // promotion just calls connect() (no scan stop needed).
  void register_client(ESPBTClient *client) {
    client->app_id = this->app_id_counter_++;
    client->set_tracker_state_version(&this->state_version_);
    client->set_parent(this);
    this->clients_.push_back(client);
    this->listeners_.push_back(client);  // clients are also listeners (parse_device)
  }

 protected:
  /// One advertisement as the scanner thread saw it: controller-order MAC plus
  /// AD structures (byte-exact from HCI, re-encoded from BlueZ properties).
  struct PendingAdvertisement {
    uint8_t mac[6];  // LSB first
    int8_t rssi;
    uint8_t addr_type;
    std::vector<uint8_t> data;
  };

  void try_promote_discovered_clients_();
  void set_adapter_mac_(const uint8_t mac_lsb[6]);

  // raw-HCI backend (opt-in)
  void scanner_thread_main_();
  bool open_hci_();
  void close_hci_();
  bool send_le_set_scan_params_();
  bool send_le_set_scan_enable_(bool enable);
  void handle_le_meta_event_(const uint8_t *evt, size_t len);

  // D-Bus / BlueZ backend (default)
  void dbus_scanner_thread_main_();
  bool parse_device1_props_(::sd_bus_message *m, PendingAdvertisement &adv);
  static int on_interfaces_added_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);
  static int on_properties_changed_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);

  void deliver_(PendingAdvertisement adv);

  std::string hci_device_name_{"hci0"};
  uint32_t scan_duration_s_{300};
  uint32_t scan_interval_ms_{320};
  uint32_t scan_window_ms_{30};
  bool scan_active_{true};
  bool scan_continuous_{true};
  bool use_hci_backend_{false};

  std::vector<ble_device_base::ESPBTDeviceListener *> listeners_;
  std::vector<BLEScannerStateListener *> scanner_state_listeners_;
  std::vector<ESPBTClient *> clients_;
  ble_device_base::RawAdvertisementCallback raw_advertisement_callback_{};
  uint8_t state_version_{0};
  uint8_t last_state_version_{0};
  uint8_t app_id_counter_{0};

  std::thread scanner_thread_;
  std::atomic<bool> stop_thread_{false};
  std::atomic<bool> hci_ok_{false};
  int hci_fd_{-1};
  int hci_dev_id_{-1};

  std::mutex queue_mu_;  // guards queue_ and adapter_mac_
  std::deque<PendingAdvertisement> queue_;
  uint8_t adapter_mac_[6]{};  // printable order (MSB first)
  static constexpr size_t QUEUE_MAX = 128;
};

}  // namespace esp32_ble_tracker
}  // namespace esphome
