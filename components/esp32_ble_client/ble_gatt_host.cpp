#ifdef USE_HOST

#include "ble_gatt_host.h"

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

#include <systemd/sd-event.h>

namespace esphome {
namespace esp32_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

static const char *const TAG = "ble_gatt_host";

// ---------------------------------------------------------------------------
// BLEGattHostThread — shared worker (one thread, one sd_event loop)
// ---------------------------------------------------------------------------

BLEGattHostThread &BLEGattHostThread::instance() {
  static BLEGattHostThread inst;
  return inst;
}

// eventfd wake handler: just drains the counter; the real work (servicing each
// host's command queue) happens after sd_event_run returns in run_().
static int wake_handler(sd_event_source * /*s*/, int fd, uint32_t /*revents*/, void * /*userdata*/) {
  uint64_t v;
  ssize_t r = read(fd, &v, sizeof(v));
  (void) r;  // best-effort drain
  return 0;
}

void BLEGattHostThread::ensure_started_() {
  bool expected = false;
  if (!this->running_.compare_exchange_strong(expected, true))
    return;  // already started

  this->wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "eventfd() failed: %s", std::strerror(errno));
    this->running_ = false;
    return;
  }
  this->thread_ = std::thread([this] { this->run_(); });
  // Process-lifetime daemon worker: detach so the joinable std::thread doesn't
  // std::terminate at exit (the singleton outlives all hosts). The loop checks
  // stop_ for clean shutdown when that is wired.
  this->thread_.detach();
}

void BLEGattHostThread::run_() {
  if (sd_event_new(&this->event_) < 0) {
    ESP_LOGE(TAG, "sd_event_new failed");
    return;
  }
  sd_event_add_io(this->event_, &this->wake_source_, this->wake_fd_, EPOLLIN, wake_handler, this);

  while (!this->stop_.load()) {
    // Service the loop; 1s timeout bounds shutdown latency. Each return is an
    // opportunity to run any newly-queued per-host commands.
    int r = sd_event_run(this->event_, 1000000 /* us */);
    if (r < 0) {
      ESP_LOGW(TAG, "sd_event_run: %s", std::strerror(-r));
      break;
    }
    std::vector<BLEGattHost *> snapshot;
    std::vector<BusWorkerClient *> client_snapshot;
    {
      std::lock_guard<std::mutex> g(this->hosts_mu_);
      snapshot = this->hosts_;
      client_snapshot = this->worker_clients_;
    }
    for (auto *h : snapshot)
      h->worker_process_commands();
    for (auto *c : client_snapshot)
      c->worker_process_commands();
  }

  if (this->wake_source_ != nullptr)
    sd_event_source_unref(this->wake_source_);
  if (this->event_ != nullptr)
    this->event_ = sd_event_unref(this->event_);
  if (this->wake_fd_ >= 0)
    ::close(this->wake_fd_);
}

void BLEGattHostThread::attach(BLEGattHost *host) {
  this->ensure_started_();
  {
    std::lock_guard<std::mutex> g(this->hosts_mu_);
    this->hosts_.push_back(host);
  }
  this->wake();
}

void BLEGattHostThread::detach(BLEGattHost *host) {
  std::lock_guard<std::mutex> g(this->hosts_mu_);
  for (auto it = this->hosts_.begin(); it != this->hosts_.end(); ++it) {
    if (*it == host) {
      this->hosts_.erase(it);
      break;
    }
  }
}

void BLEGattHostThread::attach_worker_client(BusWorkerClient *client) {
  this->ensure_started_();
  {
    std::lock_guard<std::mutex> g(this->hosts_mu_);
    this->worker_clients_.push_back(client);
  }
  this->wake();
}

void BLEGattHostThread::detach_worker_client(BusWorkerClient *client) {
  std::lock_guard<std::mutex> g(this->hosts_mu_);
  for (auto it = this->worker_clients_.begin(); it != this->worker_clients_.end(); ++it) {
    if (*it == client) {
      this->worker_clients_.erase(it);
      break;
    }
  }
}

void BLEGattHostThread::wake() {
  if (this->wake_fd_ < 0)
    return;
  uint64_t v = 1;
  ssize_t r = write(this->wake_fd_, &v, sizeof(v));
  (void) r;
}

BLEGattHost *BLEGattHostThread::find_host_by_device_(const char *device_path) {
  if (device_path == nullptr)
    return nullptr;
  std::lock_guard<std::mutex> g(this->hosts_mu_);
  for (auto *h : this->hosts_)
    if (h->device_path() == device_path)
      return h;
  return nullptr;
}

// org.bluez.Agent1 vtable. CRITICAL: registered NON-DEFAULT (RegisterAgent
// only). We never call RequestDefaultAgent, so the user's system/desktop agent
// keeps handling all other pairings (invariant: don't hijack the system agent).
static const sd_bus_vtable AGENT_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", BLEGattHostThread::agent_release_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", BLEGattHostThread::agent_request_passkey_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", BLEGattHostThread::agent_display_passkey_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", BLEGattHostThread::agent_request_confirmation_,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", BLEGattHostThread::agent_cancel_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

