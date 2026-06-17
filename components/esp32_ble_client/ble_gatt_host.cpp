#ifdef USE_HOST

#include "ble_gatt_host.h"

#include "esphome/core/log.h"

#include <cerrno>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

#include <systemd/sd-event.h>

namespace esphome {
namespace esp32_ble_client {

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
    {
      std::lock_guard<std::mutex> g(this->hosts_mu_);
      snapshot = this->hosts_;
    }
    for (auto *h : snapshot)
      h->worker_process_commands();
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

void BLEGattHostThread::wake() {
  if (this->wake_fd_ < 0)
    return;
  uint64_t v = 1;
  ssize_t r = write(this->wake_fd_, &v, sizeof(v));
  (void) r;
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
    this->commands_.push_back(cmd);
  }
  BLEGattHostThread::instance().wake();
}

void BLEGattHost::connect() {
  BLEGattHostThread::instance().attach(this);  // idempotent registration
  this->enqueue_command_({HostGattCommand::Kind::CONNECT});
}

void BLEGattHost::disconnect() { this->enqueue_command_({HostGattCommand::Kind::DISCONNECT}); }

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
  } else {
    ev.kind = HostGattEvent::Kind::DISCONNECTED;
  }
  this->post_event_(std::move(ev));
}

}  // namespace esp32_ble_client
}  // namespace esphome

#endif  // USE_HOST
