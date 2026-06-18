#ifdef USE_HOST

#include "ble_gatt_server.h"
#include "ble_server.h"
#include "ble_service.h"
#include "ble_characteristic.h"
#include "ble_descriptor.h"

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/core/log.h"

#include <cstring>

#include <systemd/sd-event.h>

namespace esphome {
namespace esp32_ble_server {

using esp32_ble::BleWorker;

static const char *const TAG = "esp32_ble_server.gatt";

// ---------------------------------------------------------------------------
// vtables (file scope so they can name the static trampolines)
// ---------------------------------------------------------------------------

static const sd_bus_vtable SERVICE_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", BLEGattServer::svc_get_uuid_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", BLEGattServer::svc_get_primary_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

static const sd_bus_vtable CHAR_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", BLEGattServer::chr_get_uuid_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", BLEGattServer::chr_get_service_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", BLEGattServer::chr_get_flags_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Value", "ay", BLEGattServer::chr_get_value_, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Notifying", "b", BLEGattServer::chr_get_notifying_, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", BLEGattServer::chr_read_value_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", BLEGattServer::chr_write_value_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StartNotify", "", "", BLEGattServer::chr_start_notify_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StopNotify", "", "", BLEGattServer::chr_stop_notify_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

static const sd_bus_vtable DESC_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", BLEGattServer::desc_get_uuid_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Characteristic", "o", BLEGattServer::desc_get_char_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", BLEGattServer::desc_get_flags_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Value", "ay", BLEGattServer::desc_get_value_, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", BLEGattServer::desc_read_value_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", BLEGattServer::desc_write_value_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

static const sd_bus_vtable ADV_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type", "s", BLEGattServer::adv_get_type_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("LocalName", "s", BLEGattServer::adv_get_local_name_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", BLEGattServer::adv_get_service_uuids_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ManufacturerData", "a{qv}", BLEGattServer::adv_get_manufacturer_data_, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceData", "a{sv}", BLEGattServer::adv_get_service_data_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Appearance", "q", BLEGattServer::adv_get_appearance_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "as", BLEGattServer::adv_get_includes_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", BLEGattServer::adv_release_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Render an ESPBTUUID as the lowercase 128-bit dashed string BlueZ expects.
static std::string uuid_to_bluez_str(esp32_ble::ESPBTUUID uuid) {
  char buf[esp32_ble::ESPBTUUID::UUID_STR_LEN];
  uuid.to_str(std::span<char, esp32_ble::ESPBTUUID::UUID_STR_LEN>(buf, esp32_ble::ESPBTUUID::UUID_STR_LEN));
  std::string s(buf);
  for (auto &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Append a std::vector<uint8_t> as a D-Bus "ay" to a reply being constructed by a
// property getter or method return.
static int append_ay(sd_bus_message *reply, const std::vector<uint8_t> &data) {
  return sd_bus_message_append_array(reply, 'y', data.data(), data.size());
}

// ---------------------------------------------------------------------------
// BLEGattServer
// ---------------------------------------------------------------------------

BLEGattServer::~BLEGattServer() {
  if (this->attached_) {
    BleWorker::instance().detach_worker_client(this);
    this->attached_ = false;
  }
  this->unregister_advertisement_();
  if (this->readv_timer_ != nullptr)
    sd_event_source_unref(this->readv_timer_);
  if (this->adv_slot_ != nullptr)
    sd_bus_slot_unref(this->adv_slot_);
  for (auto *s : this->obj_slots_)
    sd_bus_slot_unref(s);
  if (this->om_slot_ != nullptr)
    sd_bus_slot_unref(this->om_slot_);
  if (this->device_watch_slot_ != nullptr)
    sd_bus_slot_unref(this->device_watch_slot_);
  if (this->bus_ != nullptr) {
    sd_bus_flush_close_unref(this->bus_);
    this->bus_ = nullptr;
  }
}

std::string BLEGattServer::adapter_path_() { return "/org/bluez/hci0"; }

bool BLEGattServer::open_bus_() {
  if (this->bus_open_.load())
    return true;
  int r = sd_bus_open_system(&this->bus_);
  if (r < 0) {
    ESP_LOGW(TAG, "open system bus failed: %s", std::strerror(-r));
    return false;
  }
  // Needed for any future AcquireWrite/AcquireNotify (fd-based pipes).
  sd_bus_negotiate_fds(this->bus_, 1);
  sd_event *ev = BleWorker::instance().event();
  if (ev != nullptr) {
    sd_bus_attach_event(this->bus_, ev, 0);
  } else {
    ESP_LOGE(TAG, "open_bus_: worker event loop is NULL — bus NOT attached!");
  }
  this->bus_open_ = true;
  return true;
}

void BLEGattServer::start(const std::vector<BLEService *> &services) {
  // Build the object tree on the main thread (pure data: our own structs + the
  // characteristic back-pointers used by notify_characteristic). The actual bus
  // export + RegisterApplication happen on the worker (see do_start_).
  this->build_tree_(services);

  // Attach to the shared worker so worker_process_commands() runs, then post a
  // START so do_start_() executes on the worker thread.
  BleWorker::instance().attach_worker_client(this);
  this->attached_ = true;
  {
    std::lock_guard<std::mutex> g(this->cmd_mu_);
    this->start_requested_ = true;
  }
  BleWorker::instance().wake();
}

void BLEGattServer::do_start_() {
  if (this->started_)
    return;
  this->started_ = true;

  if (!this->open_bus_()) {
    ESP_LOGE(TAG, "GATT server: no system bus; not registered");
    return;
  }

  // ObjectManager at APP_ROOT — RegisterApplication enumerates from here.
  int r = sd_bus_add_object_manager(this->bus_, &this->om_slot_, APP_ROOT);
  if (r < 0) {
    ESP_LOGE(TAG, "add_object_manager failed: %s", std::strerror(-r));
    return;
  }

  // Export every node's vtable.
  for (auto &svc : this->services_) {
    sd_bus_slot *slot = nullptr;
    r = sd_bus_add_object_vtable(this->bus_, &slot, svc->path.c_str(), "org.bluez.GattService1", SERVICE_VTABLE,
                                 svc.get());
    if (r < 0)
      ESP_LOGW(TAG, "export service %s failed: %s", svc->path.c_str(), std::strerror(-r));
    else
      this->obj_slots_.push_back(slot);
  }
  for (auto &ch : this->chars_) {
    sd_bus_slot *slot = nullptr;
    r = sd_bus_add_object_vtable(this->bus_, &slot, ch->path.c_str(), "org.bluez.GattCharacteristic1", CHAR_VTABLE,
                                 ch.get());
    if (r < 0)
      ESP_LOGW(TAG, "export char %s failed: %s", ch->path.c_str(), std::strerror(-r));
    else
      this->obj_slots_.push_back(slot);
  }
  for (auto &de : this->descs_) {
    sd_bus_slot *slot = nullptr;
    r = sd_bus_add_object_vtable(this->bus_, &slot, de->path.c_str(), "org.bluez.GattDescriptor1", DESC_VTABLE,
                                 de.get());
    if (r < 0)
      ESP_LOGW(TAG, "export desc %s failed: %s", de->path.c_str(), std::strerror(-r));
    else
      this->obj_slots_.push_back(slot);
  }

  // Watch Device1.Connected on the adapter subtree to detect central connects.
  r = sd_bus_match_signal(this->bus_, &this->device_watch_slot_, "org.bluez", nullptr,
                          "org.freedesktop.DBus.Properties", "PropertiesChanged",
                          &BLEGattServer::on_device_props_changed_, this);
  if (r < 0)
    ESP_LOGW(TAG, "device watch match failed: %s", std::strerror(-r));

  this->register_application_();  // async; reply arrives via on_register_app_reply_
}

void BLEGattServer::build_tree_(const std::vector<BLEService *> &services) {
  int svc_idx = 0;
  for (auto *svc : services) {
    auto es = make_unique<ExportedService>();
    es->svc = svc;
    es->path = std::string(APP_ROOT) + "/service" + std::to_string(svc_idx);
    es->uuid_str = uuid_to_bluez_str(svc->get_uuid());
    es->primary = true;
    std::string service_path = es->path;

    int chr_idx = 0;
    for (auto *chr : svc->host_characteristics()) {
      auto ec = make_unique<ExportedChar>();
      ec->chr = chr;
      ec->path = service_path + "/char" + std::to_string(chr_idx);
      ec->service_path = service_path;
      ec->uuid_str = uuid_to_bluez_str(chr->get_uuid());
      ec->properties = chr->host_properties();
      ec->cached_value = chr->get_value();
      // Let the characteristic route notify()/path lookups back to us.
      chr->gatt_server_ = this;
      chr->object_path_ = ec->path;
      std::string char_path = ec->path;

      int desc_idx = 0;
      for (auto *desc : chr->host_descriptors()) {
        // BlueZ owns the 0x2902 CCCD — never export it.
        if (desc->get_uuid() == esp32_ble::ESPBTUUID::from_uint16(0x2902))
          continue;
        auto ed = make_unique<ExportedDesc>();
        ed->desc = desc;
        ed->path = char_path + "/desc" + std::to_string(desc_idx);
        ed->char_path = char_path;
        ed->uuid_str = uuid_to_bluez_str(desc->get_uuid());
        this->descs_.push_back(std::move(ed));
        desc_idx++;
      }
      this->chars_.push_back(std::move(ec));
      chr_idx++;
    }
    this->services_.push_back(std::move(es));
    svc_idx++;
  }
}

bool BLEGattServer::register_application_() {
  sd_bus_message *msg = nullptr;
  std::string adapter = this->adapter_path_();
  int r = sd_bus_message_new_method_call(this->bus_, &msg, "org.bluez", adapter.c_str(), "org.bluez.GattManager1",
                                         "RegisterApplication");
  if (r < 0) {
    ESP_LOGE(TAG, "RegisterApplication new_method_call failed: %s", std::strerror(-r));
    return false;
  }
  sd_bus_message_append(msg, "o", APP_ROOT);
  sd_bus_message_open_container(msg, 'a', "{sv}");  // empty options
  sd_bus_message_close_container(msg);
  // Async: BlueZ calls our ObjectManager.GetManagedObjects back BEFORE replying,
  // and only the worker's event loop can dispatch that inbound call. A blocking
  // sd_bus_call here would deadlock (we'd be waiting for a reply that needs us to
  // first answer a call we can't service). The reply lands in on_register_app_reply_.
  r = sd_bus_call_async(this->bus_, nullptr, msg, &BLEGattServer::on_register_app_reply_, this, 0);
  sd_bus_message_unref(msg);
  if (r < 0) {
    ESP_LOGE(TAG, "RegisterApplication dispatch failed: %s", std::strerror(-r));
    return false;
  }
  return true;
}

int BLEGattServer::on_register_app_reply_(sd_bus_message *reply, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<BLEGattServer *>(userdata);
  const sd_bus_error *err = sd_bus_message_get_error(reply);
  if (err != nullptr && sd_bus_error_is_set(err)) {
    ESP_LOGE(TAG, "RegisterApplication failed: %s", err->message ? err->message : err->name);
    return 0;
  }
  ESP_LOGI(TAG, "GATT application registered (%u services, %u characteristics)",
           static_cast<unsigned>(self->services_.size()), static_cast<unsigned>(self->chars_.size()));
  self->enumerate_connected_devices_();
  return 0;
}

void BLEGattServer::enumerate_connected_devices_() {
  if (this->bus_ == nullptr)
    return;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  // Blocking is safe here: we're already on the worker thread and org.bluez's
  // ObjectManager.GetManagedObjects does not call back into our objects.
  int r = sd_bus_call_method(this->bus_, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                             "GetManagedObjects", &err, &reply, "");
  if (r < 0) {
    ESP_LOGW(TAG, "GetManagedObjects (connected-device enum) failed: %s", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return;
  }
  // a{oa{sa{sv}}}
  sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
    const char *obj_path = nullptr;
    sd_bus_message_read(reply, "o", &obj_path);
    bool is_device = false, connected = false;
    sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
      const char *iface = nullptr;
      sd_bus_message_read(reply, "s", &iface);
      bool this_is_device = iface != nullptr && std::strcmp(iface, "org.bluez.Device1") == 0;
      if (this_is_device)
        is_device = true;
      sd_bus_message_enter_container(reply, 'a', "{sv}");
      while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key = nullptr;
        sd_bus_message_read(reply, "s", &key);
        if (this_is_device && key != nullptr && std::strcmp(key, "Connected") == 0) {
          sd_bus_message_enter_container(reply, 'v', "b");
          int v = 0;
          sd_bus_message_read(reply, "b", &v);
          sd_bus_message_exit_container(reply);
          connected = v != 0;
        } else {
          sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);  // e sv
      }
      sd_bus_message_exit_container(reply);  // a {sv}
      sd_bus_message_exit_container(reply);  // e sa{sv}
    }
    sd_bus_message_exit_container(reply);  // a {sa{sv}}
    sd_bus_message_exit_container(reply);  // e oa{sa{sv}}
    if (is_device && connected && obj_path != nullptr) {
      ESP_LOGD(TAG, "central already connected at startup: %s", obj_path);
      uint16_t conn_id = this->intern_device_(obj_path);
      this->post_event_({ServerEvent::Kind::CONNECT, conn_id});
    }
  }
  sd_bus_message_exit_container(reply);  // a {oa{sa{sv}}}
  sd_bus_message_unref(reply);
  sd_bus_error_free(&err);
}

// ---------------------------------------------------------------------------
// notify
// ---------------------------------------------------------------------------

void BLEGattServer::notify_characteristic(BLECharacteristic *chr, const std::vector<uint8_t> &value) {
  if (chr->object_path_.empty())
    return;
  {
    std::lock_guard<std::mutex> g(this->cmd_mu_);
    this->commands_.push_back({ServerCommand::Kind::NOTIFY, chr->object_path_, value});
  }
  BleWorker::instance().wake();
}

void BLEGattServer::worker_process_commands() {
  bool start_requested;
  std::deque<ServerCommand> cmds;
  {
    std::lock_guard<std::mutex> g(this->cmd_mu_);
    start_requested = this->start_requested_;
    this->start_requested_ = false;
    cmds.swap(this->commands_);
  }
  // Bring the server up on the worker before draining any notify/advert work.
  if (start_requested)
    this->do_start_();
  for (auto &c : cmds) {
    if (c.kind == ServerCommand::Kind::NOTIFY)
      this->do_notify_(c.char_path, c.value);
  }
  // Apply a pending advertisement registration on the worker thread (advertising
  // changes are pushed from the main thread via on_advertising_changed).
  if (this->adv_pending_) {
    this->adv_pending_ = false;
    this->register_advertisement_(this->pending_adv_);
  }
}

void BLEGattServer::do_notify_(const std::string &char_path, const std::vector<uint8_t> &value) {
  auto *ec = this->find_char_(char_path.c_str());
  if (ec == nullptr)
    return;
  ec->cached_value = value;  // the Value getter serves this
  // Emit PropertiesChanged(Value) — BlueZ delivers to subscribed clients only.
  int r = sd_bus_emit_properties_changed(this->bus_, char_path.c_str(), "org.bluez.GattCharacteristic1", "Value",
                                         nullptr);
  if (r < 0)
    ESP_LOGW(TAG, "emit Value changed failed: %s", std::strerror(-r));
}

std::deque<ServerEvent> BLEGattServer::drain_events() {
  std::deque<ServerEvent> out;
  std::lock_guard<std::mutex> g(this->evt_mu_);
  out.swap(this->events_);
  return out;
}

void BLEGattServer::post_event_(ServerEvent ev) {
  std::lock_guard<std::mutex> g(this->evt_mu_);
  this->events_.push_back(ev);
}

uint16_t BLEGattServer::intern_device_(const char *device_path) {
  if (device_path == nullptr)
    return 0;
  auto it = this->device_conn_ids_.find(device_path);
  if (it != this->device_conn_ids_.end())
    return it->second;
  uint16_t id = this->next_conn_id_++;
  this->device_conn_ids_.emplace(device_path, id);
  return id;
}

// ---------------------------------------------------------------------------
// registry lookups
// ---------------------------------------------------------------------------

ExportedChar *BLEGattServer::find_char_(const char *path) {
  for (auto &c : this->chars_)
    if (c->path == path)
      return c.get();
  return nullptr;
}
ExportedDesc *BLEGattServer::find_desc_(const char *path) {
  for (auto &d : this->descs_)
    if (d->path == path)
      return d.get();
  return nullptr;
}
ExportedService *BLEGattServer::find_service_(const char *path) {
  for (auto &s : this->services_)
    if (s->path == path)
      return s.get();
  return nullptr;
}

// ---------------------------------------------------------------------------
// GattService1 property getters
// ---------------------------------------------------------------------------

int BLEGattServer::svc_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                 sd_bus_error *) {
  auto *es = static_cast<ExportedService *>(ud);
  return sd_bus_message_append(reply, "s", es->uuid_str.c_str());
}
int BLEGattServer::svc_get_primary_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                    sd_bus_error *) {
  auto *es = static_cast<ExportedService *>(ud);
  int v = es->primary ? 1 : 0;
  return sd_bus_message_append(reply, "b", v);
}

