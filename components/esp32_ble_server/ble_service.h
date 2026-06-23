#pragma once

#include "ble_characteristic.h"
#include "esphome/components/esp32_ble/ble_uuid.h"

#if defined(USE_ESP32) || defined(USE_HOST)
#ifdef USE_HOST

// Host BLEService: a data holder owning its characteristics; BLEGattServer exports
// it as an org.bluez GattService1. Characteristics are allocated immediately and
// the whole tree goes out in one RegisterApplication.

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace esp32_ble_server {

using namespace esp32_ble;

class BLEServer;
class BLEGattServer;

class BLEService {
 public:
  BLEService(ESPBTUUID uuid, uint16_t num_handles, uint8_t inst_id, bool advertise);
  ~BLEService();
  BLECharacteristic *get_characteristic(ESPBTUUID uuid);
  BLECharacteristic *get_characteristic(uint16_t uuid);

  BLECharacteristic *create_characteristic(const std::string &uuid, uint32_t properties);
  BLECharacteristic *create_characteristic(uint16_t uuid, uint32_t properties);
  BLECharacteristic *create_characteristic(ESPBTUUID uuid, uint32_t properties);

  ESPBTUUID get_uuid() { return this->uuid_; }
  uint8_t get_inst_id() { return this->inst_id_; }
  BLECharacteristic *get_last_created_characteristic() { return this->last_created_characteristic_; }
  uint16_t get_handle() { return this->handle_; }

  BLEServer *get_server() { return this->server_; }

  void do_create(BLEServer *server);
  void do_delete() {}

  void start();
  void stop() {}

  bool is_failed() { return this->state_ == FAILED; }
  bool is_created() { return this->state_ == CREATED || this->state_ == RUNNING; }
  bool is_running() { return this->state_ == RUNNING; }
  bool is_starting() { return this->state_ == STARTING; }
  bool is_deleted() { return this->state_ == DELETED; }

  // --- host hooks (used by BLEGattServer) ---
  const std::vector<BLECharacteristic *> &host_characteristics() const { return this->characteristics_; }
  bool host_advertise() const { return this->advertise_; }

 protected:
  friend class BLEServer;
  friend class BLEGattServer;

  std::vector<BLECharacteristic *> characteristics_;
  BLECharacteristic *last_created_characteristic_{nullptr};
  BLEServer *server_{nullptr};
  ESPBTUUID uuid_;
  uint16_t num_handles_;
  uint16_t handle_{0xFFFF};
  uint8_t inst_id_;
  bool advertise_{false};

  enum State : uint8_t {
    FAILED = 0x00,
    INIT,
    CREATING,
    CREATED,
    STARTING,
    RUNNING,
    STOPPING,
    STOPPED,
    DELETING,
    DELETED,
  } state_{INIT};
};

}  // namespace esp32_ble_server
}  // namespace esphome

#endif  // USE_HOST
#endif  // USE_ESP32 || USE_HOST
