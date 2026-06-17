#pragma once
#if defined(USE_ESP32) || defined(USE_HOST)

#include "ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/automation.h"
#include "esphome/core/log.h"

#include <vector>

namespace esphome {
namespace ble_client {

namespace espbt = esphome::esp32_ble_tracker;

class BLEClientConnectTrigger : public Trigger<>, public BLEClientNode {
 public:
  explicit BLEClientConnectTrigger(BLEClient *parent) { parent->register_ble_node(this); }
  void on_services_discovered() override { this->trigger(); }
};

class BLEClientDisconnectTrigger : public Trigger<>, public BLEClientNode {
 public:
  explicit BLEClientDisconnectTrigger(BLEClient *parent) { parent->register_ble_node(this); }
  void on_disconnected(int /*reason*/) override { this->trigger(); }
};

// Write a value (static bytes or a templated lambda) to a characteristic, then
// advance the action chain when the write completes (works for both write
// types — WRITE_COMPLETE is always posted).
template<typename... Ts> class BLEClientWriteAction : public Action<Ts...>, public BLEClientNode {
 public:
  explicit BLEClientWriteAction(BLEClient *parent) { parent->register_ble_node(this); }

  void set_service_uuid16(uint16_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_service_uuid32(uint32_t uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_service_uuid128(uint8_t *uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_char_uuid16(uint16_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_char_uuid32(uint32_t uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_uint32(uuid); }
  void set_char_uuid128(uint8_t *uuid) { this->char_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_value_simple(const std::vector<uint8_t> &value) { this->value_simple_ = value; }
  void set_value_template(std::vector<uint8_t> (*func)(Ts...)) {
    this->value_template_ = func;
    this->has_template_ = true;
  }

  void play_complex(const Ts &...x) override {
    this->num_running_++;
    this->var_ = std::make_tuple(x...);
    auto *chr = this->parent()->get_characteristic(this->service_uuid_, this->char_uuid_);
    if (chr == nullptr) {
      ESP_LOGW("ble_write", "characteristic not found");
      this->play_next_(x...);
      return;
    }
    this->write_handle_ = chr->handle;
    std::vector<uint8_t> value = this->has_template_ ? (*this->value_template_)(x...) : this->value_simple_;
    bool response = (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) != 0;
    this->parent()->write_characteristic(chr->handle, value.data(), value.size(), response);
  }

  void on_characteristic_write(uint16_t handle, int /*status*/) override {
    if (handle != this->write_handle_)
      return;
    this->write_handle_ = 0;
    this->play_next_tuple_(this->var_);
  }

  void play(const Ts &...x) override {}  // unused; play_complex drives the chain

 protected:
  espbt::ESPBTUUID service_uuid_;
  espbt::ESPBTUUID char_uuid_;
  std::vector<uint8_t> value_simple_;
  std::vector<uint8_t> (*value_template_)(Ts...){nullptr};
  bool has_template_{false};
  uint16_t write_handle_{0};
  std::tuple<Ts...> var_{};
};

// --- pairing triggers/actions ---
class BLEClientPasskeyRequestTrigger : public Trigger<>, public BLEClientNode {
 public:
  explicit BLEClientPasskeyRequestTrigger(BLEClient *parent) { parent->register_ble_node(this); }
  void on_passkey_request() override { this->trigger(); }
};
class BLEClientPasskeyNotificationTrigger : public Trigger<uint32_t>, public BLEClientNode {
 public:
  explicit BLEClientPasskeyNotificationTrigger(BLEClient *parent) { parent->register_ble_node(this); }
  void on_passkey_notification(uint32_t passkey) override { this->trigger(passkey); }
};
class BLEClientNumericComparisonRequestTrigger : public Trigger<uint32_t>, public BLEClientNode {
 public:
  explicit BLEClientNumericComparisonRequestTrigger(BLEClient *parent) { parent->register_ble_node(this); }
  void on_numeric_comparison_request(uint32_t passkey) override { this->trigger(passkey); }
};

template<typename... Ts> class BLEClientPasskeyReplyAction : public Action<Ts...> {
 public:
  explicit BLEClientPasskeyReplyAction(BLEClient *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint32_t, passkey)
  void play(const Ts &...x) override { this->parent_->passkey_reply(this->passkey_.value(x...)); }

 protected:
  BLEClient *parent_;
};
template<typename... Ts> class BLEClientNumericComparisonReplyAction : public Action<Ts...> {
 public:
  explicit BLEClientNumericComparisonReplyAction(BLEClient *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, accept)
  void play(const Ts &...x) override { this->parent_->confirm_reply(this->accept_.value(x...)); }

 protected:
  BLEClient *parent_;
};
template<typename... Ts> class BLEClientRemoveBondAction : public Action<Ts...> {
 public:
  explicit BLEClientRemoveBondAction(BLEClient *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->remove_bond(); }

 protected:
  BLEClient *parent_;
};

template<typename... Ts> class BLEClientConnectAction : public Action<Ts...>, public BLEClientNode {
 public:
  explicit BLEClientConnectAction(BLEClient *parent) : parent_node_(parent) { parent->register_ble_node(this); }
  void play_complex(const Ts &...x) override {
    this->num_running_++;
    this->var_ = std::make_tuple(x...);
    this->parent_node_->set_enabled(true);
    this->parent_node_->connect();
  }
  void on_services_discovered() override { this->play_next_tuple_(this->var_); }
  void play(const Ts &...x) override {}

 protected:
  BLEClient *parent_node_;
  std::tuple<Ts...> var_{};
};

template<typename... Ts> class BLEClientDisconnectAction : public Action<Ts...>, public BLEClientNode {
 public:
  explicit BLEClientDisconnectAction(BLEClient *parent) : parent_node_(parent) { parent->register_ble_node(this); }
  void play_complex(const Ts &...x) override {
    this->num_running_++;
    this->var_ = std::make_tuple(x...);
    this->parent_node_->set_enabled(false);
    this->parent_node_->disconnect();
  }
  void on_disconnected(int /*reason*/) override { this->play_next_tuple_(this->var_); }
  void play(const Ts &...x) override {}

 protected:
  BLEClient *parent_node_;
  std::tuple<Ts...> var_{};
};

}  // namespace ble_client
}  // namespace esphome

#endif  // USE_ESP32 || USE_HOST