// ---------------------------------------------------------------------------
// GattCharacteristic1 property getters
// ---------------------------------------------------------------------------

int BLEGattServer::chr_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                 sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  return sd_bus_message_append(reply, "s", ec->uuid_str.c_str());
}
int BLEGattServer::chr_get_service_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                    sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  return sd_bus_message_append(reply, "o", ec->service_path.c_str());
}
int BLEGattServer::chr_get_value_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                  sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  return append_ay(reply, ec->cached_value);
}
int BLEGattServer::chr_get_flags_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                  sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  uint32_t p = ec->properties;
  sd_bus_message_open_container(reply, 'a', "s");
  if (p & BLECharacteristic::PROPERTY_BROADCAST)
    sd_bus_message_append(reply, "s", "broadcast");
  if (p & BLECharacteristic::PROPERTY_READ)
    sd_bus_message_append(reply, "s", "read");
  if (p & BLECharacteristic::PROPERTY_WRITE_NR)
    sd_bus_message_append(reply, "s", "write-without-response");
  if (p & BLECharacteristic::PROPERTY_WRITE)
    sd_bus_message_append(reply, "s", "write");
  if (p & BLECharacteristic::PROPERTY_NOTIFY)
    sd_bus_message_append(reply, "s", "notify");
  if (p & BLECharacteristic::PROPERTY_INDICATE)
    sd_bus_message_append(reply, "s", "indicate");
  return sd_bus_message_close_container(reply);
}
int BLEGattServer::chr_get_notifying_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                      void *ud, sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  int v = ec->notifying ? 1 : 0;
  return sd_bus_message_append(reply, "b", v);
}

