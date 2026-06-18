#ifdef USE_HOST

#include "bthome_advertiser.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace bthome_advertiser {

static const char *const TAG = "bthome_advertiser";

// All three host advertisers register against the default adapter.
static constexpr const char *ADAPTER_PATH = "/org/bluez/hci0";

static const sd_bus_vtable ADV_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type", "s", BTHomeAdvertiser::adv_get_type_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", BTHomeAdvertiser::adv_get_service_uuids_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceData", "a{sv}", BTHomeAdvertiser::adv_get_service_data_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "as", BTHomeAdvertiser::adv_get_includes_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", BTHomeAdvertiser::adv_release_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

BTHomeAdvertiser::~BTHomeAdvertiser() {
  if (this->attached_) {
    esp32_ble::BleWorker::instance().detach_worker_client(this);
    this->attached_ = false;
  }
  // Best-effort cleanup (mirrors the GATT server) so we don't leak an advertising
  // instance into BlueZ across process restarts (RUN-LOG #644/stale-instance).
  this->unregister_advertisement_();
  if (this->adv_slot_ != nullptr)
    sd_bus_slot_unref(this->adv_slot_);
  if (this->bus_ != nullptr)
    sd_bus_unref(this->bus_);
}

float BTHomeAdvertiser::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

void BTHomeAdvertiser::setup() {
  this->payload_ = build_bthome_payload(this->measurements_, this->encrypted_);
  esp32_ble::BleWorker::instance().attach_worker_client(this);
  this->attached_ = true;
  {
    std::lock_guard<std::mutex> g(this->mu_);
    this->start_requested_ = true;
  }
  esp32_ble::BleWorker::instance().wake();
}

void BTHomeAdvertiser::worker_process_commands() {
  bool start_requested;
  {
    std::lock_guard<std::mutex> g(this->mu_);
    start_requested = this->start_requested_;
    this->start_requested_ = false;
  }
  if (start_requested)
    this->do_start_();
}

bool BTHomeAdvertiser::open_bus_() {
  if (this->bus_open_)
    return true;
  int r = sd_bus_open_system(&this->bus_);
  if (r < 0) {
    ESP_LOGW(TAG, "open system bus failed: %s", std::strerror(-r));
    return false;
  }
  sd_event *ev = esp32_ble::BleWorker::instance().event();
  if (ev != nullptr) {
    sd_bus_attach_event(this->bus_, ev, 0);
  } else {
    ESP_LOGE(TAG, "open_bus_: worker event loop is NULL — bus NOT attached!");
  }
  this->bus_open_ = true;
  return true;
}

void BTHomeAdvertiser::do_start_() {
  if (this->started_)
    return;
  this->started_ = true;
  if (!this->open_bus_()) {
    ESP_LOGE(TAG, "bthome: no system bus; not advertising");
    return;
  }
  this->register_advertisement_();
}

void BTHomeAdvertiser::register_advertisement_() {
  if (this->bus_ == nullptr)
    return;
  if (this->adv_slot_ == nullptr) {
    int r = sd_bus_add_object_vtable(this->bus_, &this->adv_slot_, BTHOME_ADV_PATH, "org.bluez.LEAdvertisement1",
                                     ADV_VTABLE, this);
    if (r < 0) {
      ESP_LOGW(TAG, "export LEAdvertisement1 failed: %s", std::strerror(-r));
      return;
    }
  }
  sd_bus_message *msg = nullptr;
  int r = sd_bus_message_new_method_call(this->bus_, &msg, "org.bluez", ADAPTER_PATH,
                                         "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement");
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement new_method_call failed: %s", std::strerror(-r));
    return;
  }
  sd_bus_message_append(msg, "o", BTHOME_ADV_PATH);
  sd_bus_message_open_container(msg, 'a', "{sv}");
  sd_bus_message_close_container(msg);
  // Async: BlueZ reads our LEAdvertisement1 properties back before replying;
  // blocking here would stall the worker loop and starve those inbound reads.
  r = sd_bus_call_async(this->bus_, nullptr, msg, &BTHomeAdvertiser::on_register_adv_reply_, this, 0);
  sd_bus_message_unref(msg);
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement dispatch failed: %s", std::strerror(-r));
    return;
  }
  this->adv_registered_ = true;  // optimistic; cleared on error in the reply cb
}