void BLEGattHostThread::ensure_agent(sd_bus *bus) {
  if (this->agent_registered_ || bus == nullptr)
    return;
  // Export the Agent1 object.
  int r = sd_bus_add_object_vtable(bus, &this->agent_vtable_slot_, AGENT_PATH, "org.bluez.Agent1", AGENT_VTABLE, this);
  if (r < 0) {
    ESP_LOGW(TAG, "agent vtable export failed: %s", std::strerror(-r));
    return;
  }
  // RegisterAgent (NOT RequestDefaultAgent — never hijack the system agent).
  // Capability "NoInputNoOutput": this is a headless host with no display/keyboard,
  // so SMP must use the "Just Works" association model. SMP picks Just Works whenever
  // EITHER peer is NoInputNoOutput, so this also keeps working against peers that have
  // IO capability (e.g. an ESP32, or another BlueZ host that would otherwise negotiate
  // Numeric Comparison — which would stall with no human to confirm the digits and
  // fail as "Numeric comparison failed", tearing the link down before service
  // discovery). The passkey/confirmation agent methods remain exported for callers
  // that explicitly drive pairing via passkey_reply()/confirm_reply().
  sd_bus_error err = SD_BUS_ERROR_NULL;
  r = sd_bus_call_method(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent", &err, nullptr,
                         "os", AGENT_PATH, "NoInputNoOutput");
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAgent failed: %s", err.message ? err.message : std::strerror(-r));
    sd_bus_error_free(&err);
    return;
  }
  sd_bus_error_free(&err);
  this->agent_registered_ = true;
  ESP_LOGI(TAG, "registered non-default pairing agent (system agent untouched)");
}

int BLEGattHostThread::agent_release_(sd_bus_message *m, void * /*ud*/, sd_bus_error * /*e*/) {
  return sd_bus_reply_method_return(m, "");
}
int BLEGattHostThread::agent_cancel_(sd_bus_message *m, void * /*ud*/, sd_bus_error * /*e*/) {
  return sd_bus_reply_method_return(m, "");
}
int BLEGattHostThread::agent_request_passkey_(sd_bus_message *m, void *ud, sd_bus_error * /*e*/) {
  auto *self = static_cast<BLEGattHostThread *>(ud);
  const char *dev = nullptr;
  sd_bus_message_read(m, "o", &dev);
  auto *host = self->find_host_by_device_(dev);
  if (host == nullptr)
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "no host for %s", dev ? dev : "?");
  host->agent_request_passkey(m);  // parks the reply; answered via passkey_reply
  return 1;                        // tell sd-bus the reply is deferred
}
int BLEGattHostThread::agent_display_passkey_(sd_bus_message *m, void *ud, sd_bus_error * /*e*/) {
  auto *self = static_cast<BLEGattHostThread *>(ud);
  const char *dev = nullptr;
  uint32_t passkey = 0;
  uint16_t entered = 0;
  sd_bus_message_read(m, "ouq", &dev, &passkey, &entered);
  auto *host = self->find_host_by_device_(dev);
  if (host != nullptr)
    host->agent_display_passkey(passkey);
  return sd_bus_reply_method_return(m, "");
}
int BLEGattHostThread::agent_request_confirmation_(sd_bus_message *m, void *ud, sd_bus_error * /*e*/) {
  auto *self = static_cast<BLEGattHostThread *>(ud);
  const char *dev = nullptr;
  uint32_t passkey = 0;
  sd_bus_message_read(m, "ou", &dev, &passkey);
  auto *host = self->find_host_by_device_(dev);
  if (host == nullptr)
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "no host for %s", dev ? dev : "?");
  host->agent_request_confirmation(m, passkey);  // parks the reply; answered via confirm_reply
  return 1;                                       // deferred reply
}

// ---------------------------------------------------------------------------
// BLEGattHost
// ---------------------------------------------------------------------------

BLEGattHost::BLEGattHost(std::string adapter, std::string device_path)
    : adapter_(std::move(adapter)), device_path_(std::move(device_path)) {}

BLEGattHost::~BLEGattHost() {
  BLEGattHostThread::instance().detach(this);
  this->close_bus_();
}

void BLEGattHost::enqueue_command_(HostGattCommand cmd) {
  {
    std::lock_guard<std::mutex> g(this->cmd_mu_);
    this->commands_.push_back(std::move(cmd));
  }
  BLEGattHostThread::instance().wake();
}

void BLEGattHost::connect() {
  BLEGattHostThread::instance().attach(this);  // idempotent registration
  this->enqueue_command_({HostGattCommand::Kind::CONNECT});
}

void BLEGattHost::disconnect() { this->enqueue_command_({HostGattCommand::Kind::DISCONNECT}); }

