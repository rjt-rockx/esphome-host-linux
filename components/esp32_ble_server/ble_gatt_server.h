#pragma once

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// BLEGattServer — the only D-Bus-aware code in the GATT server. Exports the
// service tree built by the esp32_ble_server shadow classes as an org.bluez
// object tree (ObjectManager + GattService1/GattCharacteristic1/GattDescriptor1
// vtables), registers it via GattManager1.RegisterApplication on the adapter, and
// (as an esp32_ble::AdvertisingBackend) advertises via LEAdvertisingManager1.
//
// Threading uses the shared sd_event worker (esp32_ble::BleWorker), the same loop
// the GATT client uses. This server attaches its own bus to that loop and is an
// attached BusWorkerClient, so notify() commands posted from the main thread are
// drained on the worker. ReadValue/WriteValue/StartNotify/StopNotify arrive on
// the worker and are routed straight to the shadow objects (their on_read/on_write
// callbacks run on the worker — same contract as the client's notify callbacks).
// Connect/disconnect are observed via Device1.Connected and posted back to the
// main thread, where BLEServer::loop() dispatches on_connect/on_disconnect.
//
// The worker lives in esp32_ble (the BLE base this component already auto-loads),
// so the server does NOT depend on the GATT *client* component to share the loop.

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble/ble_host_thread.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <systemd/sd-bus.h>

namespace esphome {
namespace esp32_ble_server {

class BLEServer;
class BLEService;
class BLECharacteristic;
class BLEDescriptor;

// Neutral, project-scoped application root (NOT tied to any org). Generic library.
static constexpr const char *APP_ROOT = "/org/esphome/host/ble/gatt";
static constexpr const char *ADV_PATH = "/org/esphome/host/ble/advertisement0";

// An exported GattCharacteristic1 node.
struct ExportedChar {
  BLECharacteristic *chr;
  std::string path;
  std::string service_path;
  std::string uuid_str;     // 128-bit lowercase, BlueZ form
  uint32_t properties;      // BLECharacteristic PROPERTY_* bitmask
  std::vector<uint8_t> cached_value;  // worker-owned snapshot for Value getter
  bool notifying{false};    // a central StartNotify'd this characteristic
};

// An exported GattDescriptor1 node.
struct ExportedDesc {
  BLEDescriptor *desc;
  std::string path;
  std::string char_path;
  std::string uuid_str;
};

// An exported GattService1 node.
struct ExportedService {
  BLEService *svc;
  std::string path;
  std::string uuid_str;
  bool primary{true};
};

// Main-thread → worker command.
struct ServerCommand {
  enum class Kind : uint8_t { START, NOTIFY } kind;
  std::string char_path;
  std::vector<uint8_t> value;
};

// Worker → main event (connect/disconnect of a central).
struct ServerEvent {
  enum class Kind : uint8_t { CONNECT, DISCONNECT } kind;
  uint16_t conn_id;
};

class BLEGattServer : public esp32_ble::BusWorkerClient, public esp32_ble::AdvertisingBackend {
 public:
  explicit BLEGattServer(BLEServer *owner) : owner_(owner) {}
  ~BLEGattServer() override;

  // Build the object tree from the shadow service list and register with BlueZ.
  // Called once from BLEServer::setup() on the main thread.
  void start(const std::vector<BLEService *> &services);

  // esp32_ble::AdvertisingBackend — the parent ESP32BLE pushes advertising changes
  // here; we (re)register an LEAdvertisement1.
  void on_advertising_changed(const esp32_ble::HostAdvertisement &adv) override;

  // BusWorkerClient — drain the main→worker command queue (notify requests).
  void worker_process_commands() override;

  // Main-thread API: queue a Value notification for a characteristic + wake worker.
  void notify_characteristic(BLECharacteristic *chr, const std::vector<uint8_t> &value);

  // Main-thread drain of connect/disconnect events (called from BLEServer::loop()).
  std::deque<ServerEvent> drain_events();

