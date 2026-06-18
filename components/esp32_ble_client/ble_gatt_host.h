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
    READ_RSSI,
    PAIR,
    PASSKEY_REPLY,
    CONFIRM_REPLY,
    REMOVE_BOND,
  } kind;
  uint16_t handle{0};
  bool flag{false};                  // SET_NOTIFY enable / WRITE_* response / CONFIRM accept
  uint32_t passkey{0};               // PASSKEY_REPLY
  std::vector<uint8_t> data;         // WRITE_* payload
};

class BLEGattHost;

// Anything that owns an sd_bus attached to the shared worker loop and needs its
// command queue drained once per loop iteration implements this. BLEGattHost is
// one; the GATT server (esp32_ble_server) is another. Lets the worker service
// both without depending on their concrete types.
struct BusWorkerClient {
  virtual ~BusWorkerClient() = default;
  // Run on the worker thread after each sd_event_run return: drain the client's
  // main-thread→worker command queue and dispatch to D-Bus.
  virtual void worker_process_commands() = 0;
};

// Process-global worker: one thread, one sd_event loop, services every
// attached BusWorkerClient's bus. Created lazily on first use, joined at exit.
class BLEGattHostThread {
 public:
  static BLEGattHostThread &instance();
  // Register/unregister a host so the worker can service its bus + command queue.
  void attach(BLEGattHost *host);
  void detach(BLEGattHost *host);
  // Generic worker-client registration (e.g. the GATT server). The worker calls
  // worker_process_commands() on each attached client after every loop pass. The
  // client is responsible for attaching its own bus to event().
  void attach_worker_client(BusWorkerClient *client);
  void detach_worker_client(BusWorkerClient *client);
  // Wake the event loop to process newly-queued commands.
  void wake();
  sd_event *event() { return this->event_; }

  // Lazily register a NON-DEFAULT org.bluez.Agent1 (RegisterAgent only — never
  // RequestDefaultAgent, so the system agent is never hijacked). Called when a
  // client declares pairing. The agent routes callbacks to the host whose
  // device_path matches the requesting device. Uses `bus` (a client's bus).
  void ensure_agent(sd_bus *bus);
  BLEGattHost *find_host_by_device_(const char *device_path);

  // Agent1 method trampolines (public so the file-scope vtable can name them).
  static int agent_request_passkey_(sd_bus_message *m, void *userdata, sd_bus_error *e);
  static int agent_display_passkey_(sd_bus_message *m, void *userdata, sd_bus_error *e);
  static int agent_request_confirmation_(sd_bus_message *m, void *userdata, sd_bus_error *e);
  static int agent_release_(sd_bus_message *m, void *userdata, sd_bus_error *e);
  static int agent_cancel_(sd_bus_message *m, void *userdata, sd_bus_error *e);

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
  std::vector<BusWorkerClient *> worker_clients_;  // generic (server, …); same mutex
  std::mutex pending_mu_;

  // Agent1 (non-default). Registered once on the first pairing-capable client.
  bool agent_registered_{false};
  sd_bus_slot *agent_vtable_slot_{nullptr};
};

// D-Bus object path for our non-default pairing agent. Neutral, project-scoped
// path (not tied to any org) so this works as a generic library.
static constexpr const char *AGENT_PATH = "/org/esphome/host/ble/agent";

class BLEGattHost : public BusWorkerClient {
 public:
  BLEGattHost(std::string adapter, std::string device_path);
  ~BLEGattHost() override;

  // --- main-thread command API (thread-safe enqueue + worker wakeup) ---
  void connect();
  void disconnect();
  void read_char(uint16_t handle);
  void read_desc(uint16_t handle);
  void write_char(uint16_t handle, const uint8_t *data, size_t len, bool response);
  void write_desc(uint16_t handle, const uint8_t *data, size_t len, bool response);
  void set_notify(uint16_t handle, bool enable);
  void read_rssi();
  void pair();
  void passkey_reply(uint32_t passkey);
  void confirm_reply(bool accept);
  void remove_bond();

  // --- main-thread event drain ---
  // Returns and clears the queued events (called from BLEClientBase::loop()).
  std::deque<HostGattEvent> drain_events();

  const std::string &device_path() const { return this->device_path_; }

  // --- worker-thread internals (public for the worker dispatcher) ---
  void worker_process_commands() override;  // run on worker: drain command queue → D-Bus
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
  void do_read_rssi_();
  void do_pair_();
  void do_passkey_reply_(uint32_t passkey);
  void do_confirm_reply_(bool accept);
  void do_remove_bond_();
  // Subscribe to Value PropertiesChanged for a characteristic path (notify).
  void subscribe_value_(const std::string &char_path, uint16_t handle);
  static int on_value_changed_(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
  // Discovery: gate on ServicesResolved, walk GetManagedObjects, acquire MTU,
  // then post SERVICES_DISCOVERED. Returns false (and posts DISCONNECTED) if a
  // GATT object lacks a real Handle (hard requirement — no synthesis).
  void try_start_discovery_();
  bool walk_gatt_tree_(std::vector<DiscoveredService> &out);
  // Append a synthetic read-only GAP service (0x1800) built from Device1
  // properties IFF BlueZ didn't already export one (BlueZ <5.79). Keeps the
  // GATTGetServices view byte-compatible with a stock ESP32 proxy. Populates
  // synthetic_reads_ so do_read_ can serve the synthetic characteristics.
  void synthesize_gap_service_(std::vector<DiscoveredService> &out);
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
  bool discovered_{false};   // worker-only: SERVICES_DISCOVERED already posted
  bool discovering_{false};  // worker-only re-entrancy guard: a try_start_discovery_
                             // is in flight. Blocking sd_bus_call*s inside the walk
                             // pump the bus and dispatch queued ServicesResolved/notify
                             // signals re-entrantly on this same thread, which would
                             // otherwise re-enter try_start_discovery_ before discovered_
                             // is set — two invocations mutate/free the same service tree.

  // worker-only handle maps for read/write/notify routing
  std::unordered_map<uint16_t, ObjEntry> handle_map_;
  // Synthetic GAP (0x1800) characteristic reads served from Device1 properties.
  // BlueZ <5.79 never exports the GAP service as a GattService1 (it claims it
  // internally and surfaces Name/Appearance as Device1 props), so a stock-ESP32
  // proxy reports 4 services where we'd report 3. We rebuild a read-only 0x1800
  // from Device1 to stay byte-compatible with a real ESP32 GATTGetServices.
  // handle -> fixed value bytes. do_read_ checks this before handle_map_.
  std::unordered_map<uint16_t, std::vector<uint8_t>> synthetic_reads_;
  // notify subscriptions: char path → (handle, signal slot)
  struct NotifySub {
    uint16_t handle;
    sd_bus_slot *slot;
  };
  std::unordered_map<std::string, NotifySub> notify_subs_;

  // Pairing: a parked Agent1 reply awaiting passkey_reply/confirm_reply.
  sd_bus_message *pending_agent_reply_{nullptr};

 public:
  // Called by the process-global agent (worker thread) when BlueZ asks this
  // device for a passkey / numeric comparison. Stores the reply to answer later.
  void agent_request_passkey(sd_bus_message *reply);
  void agent_display_passkey(uint32_t passkey);
  void agent_request_confirmation(sd_bus_message *reply, uint32_t passkey);
  bool has_pending_agent_reply() const { return this->pending_agent_reply_ != nullptr; }

 protected:

  std::mutex cmd_mu_;
  std::deque<HostGattCommand> commands_;

  std::mutex evt_mu_;
  std::deque<HostGattEvent> events_;
};

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
