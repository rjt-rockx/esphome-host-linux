#pragma once

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/bytebuffer/bytebuffer.h"

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// Host BLEDescriptor: a value + callback holder. The owning BLEGattServer
// exports it as an org.bluez GattDescriptor1 (except the 0x2902 CCCD, which BlueZ
// owns — see ble_2902.h). ReadValue serves value_; WriteValue copies bytes and
// fires on_write_callback_.

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace esphome {
namespace esp32_ble_server {

using namespace esp32_ble;
using namespace bytebuffer;

class BLECharacteristic;

// Base class for BLE descriptors.
class BLEDescriptor {
 public:
  BLEDescriptor(ESPBTUUID uuid, uint16_t max_len = 100, bool read = true, bool write = true);
  virtual ~BLEDescriptor();
  void do_create(BLECharacteristic *characteristic);
  ESPBTUUID get_uuid() const { return this->uuid_; }

  void set_value(std::vector<uint8_t> &&buffer);
  void set_value(std::initializer_list<uint8_t> data);
  void set_value(ByteBuffer buffer) { this->set_value(buffer.get_data()); }

  bool is_created() { return this->state_ == CREATED; }
  bool is_failed() { return this->state_ == FAILED; }

  // Direct callback registration - only allocates when callback is set.
  void on_write(std::function<void(std::span<const uint8_t>, uint16_t)> &&callback) {
    this->on_write_callback_ =
        std::make_unique<std::function<void(std::span<const uint8_t>, uint16_t)>>(std::move(callback));
  }

  // --- host worker hooks (called by BLEGattServer on the bus worker thread) ---
  // Current value bytes (served by ReadValue).
  const std::vector<uint8_t> &host_value() const { return this->value_; }
  bool host_readable() const { return this->read_; }
  bool host_writable() const { return this->write_; }
  // A client wrote this descriptor. Store bytes + fire on_write.
  void host_on_write(std::span<const uint8_t> data, uint16_t conn_id);

 protected:
  friend class BLECharacteristic;
  friend class BLEGattServer;

  void set_value_impl_(const uint8_t *data, size_t length);

  BLECharacteristic *characteristic_{nullptr};
  ESPBTUUID uuid_;

  std::vector<uint8_t> value_;
  uint16_t max_len_{100};
  bool read_{true};
  bool write_{true};

  std::unique_ptr<std::function<void(std::span<const uint8_t>, uint16_t)>> on_write_callback_;

  enum State : uint8_t {
    FAILED = 0x00,
    INIT,
    CREATING,
    CREATED,
  } state_{INIT};
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