  // --- sd-bus vtable trampolines ---
  // Public because the file-scope sd_bus_vtable arrays in the .cpp name them
  // (a vtable array is not a member, so it has no access to protected members).
  // They take a void* userdata and are only ever invoked by sd-bus itself.
  // GattService1
  static int svc_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
  static int svc_get_primary_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                              sd_bus_error *);
  // GattCharacteristic1
  static int chr_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
  static int chr_get_service_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                              sd_bus_error *);
  static int chr_get_value_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                            sd_bus_error *);
  static int chr_get_flags_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                            sd_bus_error *);
  static int chr_get_notifying_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                sd_bus_error *);
  static int chr_read_value_(sd_bus_message *, void *, sd_bus_error *);
  static int chr_write_value_(sd_bus_message *, void *, sd_bus_error *);
  static int chr_start_notify_(sd_bus_message *, void *, sd_bus_error *);
  static int chr_stop_notify_(sd_bus_message *, void *, sd_bus_error *);
  // GattDescriptor1
  static int desc_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                            sd_bus_error *);
  static int desc_get_char_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                            sd_bus_error *);
  static int desc_get_flags_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                             sd_bus_error *);
  static int desc_get_value_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                             sd_bus_error *);
  static int desc_read_value_(sd_bus_message *, void *, sd_bus_error *);
  static int desc_write_value_(sd_bus_message *, void *, sd_bus_error *);
  // LEAdvertisement1
  static int adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *, sd_bus_error *);
  static int adv_get_local_name_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                 sd_bus_error *);
  static int adv_get_service_uuids_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                    sd_bus_error *);
  static int adv_get_manufacturer_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                        sd_bus_error *);
  static int adv_get_service_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                   sd_bus_error *);
  static int adv_get_appearance_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                                 sd_bus_error *);
  static int adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *, void *,
                               sd_bus_error *);
  static int adv_release_(sd_bus_message *, void *, sd_bus_error *);
  // Connect/disconnect watch (Device1.Connected PropertiesChanged on adapter subtree).
  static int on_device_props_changed_(sd_bus_message *, void *, sd_bus_error *);

 protected:
  // --- worker-thread internals ---
  bool open_bus_();
  std::string adapter_path_();  // "/org/bluez/hci0"
  void build_tree_(const std::vector<BLEService *> &services);
  // Worker-thread bring-up: open the bus, export the object tree, watch
  // Device1.Connected, and fire RegisterApplication asynchronously. MUST run on
  // the worker: BlueZ answers RegisterApplication only after calling our
  // ObjectManager.GetManagedObjects back, which the worker's event loop must be
  // free to dispatch. A blocking sd_bus_call from the main thread would deadlock
  // (the main thread can't service the inbound GetManagedObjects).
  void do_start_();
  bool register_application_();  // async; reply handled by on_register_app_reply_
  static int on_register_app_reply_(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
  // After registration, post a synthetic CONNECT for every Device1 already
  // Connected=true. BlueZ keeps ACL links + Device1 objects alive across our
  // process restarts, so a central connected before we started emits no
  // PropertiesChanged transition — without this it would be invisible to us.
  void enumerate_connected_devices_();
  void do_notify_(const std::string &char_path, const std::vector<uint8_t> &value);
  void register_advertisement_(const esp32_ble::HostAdvertisement &adv);
  static int on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
  void unregister_advertisement_();
  // Re-advertise after a central disconnects. The Intel AX211 + kernel 6.17 ext-adv
  // auto-resume path is unreliable (BlueZ does nothing on disconnect; the kernel
  // tries hci_enable_advertising but the ext-adv re-enable often fails — github
  // bluez#644). We force a fresh advertising set by Unregister+Register after a
  // short debounce. If even that fails (#644 "Invalid Parameters"), power-cycle the
  // adapter once and retry. Runs entirely on the worker thread (it owns the bus).
  void schedule_readvertise_();              // arm/re-arm the debounce timer
  static int on_readvertise_timer_(sd_event_source *s, uint64_t usec, void *userdata);
  void power_cycle_adapter_();               // Adapter1.Powered false->true (last resort)
  uint16_t intern_device_(const char *device_path);
  void post_event_(ServerEvent ev);

  ExportedChar *find_char_(const char *path);
  ExportedDesc *find_desc_(const char *path);
  ExportedService *find_service_(const char *path);

  BLEServer *owner_;

  sd_bus *bus_{nullptr};
  std::atomic<bool> bus_open_{false};
  bool attached_{false};
  bool started_{false};       // worker-only: do_start_ already ran
  bool start_requested_{false};  // main→worker: START queued (drained in worker_process_commands)

  sd_bus_slot *om_slot_{nullptr};   // ObjectManager on APP_ROOT
  sd_bus_slot *adv_slot_{nullptr};  // LEAdvertisement1 vtable
  sd_bus_slot *device_watch_slot_{nullptr};
  std::vector<sd_bus_slot *> obj_slots_;  // service/char/desc vtable slots

  std::vector<std::unique_ptr<ExportedService>> services_;
  std::vector<std::unique_ptr<ExportedChar>> chars_;
  std::vector<std::unique_ptr<ExportedDesc>> descs_;

  // Synthetic conn_id interning for central device paths.
  std::unordered_map<std::string, uint16_t> device_conn_ids_;
  uint16_t next_conn_id_{1};

  esp32_ble::HostAdvertisement pending_adv_;
  bool adv_registered_{false};
  bool adv_pending_{false};
  sd_event_source *readv_timer_{nullptr};  // one-shot re-advertise debounce (worker loop)
  bool readv_power_cycled_{false};         // power-cycle fallback used once already

  std::mutex cmd_mu_;
  std::deque<ServerCommand> commands_;
  std::mutex evt_mu_;
  std::deque<ServerEvent> events_;
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
