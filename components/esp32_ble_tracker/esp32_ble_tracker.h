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

// On ESP-IDF `esp_bt_uuid_t` is a global C typedef; some consumers (e.g.
// thermopro_ble) reference it unqualified. Expose our stand-in at global scope
// so those compile unchanged on host.
using esp_bt_uuid_t = esphome::esp32_ble::esp_bt_uuid_t;

// --- Portable stand-ins for ESP-IDF GATT symbols that survive in GATT-client/
// server consumer signatures. Values match ESP-IDF numerics so existing
// bit-tests and (== ESP_GATT_OK) comparisons in stock components are correct.
// These are real, honestly-typed host values — NOT an emulation of the IDF
// event model (the native BLEGattHost drives BlueZ directly). Global scope +
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

// esp_gatt_status_t — the proxy forwards these to Home Assistant; numerics MUST
// equal ESP-IDF's esp_gatt_status_t.
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

// esp_gatt_char_prop_t — portable bitmask; values == ESP-IDF.
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

// Write-type + auth-req enums still named in unported am43/automation bodies.
using esp_gatt_write_type_t = int;
enum { ESP_GATT_WRITE_TYPE_NO_RSP = 1, ESP_GATT_WRITE_TYPE_RSP = 2 };
using esp_gatt_auth_req_t = int;
enum { ESP_GATT_AUTH_REQ_NONE = 0 };

namespace esphome {
namespace esp32_ble_tracker {

// Re-export so downstream code can refer to esp32_ble_tracker::ESPBTUUID just
// like the upstream tracker does via `using namespace esp32_ble;`.
using ESPBTUUID = esp32_ble::ESPBTUUID;

// Size of an "AA:BB:CC:DD:EE:FF" string including the NUL terminator. Matches
// upstream so consumers (e.g. ble_scanner) that declare a fixed buffer of this
// size and call address_str_to() compile unchanged.
static constexpr size_t MAC_ADDRESS_PRETTY_BUFFER_SIZE = 18;

// Matches upstream: the manufacturer/service-data byte payload type used by
// some parsers (e.g. ruuvi_ble) as `esp32_ble_tracker::adv_data_t`.
using adv_data_t = std::vector<uint8_t>;

struct ServiceData {
  ESPBTUUID uuid;
  adv_data_t data;
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
  // Format the MAC into a caller-provided buffer (no heap alloc), returning the
  // buffer pointer. Mirrors upstream's signature so consumers like ble_scanner
  // compile unchanged.
  const char *address_str_to(std::span<char, MAC_ADDRESS_PRETTY_BUFFER_SIZE> buf) const {
    std::snprintf(buf.data(), buf.size(), "%02X:%02X:%02X:%02X:%02X:%02X", this->address_[5], this->address_[4],
                  this->address_[3], this->address_[2], this->address_[1], this->address_[0]);
    return buf.data();
  }
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

enum class AdvertisementParserType;

class ESPBTDeviceListener {
 public:
  virtual ~ESPBTDeviceListener() = default;
  virtual bool parse_device(const ESPBTDevice &device) = 0;
  // Raw-advertisement path (bluetooth_proxy). Default no-op; the host backend
  // delivers via parse_device, and the proxy re-serializes parsed fields.
  virtual bool parse_devices(const ESPBTDevice *devices, size_t count) { return false; }
  virtual void on_scan_end() {}
  void set_parent(ESP32BLETracker *parent) { this->parent_ = parent; }

