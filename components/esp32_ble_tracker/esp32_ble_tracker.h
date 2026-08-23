#pragma once

// Host esp32_ble_tracker: a ble_device_base::BLEHub backed by BlueZ (D-Bus) or
// a raw HCI socket. Advertisement types come from ble_device_base, so every
// stock BLE consumer binds to this hub unmodified; the component keeps the
// esp32_ble_tracker name so core's BLEHub alias ladder selects it.

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/ble_device_base/ble_hub.h"
#include "esphome/components/ble_device_base/scan_response_merger.h"
#include "esphome/components/esp32_ble/ble_uuid.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

// The GATT family (ble_client, esp32_ble_client, esp32_ble_server) needs the
// esp_bt_uuid_t conversions, so it keeps esp32_ble's UUID type; the scanner
// path uses the neutral one via ble_device_base's advertisement types.
using ESPBTUUID = esp32_ble::ESPBTUUID;

// Size of an "AA:BB:CC:DD:EE:FF" string including the NUL terminator. Consumers
// declare fixed buffers of this size for address_str_to().
static constexpr size_t MAC_ADDRESS_PRETTY_BUFFER_SIZE = esphome::MAC_ADDRESS_PRETTY_BUFFER_SIZE;

// Advertisement types are owned by ble_device_base; re-exported so the repo's
// own components keep referring to them as espbt::<name>.
using adv_data_t = ble_device_base::adv_data_t;
using ServiceData = ble_device_base::ServiceData;
using ESPBLEiBeacon = ble_device_base::ESPBLEiBeacon;
using ESPBTDevice = ble_device_base::ESPBTDevice;
using ESPBTDeviceListener = ble_device_base::ESPBTDeviceListener;

class ESP32BLETracker;

// GATT-client connection state machine, shared with GATT-client consumers.
enum class ClientState : uint8_t {
  INIT,
  DISCONNECTING,
  IDLE,
  DISCOVERED,
  CONNECTING,
  CONNECTED,
  ESTABLISHED,
};

const char *client_state_to_string(ClientState state);

enum class ConnectionType : uint8_t {
  V1,
  V3_WITH_CACHE,
  V3_WITHOUT_CACHE,
};

using ScannerState = ble_device_base::ScannerState;

// Listener for scanner state changes (bluetooth_proxy implements this).
class BLEScannerStateListener {
 public:
  virtual void on_scanner_state(ScannerState state) = 0;
};

enum class AdvertisementParserType {
  PARSED_ADVERTISEMENTS,
  RAW_ADVERTISEMENTS,
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

  void set_parent(ESP32BLETracker *parent) { this->parent_ = parent; }
  void set_tracker_state_version(uint8_t *version) { this->tracker_state_version_ = version; }

  uint8_t app_id;

 protected:
  void set_state_internal_(ClientState st) {
    this->state_ = st;
    if (this->tracker_state_version_ != nullptr) {
      (*this->tracker_state_version_)++;
    }
  }

