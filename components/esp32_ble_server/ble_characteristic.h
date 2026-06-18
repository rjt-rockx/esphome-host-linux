#pragma once

#include "ble_descriptor.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/bytebuffer/bytebuffer.h"

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// Host shadow of esp32_ble_server's BLECharacteristic. Owns value + properties +
// descriptors + on_read/on_write callbacks; the property bitmask uses the same
// PROPERTY_* constants as upstream so codegen's parse_properties() is unchanged.
// BLEGattServer exports each as an org.bluez GattCharacteristic1 and routes
// ReadValue/WriteValue/StartNotify/StopNotify here. notify() emits a Value
// PropertiesChanged via the server (BlueZ filters to subscribed clients).

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace esphome {
namespace esp32_ble_server {

using namespace esp32_ble;
using namespace bytebuffer;

class BLEService;
class BLEGattServer;

class BLECharacteristic {
 public:
  BLECharacteristic(ESPBTUUID uuid, uint32_t properties);
  ~BLECharacteristic();

  void set_value(ByteBuffer buffer);
  void set_value(std::vector<uint8_t> &&buffer);
  void set_value(std::initializer_list<uint8_t> data);
  void set_value(const std::string &buffer);

  void set_broadcast_property(bool value);
  void set_indicate_property(bool value);
  void set_notify_property(bool value);
  void set_read_property(bool value);
  void set_write_property(bool value);
  void set_write_no_response_property(bool value);

  void notify();

  void do_create(BLEService *service);
  void do_delete() {}

  void add_descriptor(BLEDescriptor *descriptor);
  void remove_descriptor(BLEDescriptor *descriptor);

  BLEService *get_service() { return this->service_; }
  ESPBTUUID get_uuid() { return this->uuid_; }
  std::vector<uint8_t> &get_value() { return this->value_; }

  static constexpr uint32_t PROPERTY_READ = 1 << 0;
  static constexpr uint32_t PROPERTY_WRITE = 1 << 1;
  static constexpr uint32_t PROPERTY_NOTIFY = 1 << 2;
  static constexpr uint32_t PROPERTY_BROADCAST = 1 << 3;
  static constexpr uint32_t PROPERTY_INDICATE = 1 << 4;
  static constexpr uint32_t PROPERTY_WRITE_NR = 1 << 5;

  bool is_created();
  bool is_failed();

  // Direct callback registration - only allocates when callback is set.
  void on_write(std::function<void(std::span<const uint8_t>, uint16_t)> &&callback) {
    this->on_write_callback_ =
        std::make_unique<std::function<void(std::span<const uint8_t>, uint16_t)>>(std::move(callback));
  }
  void on_read(std::function<void(uint16_t)> &&callback) {
    this->on_read_callback_ = std::make_unique<std::function<void(uint16_t)>>(std::move(callback));
  }

  // --- host worker/main hooks (used by BLEGattServer) ---
  uint32_t host_properties() const { return this->properties_; }
  const std::vector<BLEDescriptor *> &host_descriptors() const { return this->descriptors_; }
  bool host_has_notify() const { return (this->properties_ & (PROPERTY_NOTIFY | PROPERTY_INDICATE)) != 0; }
  // A client read this characteristic. Fire on_read (side-effects value_) then
  // return the bytes to serve. Runs on the bus worker thread.
  const std::vector<uint8_t> &host_on_read(uint16_t conn_id);
  // A client wrote this characteristic. Store bytes + fire on_write.
  void host_on_write(std::span<const uint8_t> data, uint16_t conn_id);

 protected:
  friend class BLEService;
  friend class BLEGattServer;

  BLEService *service_{nullptr};
  ESPBTUUID uuid_;
  uint32_t properties_;

  std::vector<uint8_t> value_;
  std::vector<BLEDescriptor *> descriptors_;

  void set_property_bit_(uint32_t bit, bool value);

  std::unique_ptr<std::function<void(std::span<const uint8_t>, uint16_t)>> on_write_callback_;
  std::unique_ptr<std::function<void(uint16_t)>> on_read_callback_;

  // Set once exported; lets notify() route to the right GattCharacteristic1 path.
  BLEGattServer *gatt_server_{nullptr};
  std::string object_path_;

  enum State : uint8_t {
    FAILED = 0x00,
    INIT,
    CREATING,
    CREATING_DEPENDENTS,
    CREATED,
  } state_{INIT};
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