// ---------------------------------------------------------------------------
// GattCharacteristic1 methods
// ---------------------------------------------------------------------------

// Parse the trailing a{sv} options dict, pulling out "offset" (q) and "device" (o).
static void parse_options_(sd_bus_message *m, uint16_t *offset, const char **device) {
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
    return;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char *key = nullptr;
    sd_bus_message_read(m, "s", &key);
    if (key != nullptr && std::strcmp(key, "offset") == 0) {
      sd_bus_message_enter_container(m, 'v', "q");
      if (offset != nullptr)
        sd_bus_message_read(m, "q", offset);
      sd_bus_message_exit_container(m);
    } else if (key != nullptr && std::strcmp(key, "device") == 0) {
      sd_bus_message_enter_container(m, 'v', "o");
      if (device != nullptr)
        sd_bus_message_read(m, "o", device);
      sd_bus_message_exit_container(m);
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);  // dict entry
  }
  sd_bus_message_exit_container(m);  // a{sv}
}

int BLEGattServer::chr_read_value_(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *ec = static_cast<ExportedChar *>(ud);
  uint16_t offset = 0;
  const char *device = nullptr;
  parse_options_(m, &offset, &device);
  // Resolve conn_id (we have no BLEGattServer* in vtable userdata; the global
  // server owns this characteristic, so route via the characteristic's server).
  BLEGattServer *srv = ec->chr->gatt_server_;
  uint16_t conn_id = srv != nullptr ? srv->intern_device_(device) : 0;

  // Fire on_read (side-effects value_) on the worker thread, then serve value_.
  const std::vector<uint8_t> &value = ec->chr->host_on_read(conn_id);
  ec->cached_value = value;
  ESP_LOGD(TAG, "ReadValue %s off=%u conn=%u -> %u bytes", ec->path.c_str(), offset, conn_id,
           static_cast<unsigned>(value.size()));

  std::vector<uint8_t> slice;
  if (offset < value.size())
    slice.assign(value.begin() + offset, value.end());
  else if (offset > value.size())
    return sd_bus_error_set(err, "org.bluez.Error.InvalidOffset", "offset past end");

  sd_bus_message *reply = nullptr;
  int r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0)
    return r;
  append_ay(reply, slice);
  r = sd_bus_send(nullptr, reply, nullptr);
  sd_bus_message_unref(reply);
  return r;
}

