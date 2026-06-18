#ifdef USE_HOST

#include "esp32_ble_beacon.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace esp32_ble_beacon {

static const char *const TAG = "esp32_ble_beacon";

// Apple's Bluetooth SIG company identifier — the ManufacturerData dict key for
// iBeacon (and the 0x004C every iBeacon scanner, incl. esp32_ble_tracker, keys on).
static constexpr uint16_t APPLE_COMPANY_ID = 0x004C;

static const sd_bus_vtable ADV_VTABLE[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type", "s", ESP32BLEBeacon::adv_get_type_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ManufacturerData", "a{qv}", ESP32BLEBeacon::adv_get_manufacturer_data_, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "as", ESP32BLEBeacon::adv_get_includes_, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", ESP32BLEBeacon::adv_release_, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// iBeacon ManufacturerData value (after the 0x004C company id): 0x02 0x15
// (iBeacon subtype + 21-byte length) + 16-byte proximity UUID + major (BE) +
// minor (BE) + 1-byte measured power = 23 bytes.
std::vector<uint8_t> ESP32BLEBeacon::build_ibeacon_payload_() const {
  std::vector<uint8_t> v;
  v.reserve(23);
  v.push_back(0x02);
  v.push_back(0x15);
  for (uint8_t b : this->uuid_)
    v.push_back(b);
  v.push_back(static_cast<uint8_t>(this->major_ >> 8));
  v.push_back(static_cast<uint8_t>(this->major_ & 0xFF));
  v.push_back(static_cast<uint8_t>(this->minor_ >> 8));
  v.push_back(static_cast<uint8_t>(this->minor_ & 0xFF));
  v.push_back(static_cast<uint8_t>(this->measured_power_));
  return v;
}

float ESP32BLEBeacon::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

void ESP32BLEBeacon::setup() {
  this->mfg_data_ = this->build_ibeacon_payload_();
  // Attach to the shared worker, then post a START so do_start_() (bus open +
  // RegisterAdvertisement) runs on the worker thread that owns the sd_event loop.
  esp32_ble::BleWorker::instance().attach_worker_client(this);
  {
    std::lock_guard<std::mutex> g(this->mu_);
    this->start_requested_ = true;
  }
  esp32_ble::BleWorker::instance().wake();
}

void ESP32BLEBeacon::worker_process_commands() {
  bool start_requested;
  {
    std::lock_guard<std::mutex> g(this->mu_);
    start_requested = this->start_requested_;
    this->start_requested_ = false;
  }
  if (start_requested)
    this->do_start_();
}

bool ESP32BLEBeacon::open_bus_() {
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

void ESP32BLEBeacon::do_start_() {
  if (this->started_)
    return;
  this->started_ = true;
  if (!this->open_bus_()) {
    ESP_LOGE(TAG, "beacon: no system bus; not advertising");
    return;
  }
  this->register_advertisement_();
}

void ESP32BLEBeacon::register_advertisement_() {
  if (this->bus_ == nullptr)
    return;
  if (this->adv_slot_ == nullptr) {
    int r = sd_bus_add_object_vtable(this->bus_, &this->adv_slot_, BEACON_ADV_PATH, "org.bluez.LEAdvertisement1",
                                     ADV_VTABLE, this);
    if (r < 0) {
      ESP_LOGW(TAG, "export LEAdvertisement1 failed: %s", std::strerror(-r));
      return;
    }
  }
  sd_bus_message *msg = nullptr;
  int r = sd_bus_message_new_method_call(this->bus_, &msg, "org.bluez", "/org/bluez/hci0",
                                         "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement");
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement new_method_call failed: %s", std::strerror(-r));
    return;
  }
  sd_bus_message_append(msg, "o", BEACON_ADV_PATH);
  sd_bus_message_open_container(msg, 'a', "{sv}");
  sd_bus_message_close_container(msg);
  // Async: BlueZ reads our LEAdvertisement1 properties back before replying;
  // blocking here would stall the worker loop and starve those inbound reads.
  r = sd_bus_call_async(this->bus_, nullptr, msg, &ESP32BLEBeacon::on_register_adv_reply_, this, 0);
  sd_bus_message_unref(msg);
  if (r < 0) {
    ESP_LOGW(TAG, "RegisterAdvertisement dispatch failed: %s", std::strerror(-r));
    return;
  }
  this->adv_registered_ = true;  // optimistic; cleared on error in the reply cb
}

int ESP32BLEBeacon::on_register_adv_reply_(sd_bus_message *reply, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<ESP32BLEBeacon *>(userdata);
  const sd_bus_error *err = sd_bus_message_get_error(reply);
  if (err != nullptr && sd_bus_error_is_set(err)) {
    ESP_LOGW(TAG, "RegisterAdvertisement failed: %s", err->message ? err->message : err->name);
    self->adv_registered_ = false;
    return 0;
  }
  ESP_LOGI(TAG, "iBeacon advertisement registered");
  return 0;
}

// --- LEAdvertisement1 property getters ---

int ESP32BLEBeacon::adv_get_type_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *,
                                  sd_bus_error *) {
  // Non-connectable, non-scannable broadcast — the BlueZ analogue of the IDF
  // ADV_TYPE_NONCONN_IND that upstream esp32_ble_beacon uses.
  return sd_bus_message_append(reply, "s", "broadcast");
}

