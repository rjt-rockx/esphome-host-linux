#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// BLEGattHost — the only D-Bus-aware code in the GATT client. Owns one sd_bus
// connection per active GATT connection (avoids BlueZ's notify-re-enable quirk
// and keeps one busy connection from starving another). All sd_bus calls run on
// a single shared sd_event worker thread (BLEGattHostThread); commands are
// posted from the main thread and results come back as HostGattEvent on a
// mutex-guarded deque drained in BLEClientBase::loop().
//
// Step 1 scope: connect / disconnect + Device1.Connected signal + connect
// timeout. Discovery, read/write/notify, MTU, pairing land in later steps.

#include "host_gatt_event.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <systemd/sd-bus.h>

struct sd_event;
struct sd_event_source;

namespace esphome {
namespace esp32_ble_client {

// A queued command from the main thread to the worker.
struct HostGattCommand {
  enum class Kind : uint8_t {
    CONNECT,
    DISCONNECT,
    READ_CHAR,
    READ_DESC,
    WRITE_CHAR,
    WRITE_DESC,
    SET_NOTIFY,
  } kind;
  uint16_t handle{0};
  bool flag{false};                  // SET_NOTIFY enable / WRITE_* response
  std::vector<uint8_t> data;         // WRITE_* payload
};

class BLEGattHost;

// Process-global worker: one thread, one sd_event loop, services every
// BLEGattHost's bus. Created lazily on first connect, joined at process exit.
class BLEGattHostThread {
 public:
  static BLEGattHostThread &instance();
  // Register/unregister a host so the worker can service its bus + command queue.
  void attach(BLEGattHost *host);
  void detach(BLEGattHost *host);
  // Wake the event loop to process newly-queued commands.
  void wake();
  sd_event *event() { return this->event_; }

 protected:
  BLEGattHostThread() = default;
  void ensure_started_();
  void run_();

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  sd_event *event_{nullptr};
  sd_event_source *wake_source_{nullptr};
  int wake_fd_{-1};  // eventfd to wake the loop from other threads
  std::mutex hosts_mu_;
  std::vector<BLEGattHost *> hosts_;
  std::mutex pending_mu_;
};

class BLEGattHost {
 public:
  BLEGattHost(std::string adapter, std::string device_path);
  ~BLEGattHost();

  // --- main-thread command API (thread-safe enqueue + worker wakeup) ---
  void connect();
  void disconnect();
  void read_char(uint16_t handle);
  void read_desc(uint16_t handle);
  void write_char(uint16_t handle, const uint8_t *data, size_t len, bool response);
  void write_desc(uint16_t handle, const uint8_t *data, size_t len, bool response);
  void set_notify(uint16_t handle, bool enable);

  // --- main-thread event drain ---
  // Returns and clears the queued events (called from BLEClientBase::loop()).
  std::deque<HostGattEvent> drain_events();

  const std::string &device_path() const { return this->device_path_; }

  // --- worker-thread internals (public for the worker dispatcher) ---
  void worker_process_commands();  // run on worker: drain command queue → D-Bus
  void worker_on_connected_changed(bool connected);

 protected:
  friend class BLEGattHostThread;

  void post_event_(HostGattEvent &&ev);
  void enqueue_command_(HostGattCommand cmd);
  bool open_bus_();
  void close_bus_();
  void do_connect_();
  void do_disconnect_();
  void do_read_(uint16_t handle, bool is_desc);
  void do_write_(uint16_t handle, const std::vector<uint8_t> &data, bool response, bool is_desc);
  void do_set_notify_(uint16_t handle, bool enable);
  // Subscribe to Value PropertiesChanged for a characteristic path (notify).
  void subscribe_value_(const std::string &char_path, uint16_t handle);
  static int on_value_changed_(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
  // Discovery: gate on ServicesResolved, walk GetManagedObjects, acquire MTU,
  // then post SERVICES_DISCOVERED. Returns false (and posts DISCONNECTED) if a
  // GATT object lacks a real Handle (hard requirement — no synthesis).
  void try_start_discovery_();
  bool walk_gatt_tree_(std::vector<DiscoveredService> &out);
  uint16_t acquire_mtu_();
  // sd-bus signal trampoline for PropertiesChanged on this device path.
  static int on_properties_changed_(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

  struct ObjEntry {
    std::string path;
    enum Kind : uint8_t { CHAR, DESC } kind;
    uint16_t parent_char_handle;
  };

  std::string adapter_;       // "hci0"
  std::string device_path_;   // "/org/bluez/hci0/dev_AA_BB_.."

  sd_bus *bus_{nullptr};
  sd_bus_slot *props_slot_{nullptr};
  std::atomic<bool> bus_open_{false};
  bool discovered_{false};  // worker-only: SERVICES_DISCOVERED already posted

  // worker-only handle maps for read/write/notify routing
  std::unordered_map<uint16_t, ObjEntry> handle_map_;
  // notify subscriptions: char path → (handle, signal slot)
  struct NotifySub {
    uint16_t handle;
    sd_bus_slot *slot;
  };
  std::unordered_map<std::string, NotifySub> notify_subs_;

  std::mutex cmd_mu_;
  std::deque<HostGattCommand> commands_;

  std::mutex evt_mu_;
  std::deque<HostGattEvent> events_;
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