int BLEGattServer::chr_write_value_(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *ec = static_cast<ExportedChar *>(ud);
  const void *data = nullptr;
  size_t len = 0;
  int r = sd_bus_message_read_array(m, 'y', &data, &len);
  if (r < 0)
    return r;
  uint16_t offset = 0;
  const char *device = nullptr;
  parse_options_(m, &offset, &device);
  BLEGattServer *srv = ec->chr->gatt_server_;
  uint16_t conn_id = srv != nullptr ? srv->intern_device_(device) : 0;

  std::span<const uint8_t> bytes(static_cast<const uint8_t *>(data), len);
  ESP_LOGD(TAG, "WriteValue %s conn=%u <- %u bytes (first=0x%02X)", ec->path.c_str(), conn_id,
           static_cast<unsigned>(len), len > 0 ? static_cast<const uint8_t *>(data)[0] : 0);
  ec->chr->host_on_write(bytes, conn_id);
  ec->cached_value.assign(bytes.begin(), bytes.end());
  (void) err;
  return sd_bus_reply_method_return(m, "");
}

int BLEGattServer::chr_start_notify_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  ec->notifying = true;
  BLEGattServer *srv = ec->chr->gatt_server_;
  if (srv != nullptr)
    sd_bus_emit_properties_changed(srv->bus_, ec->path.c_str(), "org.bluez.GattCharacteristic1", "Notifying", nullptr);
  ESP_LOGD(TAG, "StartNotify %s", ec->path.c_str());
  return sd_bus_reply_method_return(m, "");
}

