#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"

#include "ble_service.h"
#include "host_gatt_event.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace esp32_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

#ifdef USE_HOST
class BLEGattHost;
#endif

static const int UNSET_CONN_ID = 0xFFFF;

// Native host GATT client base. Implements the upstream BLEClientBase public
// surface directly on BlueZ D-Bus (via BLEGattHost) — NOT an emulation of the
// ESP-IDF gattc event model. Events from the worker are drained in loop() and
// fanned out through the dispatch_event_ seam (BLEClient overrides it to reach
// nodes).
class BLEClientBase : public espbt::ESPBTClient, public Component {
 public:
  BLEClientBase();
  ~BLEClientBase() override;  // defined in .cpp where BLEGattHost is complete

  void setup() override;
  void loop() override;
  float get_setup_priority() const override;
  void dump_config() override;

  void run_later(std::function<void()> &&f);  // NOLINT
  bool parse_device(const espbt::ESPBTDevice &device) override;
  void on_scan_end() override {}
  void connect() override;
  void disconnect() override;
  void unconditional_disconnect();
  void release_services();

  esp_err_t pair();

  bool connected() { return this->state() == espbt::ClientState::ESTABLISHED; }
  void set_auto_connect(bool auto_connect) { this->auto_connect_ = auto_connect; }

  // Adapter the connection uses (default hci0); wired from Python via the tracker.
  void set_adapter(const std::string &adapter) { this->adapter_ = adapter; }

  virtual void set_address(uint64_t address);
  const char *address_str() const { return this->address_str_; }
  uint64_t get_address() const { return this->address_; }

  BLEService *get_service(espbt::ESPBTUUID uuid);
  BLEService *get_service(uint16_t uuid);
  BLECharacteristic *get_characteristic(espbt::ESPBTUUID service, espbt::ESPBTUUID chr);
  BLECharacteristic *get_characteristic(uint16_t service, uint16_t chr);
  BLECharacteristic *get_characteristic(uint16_t handle);
  BLEDescriptor *get_descriptor(espbt::ESPBTUUID service, espbt::ESPBTUUID chr, espbt::ESPBTUUID descr);
  BLEDescriptor *get_descriptor(uint16_t handle);
  BLEDescriptor *get_config_descriptor(uint16_t handle);

  float parse_char_value(uint8_t *value, uint16_t length);

  // Kept only for proxy logging; returns an internal synthetic id, never fed to
  // any transport call.
  uint16_t get_conn_id() const { return this->conn_id_; }
  uint16_t get_mtu() const { return this->mtu_; }
  bool is_paired() const { return this->paired_; }
  uint8_t get_connection_index() const { return this->connection_index_; }
  void set_connection_index(uint8_t i) { this->connection_index_ = i; }
  virtual void set_connection_type(espbt::ConnectionType ct) { this->connection_type_ = ct; }

  void set_state(espbt::ClientState st) override;

  // --- native handle-based primitives (proxy + characteristic wrappers call
  // these with the same names/signatures the proxy already uses). Step 1
  // provides the connect/disconnect path; read/write/notify land in later steps
  // (here they return NOT_CONNECTED until implemented).
  esp_err_t read_characteristic(uint16_t handle);
  esp_err_t write_characteristic(uint16_t handle, const uint8_t *data, size_t length, bool response);
  esp_err_t read_descriptor(uint16_t handle);
  esp_err_t write_descriptor(uint16_t handle, const uint8_t *data, size_t length, bool response);
  esp_err_t notify_characteristic(uint16_t handle, bool enable);
  esp_err_t passkey_reply(uint32_t passkey);
  esp_err_t confirm_reply(bool accept);
  esp_err_t remove_bond();
  void read_rssi(std::function<void(int8_t)> &&cb);

 protected:
  // Called once per drained HostGattEvent on the main thread. Base impl advances
  // the ClientState machine; BLEClient overrides to also fan to nodes.
  virtual void dispatch_event_(const HostGattEvent &ev);

  bool check_addr_(const uint8_t bda[6]);
  void set_idle_() {
    this->set_state(espbt::ClientState::IDLE);
    this->conn_id_ = UNSET_CONN_ID;
  }
  void set_disconnecting_();
  virtual void on_disconnect_complete(esp_err_t reason) {}

  uint64_t address_{0};
  std::string adapter_{"hci0"};
  std::string device_path_;
  std::vector<BLEService *> services_;

#ifdef USE_HOST
  std::unique_ptr<BLEGattHost> host_;
#endif

  uint32_t disconnecting_started_{0};
  uint32_t connecting_started_{0};
  uint16_t conn_id_{UNSET_CONN_ID};
  uint16_t mtu_{23};
  espbt::ConnectionType connection_type_{espbt::ConnectionType::V1};
  uint8_t connection_index_{0};
  uint8_t remote_bda_[6]{};
  char address_str_[espbt::MAC_ADDRESS_PRETTY_BUFFER_SIZE]{};
  bool auto_connect_{false};
  bool paired_{false};

  std::function<void(int8_t)> rssi_cb_;

  static constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
  static constexpr uint32_t DISCONNECT_TIMEOUT_MS = 10000;
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