 protected:
  ESP32BLETracker *parent_{nullptr};
};

// Mirrors upstream esp32_ble_tracker::ClientState so esp32_ble_client and
// consumers see the same state machine.
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

// Scanner state, mirrored from upstream. On the host D-Bus backend the scan is
// effectively always running once started; RUNNING is reported.
enum class ScannerState {
  IDLE,
  STARTING,
  RUNNING,
  FAILED,
  STOPPING,
};

// Listener for scanner state changes (bluetooth_proxy implements this).
class BLEScannerStateListener {
 public:
  virtual void on_scanner_state(ScannerState state) = 0;
};

enum class AdvertisementParserType {
  PARSED_ADVERTISEMENTS,
  RAW_ADVERTISEMENTS,
};

/// Base class for GATT clients the tracker drives. On host the GATT transport
/// is BlueZ D-Bus (a native esp32_ble_client::BLEClientBase implements connect/
/// discover/read/write/notify against org.bluez — no ESP-IDF emulation). The
/// tracker only needs the connection-lifecycle contract below; it promotes a
/// DISCOVERED client to CONNECTING by calling connect(), and reads state().
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

class ESP32BLETracker : public Component {
 public:
  void set_hci_device(std::string name) { this->hci_device_name_ = std::move(name); }
  void set_scan_duration(uint32_t seconds) { this->scan_duration_s_ = seconds; }
  void set_scan_interval_ms(uint32_t ms) { this->scan_interval_ms_ = ms; }
  void set_scan_window_ms(uint32_t ms) { this->scan_window_ms_ = ms; }
  void set_scan_active(bool active) { this->scan_active_ = active; }
  void set_scan_continuous(bool cont) { this->scan_continuous_ = cont; }
  // Backend select: default is BlueZ D-Bus (coexists with bluetoothd/HA).
  // hci_backend=true uses the raw-HCI scanner, which needs an adapter the
  // daemon isn't managing (see references/ble-host CHARTER §2 item T).
  void set_use_hci_backend(bool use_hci) { this->use_hci_backend_ = use_hci; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void register_listener(ESPBTDeviceListener *listener) {
    listener->set_parent(this);
    this->listeners_.push_back(listener);
  }

  // Scanner-state listeners (bluetooth_proxy). On host the scan is always-on
  // once started, so we report RUNNING; the listener is notified at setup.
  void add_scanner_state_listener(BLEScannerStateListener *l) { this->scanner_state_listeners_.push_back(l); }
  ScannerState get_scanner_state() const { return ScannerState::RUNNING; }
  bool get_scan_active() const { return this->scan_active_; }

  // Register a GATT client so the tracker delivers scan results to it and
  // promotes it DISCOVERED→CONNECTING. On the D-Bus backend BlueZ can connect
  // while discovering, so promotion just calls connect() (no scan stop needed).
  void register_client(ESPBTClient *client) {
    client->app_id = this->app_id_counter_++;
    client->set_tracker_state_version(&this->state_version_);
    client->set_parent(this);
    this->clients_.push_back(client);
    this->listeners_.push_back(client);  // clients are also listeners (parse_device)
  }

 protected:
  void try_promote_discovered_clients_();

  // raw-HCI backend (opt-in)
  void scanner_thread_main_();
  bool open_hci_();
  void close_hci_();
  bool send_le_set_scan_params_();
  bool send_le_set_scan_enable_(bool enable);
  void handle_le_meta_event_(const uint8_t *evt, size_t len);

  // D-Bus / BlueZ backend (default)
  void dbus_scanner_thread_main_();
  bool parse_device1_props_(::sd_bus_message *m, ESPBTDevice &device);
  static int on_interfaces_added_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);
  static int on_properties_changed_(::sd_bus_message *m, void *userdata, ::sd_bus_error *ret_error);

  void deliver_device_(ESPBTDevice device);

  std::string hci_device_name_{"hci0"};
  uint32_t scan_duration_s_{300};
  uint32_t scan_interval_ms_{320};
  uint32_t scan_window_ms_{30};
  bool scan_active_{true};
  bool scan_continuous_{true};
  bool use_hci_backend_{false};

  std::vector<ESPBTDeviceListener *> listeners_;
  std::vector<BLEScannerStateListener *> scanner_state_listeners_;
  std::vector<ESPBTClient *> clients_;
  uint8_t state_version_{0};
  uint8_t last_state_version_{0};
  uint8_t app_id_counter_{0};

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