int BLEGattServer::chr_stop_notify_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *ec = static_cast<ExportedChar *>(ud);
  ec->notifying = false;
  BLEGattServer *srv = ec->chr->gatt_server_;
  if (srv != nullptr)
    sd_bus_emit_properties_changed(srv->bus_, ec->path.c_str(), "org.bluez.GattCharacteristic1", "Notifying", nullptr);
  ESP_LOGD(TAG, "StopNotify %s", ec->path.c_str());
  return sd_bus_reply_method_return(m, "");
}

// ---------------------------------------------------------------------------
// GattDescriptor1
// ---------------------------------------------------------------------------

int BLEGattServer::desc_get_uuid_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                  sd_bus_error *) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  return sd_bus_message_append(reply, "s", ed->uuid_str.c_str());
}
int BLEGattServer::desc_get_char_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                  sd_bus_error *) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  return sd_bus_message_append(reply, "o", ed->char_path.c_str());
}
int BLEGattServer::desc_get_flags_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                   sd_bus_error *) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  sd_bus_message_open_container(reply, 'a', "s");
  if (ed->desc->host_readable())
    sd_bus_message_append(reply, "s", "read");
  if (ed->desc->host_writable())
    sd_bus_message_append(reply, "s", "write");
  return sd_bus_message_close_container(reply);
}
int BLEGattServer::desc_get_value_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *ud,
                                   sd_bus_error *) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  return append_ay(reply, ed->desc->host_value());
}
int BLEGattServer::desc_read_value_(sd_bus_message *m, void *ud, sd_bus_error *err) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  uint16_t offset = 0;
  parse_options_(m, &offset, nullptr);
  const std::vector<uint8_t> &value = ed->desc->host_value();
  std::vector<uint8_t> slice;
  if (offset < value.size())
    slice.assign(value.begin() + offset, value.end());
  else if (offset > value.size())
    return sd_bus_error_set(err, "org.bluez.Error.InvalidOffset", "offset past end");
  sd_bus_message *reply = nullptr;
  int r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0)
    return r;
  append_ay(reply, slice);
  r = sd_bus_send(nullptr, reply, nullptr);
  sd_bus_message_unref(reply);
  return r;
}
int BLEGattServer::desc_write_value_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *ed = static_cast<ExportedDesc *>(ud);
  const void *data = nullptr;
  size_t len = 0;
  int r = sd_bus_message_read_array(m, 'y', &data, &len);
  if (r < 0)
    return r;
  uint16_t offset = 0;
  const char *device = nullptr;
  parse_options_(m, &offset, &device);
  BLEGattServer *srv = ed->desc->characteristic_ != nullptr ? ed->desc->characteristic_->gatt_server_ : nullptr;
  uint16_t conn_id = srv != nullptr ? srv->intern_device_(device) : 0;
  std::span<const uint8_t> bytes(static_cast<const uint8_t *>(data), len);
  ed->desc->host_on_write(bytes, conn_id);
  return sd_bus_reply_method_return(m, "");
}