void BLEGattHost::read_char(uint16_t handle) {
  HostGattCommand c{HostGattCommand::Kind::READ_CHAR};
  c.handle = handle;
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::read_desc(uint16_t handle) {
  HostGattCommand c{HostGattCommand::Kind::READ_DESC};
  c.handle = handle;
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::write_char(uint16_t handle, const uint8_t *data, size_t len, bool response) {
  HostGattCommand c{HostGattCommand::Kind::WRITE_CHAR};
  c.handle = handle;
  c.flag = response;
  c.data.assign(data, data + len);
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::write_desc(uint16_t handle, const uint8_t *data, size_t len, bool response) {
  HostGattCommand c{HostGattCommand::Kind::WRITE_DESC};
  c.handle = handle;
  c.flag = response;
  c.data.assign(data, data + len);
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::set_notify(uint16_t handle, bool enable) {
  HostGattCommand c{HostGattCommand::Kind::SET_NOTIFY};
  c.handle = handle;
  c.flag = enable;
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::read_rssi() { this->enqueue_command_({HostGattCommand::Kind::READ_RSSI}); }
void BLEGattHost::pair() { this->enqueue_command_({HostGattCommand::Kind::PAIR}); }
void BLEGattHost::passkey_reply(uint32_t passkey) {
  HostGattCommand c{HostGattCommand::Kind::PASSKEY_REPLY};
  c.passkey = passkey;
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::confirm_reply(bool accept) {
  HostGattCommand c{HostGattCommand::Kind::CONFIRM_REPLY};
  c.flag = accept;
  this->enqueue_command_(std::move(c));
}
void BLEGattHost::remove_bond() { this->enqueue_command_({HostGattCommand::Kind::REMOVE_BOND}); }

std::deque<HostGattEvent> BLEGattHost::drain_events() {
  std::deque<HostGattEvent> out;
  std::lock_guard<std::mutex> g(this->evt_mu_);
  out.swap(this->events_);
  return out;
}

void BLEGattHost::post_event_(HostGattEvent &&ev) {
  std::lock_guard<std::mutex> g(this->evt_mu_);
  this->events_.push_back(std::move(ev));
}

// --- worker thread ---

void BLEGattHost::worker_process_commands() {
  std::deque<HostGattCommand> cmds;
  {
    std::lock_guard<std::mutex> g(this->cmd_mu_);
    cmds.swap(this->commands_);
  }
  for (auto &c : cmds) {
    switch (c.kind) {
      case HostGattCommand::Kind::CONNECT:
        this->do_connect_();
        break;
      case HostGattCommand::Kind::DISCONNECT:
        this->do_disconnect_();
        break;
      case HostGattCommand::Kind::READ_CHAR:
        this->do_read_(c.handle, false);
        break;
      case HostGattCommand::Kind::READ_DESC:
        this->do_read_(c.handle, true);
        break;
      case HostGattCommand::Kind::WRITE_CHAR:
        this->do_write_(c.handle, c.data, c.flag, false);
        break;
      case HostGattCommand::Kind::WRITE_DESC:
        this->do_write_(c.handle, c.data, c.flag, true);
        break;
      case HostGattCommand::Kind::SET_NOTIFY:
        this->do_set_notify_(c.handle, c.flag);
        break;
      case HostGattCommand::Kind::READ_RSSI:
        this->do_read_rssi_();
        break;
      case HostGattCommand::Kind::PAIR:
        this->do_pair_();
        break;
      case HostGattCommand::Kind::PASSKEY_REPLY:
        this->do_passkey_reply_(c.passkey);
        break;
      case HostGattCommand::Kind::CONFIRM_REPLY:
        this->do_confirm_reply_(c.flag);
        break;
      case HostGattCommand::Kind::REMOVE_BOND:
        this->do_remove_bond_();
        break;
    }
  }
}

bool BLEGattHost::open_bus_() {
  if (this->bus_open_.load())
    return true;
  int r = sd_bus_open_system(&this->bus_);
  if (r < 0) {
    ESP_LOGW(TAG, "open system bus failed: %s", std::strerror(-r));
    return false;
  }
  // Unix-fd negotiation is needed later for AcquireWrite/Notify MTU probing.
  sd_bus_negotiate_fds(this->bus_, 1);

  // Attach this bus to the shared event loop so its replies/signals are serviced.
  sd_event *ev = BLEGattHostThread::instance().event();
  if (ev != nullptr)
    sd_bus_attach_event(this->bus_, ev, 0);

  // Subscribe to PropertiesChanged on this device path (for Connected changes).
  std::string match = "type='signal',interface='org.freedesktop.DBus.Properties',"
                      "member='PropertiesChanged',path='" +
                      this->device_path_ + "'";
  r = sd_bus_add_match(this->bus_, &this->props_slot_, match.c_str(), &BLEGattHost::on_properties_changed_, this);
  if (r < 0) {
    ESP_LOGW(TAG, "add_match failed: %s", std::strerror(-r));
  }
  this->bus_open_ = true;
  return true;
}

void BLEGattHost::close_bus_() {
  if (this->props_slot_ != nullptr) {
    sd_bus_slot_unref(this->props_slot_);
    this->props_slot_ = nullptr;
  }
  if (this->bus_ != nullptr) {
    sd_bus_flush_close_unref(this->bus_);
    this->bus_ = nullptr;
  }
  this->bus_open_ = false;
}

void BLEGattHost::do_connect_() {
  if (!this->open_bus_())
    return;
  // Check current Connected state first (don't trust cache); if already up,
  // synthesize a CONNECTED event.
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int connected = 0;
  int r = sd_bus_get_property_trivial(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1",
                                      "Connected", &err, 'b', &connected);
  sd_bus_error_free(&err);
  if (r >= 0 && connected) {
    this->worker_on_connected_changed(true);
    return;
  }

  // Device1.Connect() blocks until the link is up (or fails). On success the
  // Connected=true PropertiesChanged has already fired (→ CONNECTED). We retry
  // once on the well-known transient "le-connection-abort-by-local"; only a
  // genuine, non-recoverable failure posts DISCONNECTED — and only if the
  // signal handler hasn't already reported the device connected.
  for (int attempt = 0; attempt < 2; attempt++) {
    sd_bus_error cerr = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", "Connect", &cerr,
                           nullptr, "");
    if (r >= 0) {
      sd_bus_error_free(&cerr);
      return;  // connected; CONNECTED already posted by the Connected signal
    }
    const char *name = cerr.name ? cerr.name : "";
    const char *msg = cerr.message ? cerr.message : "";
    bool abort_local = (std::strstr(msg, "le-connection-abort-by-local") != nullptr);
    bool not_found = (std::strstr(name, "UnknownObject") != nullptr) || (std::strstr(name, "DoesNotExist") != nullptr);
    bool already_connected = (std::strstr(name, "AlreadyConnected") != nullptr);
    ESP_LOGW(TAG, "Connect(%s) attempt %d: %s", this->device_path_.c_str(), attempt, msg[0] ? msg : std::strerror(-r));
    sd_bus_error_free(&cerr);

    if (already_connected) {
      this->worker_on_connected_changed(true);
      return;
    }
    if (abort_local && attempt == 0) {
      continue;  // transient: retry once
    }
    // Genuine failure. Only post DISCONNECTED if BlueZ doesn't already consider
    // the device connected (avoid racing the Connected signal).
    sd_bus_error perr = SD_BUS_ERROR_NULL;
    int connected2 = 0;
    sd_bus_get_property_trivial(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", "Connected",
                                &perr, 'b', &connected2);
    sd_bus_error_free(&perr);
    if (connected2) {
      this->worker_on_connected_changed(true);
    } else {
      HostGattEvent ev;
      ev.kind = HostGattEvent::Kind::DISCONNECTED;
      ev.disc_reason = not_found ? 0x0a /*NOT_FOUND*/ : 0x85 /*ERROR*/;
      this->post_event_(std::move(ev));
    }
    return;
  }
}

void BLEGattHost::do_disconnect_() {
  if (!this->bus_open_.load()) {
    // Already torn down — report disconnected so the client reaches IDLE.
    HostGattEvent ev;
    ev.kind = HostGattEvent::Kind::DISCONNECTED;
    this->post_event_(std::move(ev));
    return;
  }
  sd_bus_error err = SD_BUS_ERROR_NULL;
  // Fire-and-forget; ignore UNKNOWN_OBJECT (already gone). The Connected=false
  // signal posts DISCONNECTED.
  sd_bus_call_method(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", "Disconnect", &err,
                     nullptr, "");
  sd_bus_error_free(&err);
}

namespace {

// Map a BlueZ GattCharacteristic1 "Flags" string to an esp_gatt_char_prop_t bit.
uint8_t flag_to_prop(const char *flag) {
  if (std::strcmp(flag, "broadcast") == 0)
    return ESP_GATT_CHAR_PROP_BIT_BROADCAST;
  if (std::strcmp(flag, "read") == 0)
    return ESP_GATT_CHAR_PROP_BIT_READ;
  if (std::strcmp(flag, "write-without-response") == 0)
    return ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
  if (std::strcmp(flag, "write") == 0)
    return ESP_GATT_CHAR_PROP_BIT_WRITE;
  if (std::strcmp(flag, "notify") == 0)
    return ESP_GATT_CHAR_PROP_BIT_NOTIFY;
  if (std::strcmp(flag, "indicate") == 0)
    return ESP_GATT_CHAR_PROP_BIT_INDICATE;
  if (std::strcmp(flag, "authenticated-signed-writes") == 0)
    return ESP_GATT_CHAR_PROP_BIT_AUTH;
  if (std::strcmp(flag, "extended-properties") == 0)
    return ESP_GATT_CHAR_PROP_BIT_EXT_PROP;
  return 0;
}

// A flat parsed GATT object from GetManagedObjects.
struct GattObj {
  std::string path;
  enum { SERVICE, CHAR, DESC, OTHER } kind{OTHER};
  espbt::ESPBTUUID uuid;
  uint16_t handle{0};
  bool has_handle{false};
  std::string parent;  // Service (for char) / Characteristic (for descr)
  uint8_t props{0};    // chars only
};

// Read one a{sv} property dict for a single interface, filling a GattObj.
void parse_gatt_iface_props(sd_bus_message *m, const char *iface, GattObj &obj) {
  bool is_char = std::strcmp(iface, "org.bluez.GattCharacteristic1") == 0;
  bool is_desc = std::strcmp(iface, "org.bluez.GattDescriptor1") == 0;
  bool is_svc = std::strcmp(iface, "org.bluez.GattService1") == 0;
  if (is_char)
    obj.kind = GattObj::CHAR;
  else if (is_desc)
    obj.kind = GattObj::DESC;
  else if (is_svc)
    obj.kind = GattObj::SERVICE;

  sd_bus_message_enter_container(m, 'a', "{sv}");
  for (;;) {
    if (sd_bus_message_enter_container(m, 'e', "sv") <= 0)
      break;
    const char *key = nullptr;
    sd_bus_message_read(m, "s", &key);
    if (key != nullptr && std::strcmp(key, "UUID") == 0) {
      const char *uuid = nullptr;
      sd_bus_message_read(m, "v", "s", &uuid);
      if (uuid != nullptr)
        obj.uuid = espbt::ESPBTUUID::from_uuid_str(uuid);
    } else if (key != nullptr && std::strcmp(key, "Handle") == 0) {
      uint16_t h = 0;
      if (sd_bus_message_read(m, "v", "q", &h) >= 0) {
        obj.handle = h;
        obj.has_handle = true;
      }
    } else if (key != nullptr && (std::strcmp(key, "Service") == 0 || std::strcmp(key, "Characteristic") == 0)) {
      const char *p = nullptr;
      sd_bus_message_read(m, "v", "o", &p);
      if (p != nullptr)
        obj.parent = p;
    } else if (key != nullptr && is_char && std::strcmp(key, "Flags") == 0) {
      sd_bus_message_enter_container(m, 'v', "as");
      sd_bus_message_enter_container(m, 'a', "s");
      const char *flag = nullptr;
      while (sd_bus_message_read(m, "s", &flag) > 0)
        if (flag != nullptr)
          obj.props |= flag_to_prop(flag);
      sd_bus_message_exit_container(m);
      sd_bus_message_exit_container(m);
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);  // dict entry
  }
  sd_bus_message_exit_container(m);  // a{sv}
}

}  // namespace

bool BLEGattHost::walk_gatt_tree_(std::vector<DiscoveredService> &out) {
  if (this->bus_ == nullptr)
    return false;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call_method(this->bus_, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                             &err, &reply, "");
  if (r < 0) {
    ESP_LOGW(TAG, "GetManagedObjects failed: %s", err.message ? err.message : std::strerror(-r));
    sd_bus_error_free(&err);
    return false;
  }

  std::vector<GattObj> objs;
  // reply: a{oa{sa{sv}}}
  sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  for (;;) {
    if (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") <= 0)
      break;
    const char *path = nullptr;
    sd_bus_message_read(reply, "o", &path);
    bool under_device =
        (path != nullptr && std::strncmp(path, this->device_path_.c_str(), this->device_path_.size()) == 0 &&
         path[this->device_path_.size()] == '/');
    GattObj obj;
    if (path != nullptr)
      obj.path = path;
    sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    for (;;) {
      if (sd_bus_message_enter_container(reply, 'e', "sa{sv}") <= 0)
        break;
      const char *iface = nullptr;
      sd_bus_message_read(reply, "s", &iface);
      bool is_gatt = iface != nullptr && (std::strcmp(iface, "org.bluez.GattService1") == 0 ||
                                          std::strcmp(iface, "org.bluez.GattCharacteristic1") == 0 ||
                                          std::strcmp(iface, "org.bluez.GattDescriptor1") == 0);
      if (under_device && is_gatt) {
        parse_gatt_iface_props(reply, iface, obj);
      } else {
        sd_bus_message_skip(reply, "a{sv}");
      }
      sd_bus_message_exit_container(reply);  // interface entry
    }
    sd_bus_message_exit_container(reply);  // a{sa{sv}}
    if (under_device && obj.kind != GattObj::OTHER)
      objs.push_back(std::move(obj));
    sd_bus_message_exit_container(reply);  // object entry
  }
  sd_bus_message_exit_container(reply);  // top array
  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);

  // Assemble the tree by parent object-path links. Services first.
  this->handle_map_.clear();
  for (const auto &o : objs) {
    if (o.kind != GattObj::SERVICE)
      continue;
    DiscoveredService svc;
    svc.uuid = o.uuid;
    svc.start_handle = o.handle;
    svc.end_handle = o.handle;
    // characteristics whose parent Service == this path
    for (const auto &c : objs) {
      if (c.kind != GattObj::CHAR || c.parent != o.path)
        continue;
      if (!c.has_handle) {
        ESP_LOGE(TAG, "characteristic %s has no Handle", c.path.c_str());
        return false;  // hard requirement
      }
      DiscoveredCharacteristic dc;
      dc.uuid = c.uuid;
      dc.handle = c.handle;
      dc.properties = c.props;
      this->handle_map_[c.handle] = ObjEntry{c.path, ObjEntry::CHAR, c.handle};
      for (const auto &d : objs) {
        if (d.kind != GattObj::DESC || d.parent != c.path)
          continue;
        if (!d.has_handle) {
          ESP_LOGE(TAG, "descriptor %s has no Handle", d.path.c_str());
          return false;
        }
        dc.descriptors.push_back(DiscoveredDescriptor{d.uuid, d.handle});
        this->handle_map_[d.handle] = ObjEntry{d.path, ObjEntry::DESC, c.handle};
      }
      svc.characteristics.push_back(std::move(dc));
    }
    out.push_back(std::move(svc));
  }
  // BlueZ <5.79 doesn't export the GAP service (0x1800); a stock ESP32 proxy
  // reports it. Rebuild it from Device1 props so GATTGetServices matches.
  this->synthesize_gap_service_(out);
  ESP_LOGD(TAG, "discovered %zu services on %s", out.size(), this->device_path_.c_str());
  return true;
}

// Synthetic GAP (0x1800), only if BlueZ didn't already export one (it does on
// >=5.79 with ExportClaimedServices). Read-only; values from Device1:
//   0x2a00 Device Name  <- Device1.Name
//   0x2a01 Appearance   <- Device1.Appearance (2-byte LE; default 0 = Unknown)
//   0x2aa6 Central Address Resolution (0x01 = supported)
// We deliberately do NOT synthesize 0x2a04 (Peripheral Preferred Connection
// Parameters): it is optional and device-specific, BlueZ gives us no signal for
// it, and a real ESP32 proxy only reports the characteristics the peripheral
// actually exposes. The C6 oracle's GAP has exactly 2a00/2a01/2aa6 — match that.
// Synthetic handles live in a reserved high range so they never collide with
// real BlueZ ATT handles. do_read_ serves them from synthetic_reads_.
void BLEGattHost::synthesize_gap_service_(std::vector<DiscoveredService> &out) {
  this->synthetic_reads_.clear();
  static constexpr uint16_t GAP_UUID = 0x1800;
  for (const auto &s : out) {
    if (s.uuid == BLEUUID::from_uint16(GAP_UUID))
      return;  // BlueZ exported a real 0x1800 — keep its true handles, don't duplicate
  }
  if (this->bus_ == nullptr)
    return;

  // Device Name (0x2a00) <- Device1.Name (falls back to Alias if Name absent).
  std::vector<uint8_t> name_bytes;
  for (const char *prop : {"Name", "Alias"}) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *name = nullptr;
    int r = sd_bus_get_property_string(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", prop,
                                       &err, &name);
    sd_bus_error_free(&err);
    if (r >= 0 && name != nullptr && name[0] != '\0') {
      name_bytes.assign(name, name + std::strlen(name));
      free(name);
      break;
    }
    free(name);
  }

  // Appearance (0x2a01) <- Device1.Appearance (uint16, little-endian on the wire).
  uint16_t appearance = 0;
  {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_get_property_trivial(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", "Appearance",
                                &err, 'q', &appearance);
    sd_bus_error_free(&err);
  }

  static constexpr uint16_t H_SVC = 0xFF00;
  static constexpr uint16_t H_NAME = 0xFF01;
  static constexpr uint16_t H_APPEARANCE = 0xFF02;
  static constexpr uint16_t H_CAR = 0xFF03;
  static constexpr uint8_t PROP_READ = 0x02;  // esp_gatt_char_prop_t READ

  DiscoveredService gap;
  gap.uuid = BLEUUID::from_uint16(GAP_UUID);
  gap.start_handle = H_SVC;
  gap.end_handle = H_CAR;

  auto add_char = [&](uint16_t handle, uint16_t uuid16, std::vector<uint8_t> value) {
    DiscoveredCharacteristic dc;
    dc.uuid = BLEUUID::from_uint16(uuid16);
    dc.handle = handle;
    dc.properties = PROP_READ;
    gap.characteristics.push_back(std::move(dc));
    this->synthetic_reads_[handle] = std::move(value);
  };
  add_char(H_NAME, 0x2A00, std::move(name_bytes));
  add_char(H_APPEARANCE, 0x2A01, {(uint8_t) (appearance & 0xFF), (uint8_t) (appearance >> 8)});
  add_char(H_CAR, 0x2AA6, {0x01});  // Central Address Resolution: supported

  ESP_LOGD(TAG, "synthesized GAP service 0x1800 (3 chars) from Device1 props");
  out.push_back(std::move(gap));
}

uint16_t BLEGattHost::acquire_mtu_() {
  // Read GattCharacteristic1.MTU off any characteristic (BlueZ >= 5.62).
  for (const auto &kv : this->handle_map_) {
    if (kv.second.kind != ObjEntry::CHAR)
      continue;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    uint16_t mtu = 0;
    int r = sd_bus_get_property_trivial(this->bus_, "org.bluez", kv.second.path.c_str(),
                                        "org.bluez.GattCharacteristic1", "MTU", &err, 'q', &mtu);
    sd_bus_error_free(&err);
    if (r >= 0 && mtu >= 23)
      return mtu;
  }
  return 23;  // fallback (the AcquireWrite probe lands in a later refinement)
}

void BLEGattHost::do_read_rssi_() {
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::RSSI;
  ev.rssi = 0;
  if (this->bus_ != nullptr) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int16_t rssi = 0;
    int r = sd_bus_get_property_trivial(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1",
                                        "RSSI", &err, 'n', &rssi);
    sd_bus_error_free(&err);
    if (r >= 0)
      ev.rssi = static_cast<int8_t>(rssi);
  }
  this->post_event_(std::move(ev));
}

void BLEGattHost::do_pair_() {
  if (this->bus_ == nullptr)
    return;
  // Ensure our non-default agent is registered before pairing.
  BLEGattHostThread::instance().ensure_agent(this->bus_);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1", "Pair", &err,
                             nullptr, "");
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::PAIRING_COMPLETE;
  if (r < 0) {
    const char *name = err.name ? err.name : "";
    bool already = std::strstr(name, "AlreadyExists") != nullptr;
    ev.pairing_success = already;
    if (!already)
      ESP_LOGW(TAG, "Pair(%s) failed: %s", this->device_path_.c_str(), err.message ? err.message : std::strerror(-r));
  } else {
    ev.pairing_success = true;
  }
  sd_bus_error_free(&err);
  this->post_event_(std::move(ev));
}

void BLEGattHost::do_passkey_reply_(uint32_t passkey) {
  if (this->pending_agent_reply_ == nullptr)
    return;
  sd_bus_message *reply = this->pending_agent_reply_;
  this->pending_agent_reply_ = nullptr;
  // RequestPasskey returns a uint32.
  sd_bus_message *resp = nullptr;
  sd_bus_message_new_method_return(reply, &resp);
  sd_bus_message_append(resp, "u", passkey);
  sd_bus_send(this->bus_, resp, nullptr);
  sd_bus_message_unref(resp);
  sd_bus_message_unref(reply);
}

void BLEGattHost::do_confirm_reply_(bool accept) {
  if (this->pending_agent_reply_ == nullptr)
    return;
  sd_bus_message *reply = this->pending_agent_reply_;
  this->pending_agent_reply_ = nullptr;
  if (accept) {
    // RequestConfirmation returns void on accept.
    sd_bus_message *resp = nullptr;
    sd_bus_message_new_method_return(reply, &resp);
    sd_bus_send(this->bus_, resp, nullptr);
    sd_bus_message_unref(resp);
  } else {
    sd_bus_reply_method_errorf(reply, "org.bluez.Error.Rejected", "rejected");
  }
  sd_bus_message_unref(reply);
}

void BLEGattHost::do_remove_bond_() {
  if (this->bus_ == nullptr)
    return;
  // RemoveDevice on the adapter drops the bond.
  std::string adapter_path = "/org/bluez/" + this->adapter_;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_call_method(this->bus_, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", "RemoveDevice", &err, nullptr,
                     "o", this->device_path_.c_str());
  sd_bus_error_free(&err);
}

void BLEGattHost::agent_request_passkey(sd_bus_message *reply) {
  this->pending_agent_reply_ = sd_bus_message_ref(reply);
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::PASSKEY_REQUEST;
  this->post_event_(std::move(ev));
}
void BLEGattHost::agent_display_passkey(uint32_t passkey) {
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::PASSKEY_NOTIFY;
  ev.passkey = passkey;
  this->post_event_(std::move(ev));
}
void BLEGattHost::agent_request_confirmation(sd_bus_message *reply, uint32_t passkey) {
  this->pending_agent_reply_ = sd_bus_message_ref(reply);
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::NUMERIC_COMPARE;
  ev.passkey = passkey;
  this->post_event_(std::move(ev));
}

namespace {
// Append the standard options dict ({} — no offset) to a Read/WriteValue call.
void append_empty_options(sd_bus_message *m) {
  sd_bus_message_open_container(m, 'a', "{sv}");
  sd_bus_message_close_container(m);
}
// Copy a 'ay' byte array out of a reply/message into a fresh owned buffer.
std::unique_ptr<uint8_t[]> read_ay(sd_bus_message *m, uint16_t &len_out) {
  const void *data = nullptr;
  size_t len = 0;
  if (sd_bus_message_read_array(m, 'y', &data, &len) < 0) {
    len_out = 0;
    return nullptr;
  }
  len_out = static_cast<uint16_t>(len);
  if (len == 0)
    return nullptr;
  auto buf = std::make_unique<uint8_t[]>(len);
  std::memcpy(buf.get(), data, len);
  return buf;
}
}  // namespace

void BLEGattHost::do_read_(uint16_t handle, bool is_desc) {
  // Synthetic GAP (0x1800) characteristics have no BlueZ object — serve the
  // value we cached from Device1 props at discovery time.
  if (!is_desc) {
    auto sit = this->synthetic_reads_.find(handle);
    if (sit != this->synthetic_reads_.end()) {
      HostGattEvent ev;
      ev.kind = HostGattEvent::Kind::READ_COMPLETE;
      ev.handle = handle;
      ev.status = ESP_GATT_OK;
      ev.len = (uint16_t) sit->second.size();
      if (ev.len > 0) {
        ev.data = std::make_unique<uint8_t[]>(ev.len);
        std::memcpy(ev.data.get(), sit->second.data(), ev.len);
      }
      this->post_event_(std::move(ev));
      return;
    }
  }
  auto it = this->handle_map_.find(handle);
  if (it == this->handle_map_.end()) {
    HostGattEvent ev;
    ev.kind = is_desc ? HostGattEvent::Kind::DESC_READ : HostGattEvent::Kind::READ_COMPLETE;
    ev.handle = handle;
    ev.status = ESP_GATT_INVALID_HANDLE;
    this->post_event_(std::move(ev));
    return;
  }
  const char *iface = (it->second.kind == ObjEntry::DESC) ? "org.bluez.GattDescriptor1" : "org.bluez.GattCharacteristic1";
  sd_bus_message *call = nullptr;
  sd_bus_message_new_method_call(this->bus_, &call, "org.bluez", it->second.path.c_str(), iface, "ReadValue");
  append_empty_options(call);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call(this->bus_, call, 0, &err, &reply);
  sd_bus_message_unref(call);

  HostGattEvent ev;
  ev.kind = is_desc ? HostGattEvent::Kind::DESC_READ : HostGattEvent::Kind::READ_COMPLETE;
  ev.handle = handle;
  if (r < 0) {
    ev.status = ESP_GATT_ERROR;
    ESP_LOGW(TAG, "ReadValue(0x%04X) failed: %s", handle, err.message ? err.message : std::strerror(-r));
  } else {
    ev.status = ESP_GATT_OK;
    ev.data = read_ay(reply, ev.len);
  }
  sd_bus_error_free(&err);
  if (reply != nullptr)
    sd_bus_message_unref(reply);
  this->post_event_(std::move(ev));
}

void BLEGattHost::do_write_(uint16_t handle, const std::vector<uint8_t> &data, bool response, bool is_desc) {
  // CCCD interception: a write of {0x01,0x00}/{0x02,0x00} to a 0x2902 descriptor
  // → StartNotify on the parent char; {0x00,0x00} → StopNotify. BlueZ owns CCCD.
  auto it = this->handle_map_.find(handle);
  HostGattEvent ev;
  ev.kind = is_desc ? HostGattEvent::Kind::DESC_WRITE : HostGattEvent::Kind::WRITE_COMPLETE;
  ev.handle = handle;
  if (it == this->handle_map_.end()) {
    ev.status = ESP_GATT_INVALID_HANDLE;
    this->post_event_(std::move(ev));
    return;
  }
  const char *iface = (it->second.kind == ObjEntry::DESC) ? "org.bluez.GattDescriptor1" : "org.bluez.GattCharacteristic1";
  sd_bus_message *call = nullptr;
  sd_bus_message_new_method_call(this->bus_, &call, "org.bluez", it->second.path.c_str(), iface, "WriteValue");
  sd_bus_message_append_array(call, 'y', data.data(), data.size());
  // options: {"type": "request"|"command"}
  sd_bus_message_open_container(call, 'a', "{sv}");
  sd_bus_message_open_container(call, 'e', "sv");
  sd_bus_message_append(call, "s", "type");
  sd_bus_message_append(call, "v", "s", response ? "request" : "command");
  sd_bus_message_close_container(call);
  sd_bus_message_close_container(call);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call(this->bus_, call, 0, &err, nullptr);
  sd_bus_message_unref(call);
  // Always post WRITE_COMPLETE (both write types) so action chains never hang.
  ev.status = (r < 0) ? ESP_GATT_ERROR : ESP_GATT_OK;
  if (r < 0)
    ESP_LOGW(TAG, "WriteValue(0x%04X) failed: %s", handle, err.message ? err.message : std::strerror(-r));
  sd_bus_error_free(&err);
  this->post_event_(std::move(ev));
}

void BLEGattHost::do_set_notify_(uint16_t handle, bool enable) {
  auto it = this->handle_map_.find(handle);
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::NOTIFY_REGISTERED;
  ev.handle = handle;
  if (it == this->handle_map_.end() || it->second.kind != ObjEntry::CHAR) {
    ev.status = ESP_GATT_INVALID_HANDLE;
    this->post_event_(std::move(ev));
    return;
  }
  const std::string &path = it->second.path;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(this->bus_, "org.bluez", path.c_str(), "org.bluez.GattCharacteristic1",
                             enable ? "StartNotify" : "StopNotify", &err, nullptr, "");
  if (r < 0) {
    ESP_LOGW(TAG, "%s(0x%04X) failed: %s", enable ? "StartNotify" : "StopNotify", handle,
             err.message ? err.message : std::strerror(-r));
    ev.status = ESP_GATT_ERROR;
  } else {
    ev.status = ESP_GATT_OK;
    if (enable) {
      this->subscribe_value_(path, handle);
    } else {
      auto sub = this->notify_subs_.find(path);
      if (sub != this->notify_subs_.end()) {
        if (sub->second.slot != nullptr)
          sd_bus_slot_unref(sub->second.slot);
        this->notify_subs_.erase(sub);
      }
    }
  }
  sd_bus_error_free(&err);
  this->post_event_(std::move(ev));
}

void BLEGattHost::subscribe_value_(const std::string &char_path, uint16_t handle) {
  if (this->notify_subs_.count(char_path))
    return;  // already subscribed
  NotifySub sub{handle, nullptr};
  std::string match = "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='" +
                      char_path + "'";
  // userdata is `this`; the handler maps path→handle via notify_subs_.
  sd_bus_add_match(this->bus_, &sub.slot, match.c_str(), &BLEGattHost::on_value_changed_, this);
  this->notify_subs_[char_path] = sub;
}

int BLEGattHost::on_value_changed_(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<BLEGattHost *>(userdata);
  const char *path = sd_bus_message_get_path(m);
  if (path == nullptr)
    return 0;
  auto sub = self->notify_subs_.find(path);
  if (sub == self->notify_subs_.end())
    return 0;
  uint16_t handle = sub->second.handle;

  const char *iface = nullptr;
  if (sd_bus_message_read(m, "s", &iface) < 0)
    return 0;
  if (iface == nullptr || std::strcmp(iface, "org.bluez.GattCharacteristic1") != 0)
    return 0;
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
    return 0;
  for (;;) {
    if (sd_bus_message_enter_container(m, 'e', "sv") <= 0)
      break;
    const char *key = nullptr;
    sd_bus_message_read(m, "s", &key);
    if (key != nullptr && std::strcmp(key, "Value") == 0) {
      sd_bus_message_enter_container(m, 'v', "ay");
      HostGattEvent ev;
      ev.kind = HostGattEvent::Kind::NOTIFY;
      ev.handle = handle;
      ev.data = read_ay(m, ev.len);
      sd_bus_message_exit_container(m);
      self->post_event_(std::move(ev));
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);
  }
  sd_bus_message_exit_container(m);
  return 0;
}

int BLEGattHost::on_properties_changed_(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<BLEGattHost *>(userdata);
  const char *iface = nullptr;
  if (sd_bus_message_read(m, "s", &iface) < 0)
    return 0;
  if (iface == nullptr || std::strcmp(iface, "org.bluez.Device1") != 0)
    return 0;

  // changed: a{sv}
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
    return 0;
  for (;;) {
    int r = sd_bus_message_enter_container(m, 'e', "sv");
    if (r <= 0)
      break;
    const char *key = nullptr;
    sd_bus_message_read(m, "s", &key);
    if (key != nullptr && std::strcmp(key, "Connected") == 0) {
      int connected = 0;
      sd_bus_message_read(m, "v", "b", &connected);
      self->worker_on_connected_changed(connected != 0);
    } else if (key != nullptr && std::strcmp(key, "ServicesResolved") == 0) {
      int resolved = 0;
      sd_bus_message_read(m, "v", "b", &resolved);
      if (resolved)
        self->try_start_discovery_();
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);
  }
  sd_bus_message_exit_container(m);
  return 0;
}

void BLEGattHost::worker_on_connected_changed(bool connected) {
  HostGattEvent ev;
  if (connected) {
    ev.kind = HostGattEvent::Kind::CONNECTED;
    this->post_event_(std::move(ev));
    // ServicesResolved may already be true (the signal won't re-fire); poll it.
    if (this->bus_ != nullptr) {
      sd_bus_error err = SD_BUS_ERROR_NULL;
      int resolved = 0;
      int r = sd_bus_get_property_trivial(this->bus_, "org.bluez", this->device_path_.c_str(), "org.bluez.Device1",
                                          "ServicesResolved", &err, 'b', &resolved);
      sd_bus_error_free(&err);
      if (r >= 0 && resolved)
        this->try_start_discovery_();
    }
  } else {
    this->discovered_ = false;
    this->discovering_ = false;
    this->handle_map_.clear();
    ev.kind = HostGattEvent::Kind::DISCONNECTED;
    this->post_event_(std::move(ev));
  }
}

// Walk the GATT tree once, build the discovery snapshot + handle maps, acquire
// MTU, and post SERVICES_DISCOVERED. Idempotent per connection.
void BLEGattHost::try_start_discovery_() {
  // Guard BOTH the completed state and an in-flight walk. walk_gatt_tree_() and
  // acquire_mtu_() below make blocking sd_bus_call*s that pump this bus and may
  // dispatch a queued ServicesResolved/notify signal re-entrantly on this same
  // worker thread — re-entering here. Without discovering_, the nested call would
  // pass the discovered_==false check and build/free a second copy of the same
  // service tree concurrently with this frame → heap corruption (use-after-free
  // observed crashing at ble_client_base.cpp:169 under the 3-slot proxy).
  if (this->discovered_ || this->discovering_)
    return;
  this->discovering_ = true;
  std::vector<DiscoveredService> tree;
  if (!this->walk_gatt_tree_(tree)) {
    // A GATT object lacked a real Handle (BlueZ < 5.62) — protocol-breaking for
    // the proxy contract. Hard-fail the connection.
    ESP_LOGE(TAG, "GATT object missing Handle (need BlueZ >= 5.62); disconnecting %s",
             this->device_path_.c_str());
    this->discovering_ = false;
    this->do_disconnect_();
    return;
  }
  this->discovered_ = true;
  HostGattEvent ev;
  ev.kind = HostGattEvent::Kind::SERVICES_DISCOVERED;
  ev.mtu = this->acquire_mtu_();
  ev.services = std::move(tree);
  this->discovering_ = false;
  this->post_event_(std::move(ev));
}

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_HOST