  ESP32BLETracker *parent_{nullptr};
  bool want_disconnect_{false};
  ClientState state_{ClientState::INIT};
  uint8_t *tracker_state_version_{nullptr};
};

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

  // Mirrors esp32_ble_tracker / rp2_ble_tracker: a one-shot scan
  // (continuous: false) stops after its duration; restart it from a lambda
  // with `id(my_tracker).start_scan();`. Set scan_continuous_ via
  // set_scan_continuous() first to change the mode.
  void start_scan();
  void stop_scan();

  // ---- ble_device_base::BLEHub contract ----
  void register_listener(ble_device_base::ESPBTDeviceListener *listener) {
    this->dispatcher_.register_listener(listener);
  }
  void set_raw_advertisement_callback(ble_device_base::RawAdvertisementCallback callback) {
    this->dispatcher_.set_raw_advertisement_callback(callback);
  }
  static constexpr ble_device_base::HubCapabilities get_capabilities() {
    // Both backends deliver merged frames: BlueZ aggregates advertisement and
    // scan response into the Device1 properties, and the raw-HCI path runs the
    // shared ScanResponseMerger. GATT connections still go through the repo's
    // own BlueZ client rather than a ble_device_base backend.
    return {.active_scan = true, .merges_scan_response = true, .gatt = false, .scan_mode_switch = false};
  }
  /// Adapter address in printable (MSB-first) order; all-zero when it could not
  /// be read (no adapter, or no permission).
  void get_adapter_mac(uint8_t out[MAC_ADDRESS_SIZE]) { std::memcpy(out, this->adapter_mac_, MAC_ADDRESS_SIZE); }
  bool scan_running() { return this->scan_running_.load(std::memory_order_relaxed); }
  bool scan_active() { return this->scan_active_; }
  /// Neither backend re-programs the scan type after start, so the mode is
  /// whatever scan_parameters configured (scan_mode_switch = false).
  bool request_scan_mode(bool active) { return false; }

  // RUNNING while a scanner backend worker is up, IDLE after a one-shot scan
  // ends (or a stop_scan()); listeners are notified on every transition.
  void add_scanner_state_listener(BLEScannerStateListener *l) { this->scanner_state_listeners_.push_back(l); }
  ScannerState get_scanner_state() const { return this->scanner_state_; }
  bool get_scan_active() const { return this->scan_active_; }

  // Register a GATT client so the tracker delivers scan results to it and
  // promotes it DISCOVERED→CONNECTING. BlueZ can connect while discovering, so
  // promotion just calls connect() (no scan stop needed).
  void register_client(ESPBTClient *client) {
    client->app_id = this->app_id_counter_++;
    client->set_tracker_state_version(&this->state_version_);
    client->set_parent(this);
    this->clients_.push_back(client);
    this->dispatcher_.register_listener(client);  // clients are also listeners (parse_device)
  }

 protected:
  // One advertisement as produced by a scanner thread, drained on the main loop.
  // 62 bytes = legacy advertisement (31) + scan response (31), matching the
  // merger's maximum frame.
  struct AdvFrame {
    uint8_t mac[MAC_ADDRESS_SIZE];  // controller order (LSB first)
    int8_t rssi;
    uint8_t addr_type;
    uint8_t evt_type;  // HCI advertising event type, or EVT_TYPE_MERGED
    uint8_t len;
    uint8_t data[62];
  };
  // The D-Bus backend re-encodes already-merged BlueZ properties, so its frames
  // bypass the merger. Outside the 0x00..0x04 range the HCI spec uses.
  static constexpr uint8_t EVT_TYPE_MERGED = 0xff;

  void try_promote_discovered_clients_();
  void read_adapter_mac_();
  void enqueue_frame_(const AdvFrame &frame);

  // Scan lifecycle (all called on the main loop).
  void start_scan_();
  void stop_scan_();
  void drain_queue_(uint32_t now);
  void fire_scan_end_();
  void set_scanner_state_(ScannerState state);

  // raw-HCI backend (opt-in)
  void scanner_thread_main_();
  bool open_hci_();
  void close_hci_();
  bool send_le_set_scan_params_();
  bool send_le_set_scan_enable_(bool enable);
  void handle_le_meta_event_(const uint8_t *evt, size_t len);

  // D-Bus / BlueZ backend (default)
  void dbus_scanner_thread_main_();
  bool parse_device1_props_(::sd_bus_message *m, AdvFrame &frame);
  static int on_interfaces_added_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);
  static int on_properties_changed_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);
  void record_addr_type_(const uint8_t mac[MAC_ADDRESS_SIZE], uint8_t addr_type);
  void lookup_addr_type_(const uint8_t mac[MAC_ADDRESS_SIZE], uint8_t &addr_type) const;

  std::string hci_device_name_{"hci0"};
  uint32_t scan_duration_s_{300};
  uint32_t scan_interval_ms_{320};
  uint32_t scan_window_ms_{30};
  bool scan_active_{true};
  bool scan_continuous_{true};
  bool use_hci_backend_{false};
  uint8_t adapter_mac_[MAC_ADDRESS_SIZE]{};

  std::vector<BLEScannerStateListener *> scanner_state_listeners_;
  std::vector<ESPBTClient *> clients_;
  uint8_t state_version_{0};
  uint8_t last_state_version_{0};
  uint8_t app_id_counter_{0};
  uint32_t scan_period_start_{0};

  std::thread scanner_thread_;
  std::atomic<bool> stop_thread_{false};
  std::atomic<bool> scan_running_{false};
  // Set by the worker when it returns (stop honored, backend failure); the
  // main loop reaps the thread and reports FAILED (or IDLE on request).
  std::atomic<bool> thread_exited_{false};
  ScannerState scanner_state_{ScannerState::IDLE};
  // Address types seen in InterfacesAdded, keyed by MAC packed into a u64:
  // PropertiesChanged updates rarely carry AddressType, so without this cache
  // every update would flip a random-address device back to public. Only
  // touched from the D-Bus worker thread.
  std::vector<std::pair<uint64_t, uint8_t>> dbus_addr_types_;
  int hci_fd_{-1};
  int hci_dev_id_{-1};

  std::mutex queue_mu_;
  std::deque<AdvFrame> queue_;
  static constexpr size_t QUEUE_MAX = 128;

  // Shared adv + scan-response merge and frame dispatch (ble_device_base). All
  // calls run on the main loop.
  ble_device_base::ScanResponseMerger merger_;
  ble_device_base::AdvDispatcher dispatcher_;
};

}  // namespace esp32_ble_tracker
}  // namespace esphome