// ---------------------------------------------------------------------------
// LEAdvertisement1
// ---------------------------------------------------------------------------

void BLEGattServer::on_advertising_changed(const esp32_ble::HostAdvertisement &adv) {
  if (!adv.active)
    return;
  this->pending_adv_ = adv;
  this->adv_pending_ = true;
  BleWorker::instance().wake();
}

void BLEGattServer::register_advertisement_(const esp32_ble::HostAdvertisement &adv) {
  if (this->bus_ == nullptr)
    return;
  if (this->adv_registered_) {
    // Re-registering requires unregister first; BlueZ rejects a duplicate path.
    this->unregister_advertisement_();
  }
  if (this->adv_slot_ == nullptr) {
    int r = sd_bus_add_object_vtable(this->bus_, &this->adv_slot_, ADV_PATH, "org.bluez.LEAdvertisement1", ADV_VTABLE,
                                     this);
    if (r < 0) {
      ESP_LOGW(TAG, "export LEAdvertisement1 failed: %s", std::strerror(-r));
      return;
    }
  }
  sd_bus_message *msg = nullptr;
  std::string adapter = this->adapter_path_();
  int r = sd_bus_message_new_method_call(this->bus_, &msg, "org.bluez", adapter.c_str(),
                                         "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement");
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement new_method_call failed: %s", std::strerror(-r));
    return;
  }
  sd_bus_message_append(msg, "o", ADV_PATH);
  sd_bus_message_open_container(msg, 'a', "{sv}");
  sd_bus_message_close_container(msg);
  // Async: like RegisterApplication, BlueZ reads our LEAdvertisement1 properties
  // back before replying. Blocking here would stall the worker loop and starve
  // the inbound property reads (and the GATT app's GetManagedObjects).
  r = sd_bus_call_async(this->bus_, nullptr, msg, &BLEGattServer::on_register_adv_reply_, this, 0);
  sd_bus_message_unref(msg);
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement dispatch failed: %s", std::strerror(-r));
    return;
  }
  this->adv_registered_ = true;  // optimistic; cleared on error in the reply cb
}