void BTHomeAdvertiser::unregister_advertisement_() {
  if (!this->adv_registered_ || this->bus_ == nullptr)
    return;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_call_method(this->bus_, "org.bluez", ADAPTER_PATH, "org.bluez.LEAdvertisingManager1",
                     "UnregisterAdvertisement", &err, nullptr, "o", BTHOME_ADV_PATH);
  sd_bus_error_free(&err);
  this->adv_registered_ = false;
}

int BTHomeAdvertiser::on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<BTHomeAdvertiser *>(userdata);
  const sd_bus_error *err = sd_bus_message_get_error(reply);
  if (err != nullptr && sd_bus_error_is_set(err)) {
    ESP_LOGW(TAG, "RegisterAdvertisement failed: %s", err->message ? err->message : err->name);
    self->adv_registered_ = false;
    return 0;
  }
  ESP_LOGI(TAG, "BTHome advertisement registered (%zu-byte payload)", self->payload_.size());
  return 0;
}

// --- LEAdvertisement1 property getters ---

int BTHomeAdvertiser::adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *,
                                    sd_bus_error *) {
  // Non-connectable broadcast (BTHome adverts are broadcast-only).
  return sd_bus_message_append(reply, "s", "broadcast");
}

int BTHomeAdvertiser::adv_get_service_uuids_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                             void *, sd_bus_error *) {
  sd_bus_message_open_container(reply, 'a', "s");
  sd_bus_message_append(reply, "s", BTHOME_SERVICE_UUID);
  return sd_bus_message_close_container(reply);
}

int BTHomeAdvertiser::adv_get_service_data_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                            void *ud, sd_bus_error *) {
  auto *self = static_cast<BTHomeAdvertiser *>(ud);
  sd_bus_message_open_container(reply, 'a', "{sv}");
  sd_bus_message_open_container(reply, 'e', "sv");
  sd_bus_message_append(reply, "s", BTHOME_SERVICE_UUID);
  sd_bus_message_open_container(reply, 'v', "ay");
  sd_bus_message_append_array(reply, 'y', self->payload_.data(), self->payload_.size());
  sd_bus_message_close_container(reply);  // v
  sd_bus_message_close_container(reply);  // e
  return sd_bus_message_close_container(reply);  // a
}

int BTHomeAdvertiser::adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply,
                                        void *, sd_bus_error *) {
  // No adapter-default fields auto-included (keeps the AD minimal/legacy).
  sd_bus_message_open_container(reply, 'a', "s");
  return sd_bus_message_close_container(reply);
}

int BTHomeAdvertiser::adv_release_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *self = static_cast<BTHomeAdvertiser *>(ud);
  self->adv_registered_ = false;
  ESP_LOGD(TAG, "BTHome advertisement released by BlueZ");
  return sd_bus_reply_method_return(m, "");
}

void BTHomeAdvertiser::dump_config() {
  ESP_LOGCONFIG(TAG, "BTHome Advertiser (host/BlueZ):");
  ESP_LOGCONFIG(TAG, "  Service Data UUID: 0xFCD2 (BTHome v2%s)", this->encrypted_ ? ", encrypted" : "");
  ESP_LOGCONFIG(TAG, "  Measurements: %zu, payload: %zu bytes", this->measurements_.size(), this->payload_.size());
  // Service Data AD (~payload+4) + Flags (3) + 16-bit UUIDs AD (4). >31 total ->
  // BlueZ promotes to an extended-PDU advert; warn so the user can trim objects.
  size_t ad_estimate = this->payload_.size() + 4 + 3 + 4;
  if (ad_estimate > 31)
    ESP_LOGW(TAG, "  AD ~%zu bytes > 31 — BlueZ will use an extended-PDU advert (not legacy-visible)", ad_estimate);
}

}  // namespace bthome_advertiser
}  // namespace esphome

#endif  // USE_HOST