int ESP32BLEBeacon::adv_get_manufacturer_data_(sd_bus *, const char *, const char *, const char *,
                                               sd_bus_message *reply, void *ud, sd_bus_error *) {
  auto *self = static_cast<ESP32BLEBeacon *>(ud);
  sd_bus_message_open_container(reply, 'a', "{qv}");
  sd_bus_message_open_container(reply, 'e', "qv");
  sd_bus_message_append(reply, "q", APPLE_COMPANY_ID);
  sd_bus_message_open_container(reply, 'v', "ay");
  sd_bus_message_append_array(reply, 'y', self->mfg_data_.data(), self->mfg_data_.size());
  sd_bus_message_close_container(reply);  // v
  sd_bus_message_close_container(reply);  // e
  return sd_bus_message_close_container(reply);  // a
}

int ESP32BLEBeacon::adv_get_includes_(sd_bus *, const char *, const char *, const char *, sd_bus_message *reply, void *,
                                      sd_bus_error *) {
  // No adapter-default fields auto-included (a pure iBeacon is flags + mfg data).
  sd_bus_message_open_container(reply, 'a', "s");
  return sd_bus_message_close_container(reply);
}

int ESP32BLEBeacon::adv_release_(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *self = static_cast<ESP32BLEBeacon *>(ud);
  self->adv_registered_ = false;
  ESP_LOGD(TAG, "iBeacon advertisement released by BlueZ");
  return sd_bus_reply_method_return(m, "");
}

void ESP32BLEBeacon::dump_config() {
  char uuid[37];
  char *bpos = uuid;
  for (int8_t ii = 0; ii < 16; ++ii) {
    static const char *const HEX = "0123456789abcdef";
    *bpos++ = HEX[this->uuid_[ii] >> 4];
    *bpos++ = HEX[this->uuid_[ii] & 0x0F];
    if (ii == 3 || ii == 5 || ii == 7 || ii == 9)
      *bpos++ = '-';
  }
  *bpos = '\0';
  ESP_LOGCONFIG(TAG, "ESP32 BLE Beacon (host/BlueZ):");
  ESP_LOGCONFIG(TAG, "  UUID: %s, Major: %u, Minor: %u, Measured Power: %d", uuid, this->major_, this->minor_,
                this->measured_power_);
  // Advertising interval and TX power are adapter-global on Linux (the kernel /
  // BlueZ own the radio); these are accepted for config parity but informational.
  ESP_LOGCONFIG(TAG, "  Min Interval: %ums, Max Interval: %ums, TX Power: %ddBm (BlueZ-controlled)",
                this->min_interval_, this->max_interval_, this->tx_power_);
}

}  // namespace esp32_ble_beacon
}  // namespace esphome

#endif  // USE_HOST