int BLEGattServer::on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<BLEGattServer *>(userdata);
  const sd_bus_error *err = sd_bus_message_get_error(reply);
  if (err != nullptr && sd_bus_error_is_set(err)) {
    ESP_LOGW(TAG, "RegisterAdvertisement failed: %s", err->message ? err->message : err->name);
    self->adv_registered_ = false;
    // bluez#644: re-register can fail with "Invalid Parameters"/"Failed" until the
    // adapter is bounced. Power-cycle once, then retry; don't loop forever.
    if (!self->readv_power_cycled_) {
      self->readv_power_cycled_ = true;
      self->power_cycle_adapter_();
    }
    return 0;
  }
  ESP_LOGD(TAG, "LE advertisement registered");
  self->readv_power_cycled_ = false;  // success — allow the fallback again next time
  return 0;
}

void BLEGattServer::unregister_advertisement_() {
  if (!this->adv_registered_ || this->bus_ == nullptr)
    return;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  std::string adapter = this->adapter_path_();
  sd_bus_call_method(this->bus_, "org.bluez", adapter.c_str(), "org.bluez.LEAdvertisingManager1",
                     "UnregisterAdvertisement", &err, nullptr, "o", ADV_PATH);
  sd_bus_error_free(&err);
  this->adv_registered_ = false;
}

// Arm (or re-arm) a one-shot timer ~300 ms out on the shared worker's event loop.
// The debounce lets the kernel finish tearing the link down before we re-add the
// advertising set — re-registering too early races the kernel's own (failing)
// hci_enable_advertising and can hit "Busy"/"Invalid Parameters".
void BLEGattServer::schedule_readvertise_() {
  sd_event *ev = BleWorker::instance().event();
  if (ev == nullptr)
    return;
  uint64_t now = 0;
  sd_event_now(ev, CLOCK_MONOTONIC, &now);
  const uint64_t when = now + 300000;  // +300 ms
  if (this->readv_timer_ != nullptr) {
    // Already armed — push it out and re-enable (coalesces a burst of disconnects).
    sd_event_source_set_time(this->readv_timer_, when);
    sd_event_source_set_enabled(this->readv_timer_, SD_EVENT_ONESHOT);
    return;
  }
  int r = sd_event_add_time(ev, &this->readv_timer_, CLOCK_MONOTONIC, when, 0 /*accuracy*/,
                            &BLEGattServer::on_readvertise_timer_, this);
  if (r < 0)
    ESP_LOGW(TAG, "schedule re-advertise timer failed: %s", std::strerror(-r));
}

int BLEGattServer::on_readvertise_timer_(sd_event_source * /*s*/, uint64_t /*usec*/, void *userdata) {
  auto *self = static_cast<BLEGattServer *>(userdata);
  ESP_LOGD(TAG, "central disconnected — re-asserting advertising");
  self->register_advertisement_(self->pending_adv_);  // unregisters first, then re-registers
  return 0;
}

// Last-resort recovery for the bluez#644 failure mode where Unregister+Register
// returns "Invalid Parameters (0x0d)" and only an adapter power-cycle restores
// advertising. Bounce Adapter1.Powered, then re-register once.
void BLEGattServer::power_cycle_adapter_() {
  if (this->bus_ == nullptr)
    return;
  std::string adapter = this->adapter_path_();
  ESP_LOGW(TAG, "advertising stuck (bluez#644) — power-cycling %s", adapter.c_str());
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int off = 0, on = 1;
  sd_bus_set_property(this->bus_, "org.bluez", adapter.c_str(), "org.bluez.Adapter1", "Powered", &err, "b", off);
  sd_bus_error_free(&err);
  err = SD_BUS_ERROR_NULL;
  sd_bus_set_property(this->bus_, "org.bluez", adapter.c_str(), "org.bluez.Adapter1", "Powered", &err, "b", on);
  sd_bus_error_free(&err);
  // The GATT application stays registered across a power-cycle; only advertising
  // and the adv slot were lost. Drop our slot so register_advertisement_ re-exports.
  if (this->adv_slot_ != nullptr) {
    sd_bus_slot_unref(this->adv_slot_);
    this->adv_slot_ = nullptr;
  }
  this->adv_registered_ = false;
  this->register_advertisement_(this->pending_adv_);
}

