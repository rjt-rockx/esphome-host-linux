#if defined(USE_ESP32) || defined(USE_HOST)

#include "am43_cover.h"

#include "esphome/core/log.h"

namespace esphome {
namespace am43 {

static const char *const TAG = "am43_cover";

using namespace esphome::cover;

void Am43Component::dump_config() {
  LOG_COVER("", "AM43 Cover", this);
  ESP_LOGCONFIG(TAG, "  Device Pin: %d, Invert Position: %d", this->pin_, (int) this->invert_position_);
}

void Am43Component::setup() {
  this->position = COVER_OPEN;
  this->encoder_ = make_unique<Am43Encoder>();
  this->decoder_ = make_unique<Am43Decoder>();
  this->logged_in_ = false;
}

void Am43Component::write_packet_(Am43Packet *packet) {
  // Native host write: no-response write to the control characteristic. The
  // ordering invariant (first write only after ESTABLISHED) is guaranteed by
  // the dispatch order — set_state/node_state is fanned before loop()/hooks.
  this->parent()->write_characteristic(this->char_handle_, packet->data, packet->length, /*response=*/false);
}

void Am43Component::loop() {
  if (this->node_state == espbt::ClientState::ESTABLISHED && !this->logged_in_ && this->char_handle_ != 0) {
    ESP_LOGI(TAG, "[%s] Logging into AM43", this->get_name().c_str());
    this->write_packet_(this->encoder_->get_send_pin_request(this->pin_));
    this->logged_in_ = true;
  }
}

CoverTraits Am43Component::get_traits() {
  auto traits = CoverTraits();
  traits.set_supports_stop(true);
  traits.set_supports_position(true);
  traits.set_supports_tilt(false);
  traits.set_is_assumed_state(false);
  return traits;
}

void Am43Component::control(const CoverCall &call) {
  if (this->node_state != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(TAG, "[%s] Cannot send cover control, not connected", this->get_name().c_str());
    return;
  }
  if (call.get_stop()) {
    this->write_packet_(this->encoder_->get_stop_request());
  }
  auto opt_pos = call.get_position();
  if (opt_pos.has_value()) {
    auto pos = *opt_pos;
    if (this->invert_position_)
      pos = 1 - pos;
    this->write_packet_(this->encoder_->get_set_position_request(100 - (uint8_t) (pos * 100)));
  }
}

void Am43Component::on_disconnected(int /*reason*/) {
  this->logged_in_ = false;
  this->char_handle_ = 0;
}

void Am43Component::on_services_discovered() {
  auto *chr = this->parent()->get_characteristic(AM43_SERVICE_UUID, AM43_CHARACTERISTIC_UUID);
  if (chr == nullptr) {
    if (this->parent()->get_characteristic(AM43_TUYA_SERVICE_UUID, AM43_TUYA_CHARACTERISTIC_UUID) != nullptr) {
      ESP_LOGE(TAG, "[%s] Detected a Tuya AM43 which is not supported, sorry.", this->get_name().c_str());
    } else {
      ESP_LOGE(TAG, "[%s] No control service found at device, not an AM43..?", this->get_name().c_str());
    }
    return;
  }
  this->char_handle_ = chr->handle;
  // Subscribe; ESTABLISHED is reached on notify-registration. The loop() login
  // gate fires once node_state becomes ESTABLISHED.
  chr->start_notify(nullptr);
  this->node_state = espbt::ClientState::ESTABLISHED;
}

void Am43Component::on_notify(const BLENotifyEvent &e) {
  if (e.handle != this->char_handle_)
    return;
  this->decoder_->decode(e.data, e.len);

  if (this->decoder_->has_position()) {
    this->position = ((float) this->decoder_->position_ / 100.0);
    if (!this->invert_position_)
      this->position = 1 - this->position;
    if (this->position > 0.97)
      this->position = 1.0;
    if (this->position < 0.02)
      this->position = 0.0;
    this->publish_state();
  }

  if (this->decoder_->has_pin_response()) {
    if (this->decoder_->pin_ok_) {
      ESP_LOGI(TAG, "[%s] AM43 pin accepted.", this->get_name().c_str());
      this->write_packet_(this->encoder_->get_position_request());
    } else {
      ESP_LOGW(TAG, "[%s] AM43 pin rejected!", this->get_name().c_str());
    }
  }
  if (this->decoder_->has_set_position_response() && !this->decoder_->set_position_ok_)
    ESP_LOGW(TAG, "[%s] Got nack after set_position. Bad pin?", this->get_name().c_str());
  if (this->decoder_->has_set_state_response() && !this->decoder_->set_state_ok_)
    ESP_LOGW(TAG, "[%s] Got nack after set_state. Bad pin?", this->get_name().c_str());
}

}  // namespace am43
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
