#if defined(USE_ESP32) || defined(USE_HOST)

#include "am43_sensor.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace am43 {

static const char *const TAG = "am43";

void Am43::dump_config() {
  ESP_LOGCONFIG(TAG, "AM43");
  LOG_SENSOR(" ", "Battery", this->battery_);
  LOG_SENSOR(" ", "Illuminance", this->illuminance_);
}

void Am43::setup() {
  this->encoder_ = make_unique<Am43Encoder>();
  this->decoder_ = make_unique<Am43Decoder>();
  this->last_battery_update_ = 0;
  this->current_sensor_ = 0;
}

void Am43::write_packet_(Am43Packet *packet) {
  this->parent()->write_characteristic(this->char_handle_, packet->data, packet->length, /*response=*/false);
}

void Am43::on_disconnected(int /*reason*/) {
  this->node_state = espbt::ClientState::IDLE;
  this->char_handle_ = 0;
  if (this->battery_ != nullptr)
    this->battery_->publish_state(NAN);
  if (this->illuminance_ != nullptr)
    this->illuminance_->publish_state(NAN);
}

void Am43::on_services_discovered() {
  auto *chr = this->parent()->get_characteristic(AM43_SERVICE_UUID, AM43_CHARACTERISTIC_UUID);
  if (chr == nullptr) {
    if (this->parent()->get_characteristic(AM43_TUYA_SERVICE_UUID, AM43_TUYA_CHARACTERISTIC_UUID) != nullptr) {
      ESP_LOGE(TAG, "[%s] Detected a Tuya AM43 which is not supported, sorry.", this->parent()->address_str());
    } else {
      ESP_LOGE(TAG, "[%s] No control service found, not an AM43..?", this->parent()->address_str());
    }
    return;
  }
  this->char_handle_ = chr->handle;
  chr->start_notify(nullptr);
  this->node_state = espbt::ClientState::ESTABLISHED;
  this->update();
}

void Am43::update() {
  if (this->node_state != espbt::ClientState::ESTABLISHED || this->char_handle_ == 0)
    return;
  if (this->current_sensor_ == 0) {
    if (this->battery_ != nullptr)
      this->write_packet_(this->encoder_->get_battery_level_request());
    this->current_sensor_++;
  }
}

void Am43::on_notify(const BLENotifyEvent &e) {
  if (e.handle != this->char_handle_)
    return;
  this->decoder_->decode(e.data, e.len);

  if (this->battery_ != nullptr && this->decoder_->has_battery_level() &&
      millis() - this->last_battery_update_ > 10000) {
    this->battery_->publish_state(this->decoder_->battery_level_);
    this->last_battery_update_ = millis();
  }
  if (this->illuminance_ != nullptr && this->decoder_->has_light_level()) {
    this->illuminance_->publish_state(this->decoder_->light_level_);
  }
  if (this->current_sensor_ > 0) {
    if (this->illuminance_ != nullptr)
      this->write_packet_(this->encoder_->get_light_level_request());
    this->current_sensor_ = 0;
  }
}

}  // namespace am43
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