int BLEGattServer::adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *,
                                 sd_bus_error *) {
  return sd_bus_message_append(reply, "s", "peripheral");
}
int BLEGattServer::adv_get_local_name_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                       void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  return sd_bus_message_append(reply, "s", self->pending_adv_.local_name.c_str());
}
int BLEGattServer::adv_get_service_uuids_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                          void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  sd_bus_message_open_container(reply, 'a', "s");
  for (auto &u : self->pending_adv_.service_uuids) {
    std::string s = uuid_to_bluez_str(u);
    sd_bus_message_append(reply, "s", s.c_str());
  }
  return sd_bus_message_close_container(reply);
}
int BLEGattServer::adv_get_manufacturer_data_(sd_bus *, const char *, const char *, const char *,
                                              sd_bus_message *reply, void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  sd_bus_message_open_container(reply, 'a', "{qv}");
  if (self->pending_adv_.manufacturer_company != 0 && !self->pending_adv_.manufacturer_data.empty()) {
    sd_bus_message_open_container(reply, 'e', "qv");
    sd_bus_message_append(reply, "q", self->pending_adv_.manufacturer_company);
    sd_bus_message_open_container(reply, 'v', "ay");
    append_ay(reply, self->pending_adv_.manufacturer_data);
    sd_bus_message_close_container(reply);  // v
    sd_bus_message_close_container(reply);  // e
  }
  return sd_bus_message_close_container(reply);  // a
}
int BLEGattServer::adv_get_service_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                         void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  sd_bus_message_open_container(reply, 'a', "{sv}");
  // Service data keyed by the first advertised service UUID, if any.
  if (!self->pending_adv_.service_data.empty() && !self->pending_adv_.service_uuids.empty()) {
    std::string key = uuid_to_bluez_str(self->pending_adv_.service_uuids.front());
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", key.c_str());
    sd_bus_message_open_container(reply, 'v', "ay");
    append_ay(reply, self->pending_adv_.service_data);
    sd_bus_message_close_container(reply);  // v
    sd_bus_message_close_container(reply);  // e
  }
  return sd_bus_message_close_container(reply);  // a
}
int BLEGattServer::adv_get_appearance_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                       void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  return sd_bus_message_append(reply, "q", self->pending_adv_.appearance);
}
int BLEGattServer::adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                     void *ud, sd_bus_error *) {
  // Includes is ONLY for fields BlueZ should auto-fill from adapter defaults
  // (e.g. "tx-power"). It is mutually exclusive with the explicit Appearance /
  // LocalName properties: listing "appearance"/"local-name" here while ALSO
  // exposing those properties makes bluetoothd reject the advertisement with
  // "Failed to parse advertisement". We always expose Appearance/LocalName
  // explicitly when set, so Includes stays empty.
  (void) ud;
  sd_bus_message_open_container(reply, 'a', "s");
  return sd_bus_message_close_container(reply);
}
int BLEGattServer::adv_release_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  self->adv_registered_ = false;
  ESP_LOGD(TAG, "LE advertisement released by BlueZ");
  return sd_bus_reply_method_return(m, "");
}

// ---------------------------------------------------------------------------
// connect/disconnect watch
// ---------------------------------------------------------------------------

int BLEGattServer::on_device_props_changed_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *self = static_cast<BLEGattServer *>(ud);
  const char *path = sd_bus_message_get_path(m);
  const char *iface = nullptr;
  if (sd_bus_message_read(m, "s", &iface) < 0)
    return 0;
  if (iface == nullptr || std::strcmp(iface, "org.bluez.Device1") != 0)
    return 0;

  bool have_connected = false, connected = false;
  if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
    return 0;
  while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
    const char *key = nullptr;
    sd_bus_message_read(m, "s", &key);
    if (key != nullptr && std::strcmp(key, "Connected") == 0) {
      sd_bus_message_enter_container(m, 'v', "b");
      int v = 0;
      sd_bus_message_read(m, "b", &v);
      sd_bus_message_exit_container(m);
      have_connected = true;
      connected = v != 0;
    } else {
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);
  }
  sd_bus_message_exit_container(m);

  if (!have_connected || path == nullptr)
    return 0;
  uint16_t conn_id = self->intern_device_(path);
  self->post_event_({connected ? ServerEvent::Kind::CONNECT : ServerEvent::Kind::DISCONNECT, conn_id});
  // A central just dropped — the controller stopped our connectable advertising and
  // the kernel's ext-adv auto-resume is unreliable on this radio (bluez#644). Force
  // a fresh advertising set so the central (or another) can reconnect.
  if (!connected)
    self->schedule_readvertise_();
  return 0;
}

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
