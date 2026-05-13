#include "esp32_ble_tracker.h"

#include "esphome/core/log.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// BlueZ HCI raw socket constants. We can't depend on libbluetooth headers
// being installed (they often aren't on minimal Pi images), so we inline the
// pieces we need from <bluetooth/{bluetooth,hci,hci_sock}.h>.

namespace esphome {
namespace esp32_ble_tracker {

static const char *const TAG = "esp32_ble_tracker";

namespace {

constexpr int BTPROTO_HCI_LOCAL = 1;
constexpr int HCI_CHANNEL_RAW_LOCAL = 0;

constexpr uint16_t HCI_DEV_NONE_LOCAL = 0xffff;

constexpr int HCIDEVUP_LOCAL = 0x400448c9;
constexpr int HCIDEVDOWN_LOCAL = 0x400448ca;
constexpr int HCIGETDEVLIST_LOCAL = 0x800448d2;
constexpr int HCIGETDEVINFO_LOCAL = 0x800448d3;

constexpr uint8_t HCI_COMMAND_PKT = 0x01;
constexpr uint8_t HCI_EVENT_PKT = 0x04;

constexpr uint8_t EVT_LE_META_EVENT = 0x3e;
constexpr uint8_t EVT_CMD_COMPLETE = 0x0e;
constexpr uint8_t EVT_CMD_STATUS = 0x0f;

constexpr uint8_t SUBEVT_LE_ADVERTISING_REPORT = 0x02;
constexpr uint8_t SUBEVT_LE_EXTENDED_ADV_REPORT = 0x0d;

constexpr uint16_t OCF_LE_SET_SCAN_PARAMETERS = 0x000b;
constexpr uint16_t OCF_LE_SET_SCAN_ENABLE = 0x000c;
constexpr uint16_t OGF_LE_CTL = 0x08;

constexpr uint8_t AD_FLAGS = 0x01;
constexpr uint8_t AD_INCOMPLETE_LIST_UUID16 = 0x02;
constexpr uint8_t AD_COMPLETE_LIST_UUID16 = 0x03;
constexpr uint8_t AD_INCOMPLETE_LIST_UUID32 = 0x04;
constexpr uint8_t AD_COMPLETE_LIST_UUID32 = 0x05;
constexpr uint8_t AD_INCOMPLETE_LIST_UUID128 = 0x06;
constexpr uint8_t AD_COMPLETE_LIST_UUID128 = 0x07;
constexpr uint8_t AD_SHORTENED_LOCAL_NAME = 0x08;
constexpr uint8_t AD_COMPLETE_LOCAL_NAME = 0x09;
constexpr uint8_t AD_TX_POWER_LEVEL = 0x0a;
constexpr uint8_t AD_SERVICE_DATA_UUID16 = 0x16;
constexpr uint8_t AD_SERVICE_DATA_UUID32 = 0x20;
constexpr uint8_t AD_SERVICE_DATA_UUID128 = 0x21;
constexpr uint8_t AD_APPEARANCE = 0x19;
constexpr uint8_t AD_MANUFACTURER_DATA = 0xff;

struct __attribute__((packed)) sockaddr_hci_local {
  uint16_t hci_family;  // AF_BLUETOOTH = 31
  uint16_t hci_dev;
  uint16_t hci_channel;
};

constexpr uint16_t AF_BLUETOOTH_LOCAL = 31;

// Kernel `struct hci_filter` is NOT packed: opcode at offset 12 leaves 2 bytes
// of padding, making sizeof == 16. Mismatched optlen yields EINVAL.
struct hci_filter_local {
  uint32_t type_mask;
  uint32_t event_mask[2];
  uint16_t opcode;
};

constexpr int SOL_HCI_LOCAL = 0;
constexpr int HCI_FILTER_LOCAL = 2;

inline void hci_filter_set_ptype(int t, hci_filter_local *f) { f->type_mask |= (1u << (t & 0x1f)); }
inline void hci_filter_set_event(int e, hci_filter_local *f) {
  if (e < 32) {
    f->event_mask[0] |= (1u << e);
  } else {
    f->event_mask[1] |= (1u << (e - 32));
  }
}

inline uint16_t hci_opcode(uint16_t ogf, uint16_t ocf) {
  return static_cast<uint16_t>((ogf << 10) | (ocf & 0x03ff));
}

void parse_ad_payload(const uint8_t *payload, uint8_t len, ESPBTDevice &device) {
  size_t pos = 0;
  while (pos + 1 < len) {
    uint8_t field_len = payload[pos];
    if (field_len == 0 || pos + 1 + field_len > len)
      break;
    uint8_t type = payload[pos + 1];
    const uint8_t *data = payload + pos + 2;
    uint8_t data_len = field_len - 1;

    switch (type) {
      case AD_FLAGS:
        if (data_len > 0)
          device.set_ad_flag(data[0]);
        break;
      case AD_INCOMPLETE_LIST_UUID16:
      case AD_COMPLETE_LIST_UUID16:
        for (size_t i = 0; i + 1 < data_len; i += 2) {
          uint16_t u = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
          device.add_service_uuid(ESPBTUUID::from_uint16(u));
        }
        break;
      case AD_INCOMPLETE_LIST_UUID32:
      case AD_COMPLETE_LIST_UUID32:
        for (size_t i = 0; i + 3 < data_len; i += 4) {
          uint32_t u = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                       (static_cast<uint32_t>(data[i + 2]) << 16) | (static_cast<uint32_t>(data[i + 3]) << 24);
          device.add_service_uuid(ESPBTUUID::from_uint32(u));
        }
        break;
      case AD_INCOMPLETE_LIST_UUID128:
      case AD_COMPLETE_LIST_UUID128:
        for (size_t i = 0; i + 15 < data_len; i += 16) {
          device.add_service_uuid(ESPBTUUID::from_raw(data + i));
        }
        break;
      case AD_SHORTENED_LOCAL_NAME:
      case AD_COMPLETE_LOCAL_NAME:
        device.set_name(std::string(reinterpret_cast<const char *>(data), data_len));
        break;
      case AD_TX_POWER_LEVEL:
        if (data_len > 0)
          device.add_tx_power(static_cast<int8_t>(data[0]));
        break;
      case AD_APPEARANCE:
        if (data_len >= 2)
          device.set_appearance(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
        break;
      case AD_SERVICE_DATA_UUID16: {
        if (data_len >= 2) {
          ServiceData sd;
          uint16_t u = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
          sd.uuid = ESPBTUUID::from_uint16(u);
          sd.data.assign(data + 2, data + data_len);
          device.add_service_data(std::move(sd));
        }
        break;
      }
      case AD_SERVICE_DATA_UUID32: {
        if (data_len >= 4) {
          ServiceData sd;
          uint32_t u = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                       (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
          sd.uuid = ESPBTUUID::from_uint32(u);
          sd.data.assign(data + 4, data + data_len);
          device.add_service_data(std::move(sd));
        }
        break;
      }
      case AD_SERVICE_DATA_UUID128: {
        if (data_len >= 16) {
          ServiceData sd;
          sd.uuid = ESPBTUUID::from_raw(data);
          sd.data.assign(data + 16, data + data_len);
          device.add_service_data(std::move(sd));
        }
        break;
      }
      case AD_MANUFACTURER_DATA: {
        if (data_len >= 2) {
          ServiceData sd;
          uint16_t u = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
          sd.uuid = ESPBTUUID::from_uint16(u);
          sd.data.assign(data, data + data_len);
          device.add_manufacturer_data(std::move(sd));
        }
        break;
      }
      default:
        break;
    }
    pos += 1 + field_len;
  }
}

}  // namespace

optional<ESPBLEiBeacon> ESPBLEiBeacon::from_manufacturer_data(const ServiceData &data) {
  // Manufacturer data layout in advertisement: <2-byte company ID LE><payload>.
  // We packed it as: data[0..1] = company, data[2..] = payload.
  if (data.data.size() < 2 + sizeof(ESPBLEiBeacon::beacon_data_))
    return {};
  // Apple = 0x004c, iBeacon sub-type = 0x02 length 0x15.
  if (data.data[0] != 0x4c || data.data[1] != 0x00)
    return {};
  if (data.data[2] != 0x02 || data.data[3] != 0x15)
    return {};
  return ESPBLEiBeacon(data.data.data() + 2);
}

std::string ESPBTDevice::address_str() const {
  char buf[18];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", this->address_[5], this->address_[4],
                this->address_[3], this->address_[2], this->address_[1], this->address_[0]);
  return std::string(buf);
}

uint64_t ESPBTDevice::address_uint64() const {
  uint64_t v = 0;
  for (int i = 5; i >= 0; i--)
    v = (v << 8) | this->address_[i];
  return v;
}

float ESP32BLETracker::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void ESP32BLETracker::setup() {
  this->stop_thread_ = false;
  this->scanner_thread_ = std::thread([this] { this->scanner_thread_main_(); });
}

void ESP32BLETracker::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE Tracker (Linux HCI):");
  ESP_LOGCONFIG(TAG, "  HCI device: %s", this->hci_device_name_.c_str());
  ESP_LOGCONFIG(TAG, "  Scan interval: %u ms", this->scan_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Scan window: %u ms", this->scan_window_ms_);
  ESP_LOGCONFIG(TAG, "  Active scan: %s", this->scan_active_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Listeners: %zu", this->listeners_.size());
}

void ESP32BLETracker::loop() {
  std::deque<ESPBTDevice> drained;
  {
    std::lock_guard<std::mutex> g(this->queue_mu_);
    drained.swap(this->queue_);
  }
  if (drained.empty())
    return;
  for (auto &device : drained) {
    for (auto *listener : this->listeners_) {
      listener->parse_device(device);
    }
  }
}

void ESP32BLETracker::deliver_device_(ESPBTDevice device) {
  ESP_LOGV(TAG, "Advertisement: %s RSSI=%d name='%s' service_uuids=%zu", device.address_str().c_str(),
           device.get_rssi(), device.get_name().c_str(), device.get_service_uuids().size());
  std::lock_guard<std::mutex> g(this->queue_mu_);
  if (this->queue_.size() >= QUEUE_MAX)
    this->queue_.pop_front();
  this->queue_.push_back(std::move(device));
}

bool ESP32BLETracker::open_hci_() {
  int dev_id = -1;
  if (this->hci_device_name_.size() > 3 && this->hci_device_name_.rfind("hci", 0) == 0) {
    dev_id = std::atoi(this->hci_device_name_.c_str() + 3);
  }
  if (dev_id < 0) {
    ESP_LOGW(TAG, "Invalid HCI device name '%s'", this->hci_device_name_.c_str());
    return false;
  }
  int fd = socket(AF_BLUETOOTH_LOCAL, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI_LOCAL);
  if (fd < 0) {
    ESP_LOGW(TAG, "socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI) failed: %s — BLE scan disabled. "
                  "Run with cap_net_admin,cap_net_raw or as root.",
             std::strerror(errno));
    return false;
  }

  hci_filter_local flt{};
  hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
  hci_filter_set_event(EVT_LE_META_EVENT, &flt);
  hci_filter_set_event(EVT_CMD_COMPLETE, &flt);
  hci_filter_set_event(EVT_CMD_STATUS, &flt);
  if (setsockopt(fd, SOL_HCI_LOCAL, HCI_FILTER_LOCAL, &flt, sizeof(flt)) < 0) {
    ESP_LOGW(TAG, "HCI_FILTER setsockopt failed: %s", std::strerror(errno));
    ::close(fd);
    return false;
  }

  sockaddr_hci_local addr{};
  addr.hci_family = AF_BLUETOOTH_LOCAL;
  addr.hci_dev = static_cast<uint16_t>(dev_id);
  addr.hci_channel = HCI_CHANNEL_RAW_LOCAL;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGW(TAG, "HCI bind(%s) failed: %s", this->hci_device_name_.c_str(), std::strerror(errno));
    ::close(fd);
    return false;
  }

  this->hci_fd_ = fd;
  this->hci_dev_id_ = dev_id;
  return true;
}

void ESP32BLETracker::close_hci_() {
  if (this->hci_fd_ >= 0) {
    ::close(this->hci_fd_);
    this->hci_fd_ = -1;
  }
}

bool ESP32BLETracker::send_le_set_scan_params_() {
  struct __attribute__((packed)) {
    uint8_t pkt;
    uint16_t opcode;
    uint8_t plen;
    uint8_t scan_type;
    uint16_t interval;
    uint16_t window;
    uint8_t own_addr;
    uint8_t filter;
  } cmd{};
  cmd.pkt = HCI_COMMAND_PKT;
  cmd.opcode = hci_opcode(OGF_LE_CTL, OCF_LE_SET_SCAN_PARAMETERS);
  cmd.plen = 7;
  cmd.scan_type = this->scan_active_ ? 0x01 : 0x00;
  cmd.interval = static_cast<uint16_t>(this->scan_interval_ms_ * 1000 / 625);
  cmd.window = static_cast<uint16_t>(this->scan_window_ms_ * 1000 / 625);
  cmd.own_addr = 0x00;
  cmd.filter = 0x00;
  ssize_t n = write(this->hci_fd_, &cmd, sizeof(cmd));
  if (n != sizeof(cmd)) {
    ESP_LOGW(TAG, "LE set scan params write failed: %s", std::strerror(errno));
    return false;
  }
  return true;
}

bool ESP32BLETracker::send_le_set_scan_enable_(bool enable) {
  struct __attribute__((packed)) {
    uint8_t pkt;
    uint16_t opcode;
    uint8_t plen;
    uint8_t enable;
    uint8_t filter_dup;
  } cmd{};
  cmd.pkt = HCI_COMMAND_PKT;
  cmd.opcode = hci_opcode(OGF_LE_CTL, OCF_LE_SET_SCAN_ENABLE);
  cmd.plen = 2;
  cmd.enable = enable ? 0x01 : 0x00;
  cmd.filter_dup = 0x00;
  ssize_t n = write(this->hci_fd_, &cmd, sizeof(cmd));
  if (n != sizeof(cmd)) {
    ESP_LOGW(TAG, "LE set scan enable=%d write failed: %s", enable, std::strerror(errno));
    return false;
  }
  return true;
}

void ESP32BLETracker::handle_le_meta_event_(const uint8_t *evt, size_t len) {
  if (len < 1)
    return;
  uint8_t sub = evt[0];
  if (sub != SUBEVT_LE_ADVERTISING_REPORT)
    return;
  if (len < 2)
    return;
  uint8_t num_reports = evt[1];
  size_t pos = 2;
  for (uint8_t i = 0; i < num_reports; i++) {
    if (pos + 9 > len)
      return;
    uint8_t /*evt_type*/ _t = evt[pos];
    (void) _t;
    uint8_t /*addr_type*/ _at = evt[pos + 1];
    (void) _at;
    const uint8_t *addr = evt + pos + 2;  // 6 bytes, little-endian (LSB first)
    uint8_t data_len = evt[pos + 8];
    if (pos + 9 + data_len + 1 > len)
      return;
    const uint8_t *data = evt + pos + 9;
    int8_t rssi = static_cast<int8_t>(evt[pos + 9 + data_len]);

    ESPBTDevice device;
    device.set_address(addr);
    device.set_rssi(rssi);
    parse_ad_payload(data, data_len, device);
    this->deliver_device_(std::move(device));

    pos += 9 + data_len + 1;
  }
}

void ESP32BLETracker::scanner_thread_main_() {
  if (!this->open_hci_()) {
    return;
  }
  // Stop any in-progress scan from a previous owner so set_scan_params succeeds.
  this->send_le_set_scan_enable_(false);
  if (!this->send_le_set_scan_params_()) {
    ESP_LOGW(TAG, "BLE scan disabled. Grant the binary HCI capabilities once: "
                  "sudo setcap 'cap_net_admin,cap_net_raw+eip' <program>");
    this->close_hci_();
    return;
  }
  if (!this->send_le_set_scan_enable_(true)) {
    ESP_LOGW(TAG, "BLE scan enable rejected. Grant the binary HCI capabilities once: "
                  "sudo setcap 'cap_net_admin,cap_net_raw+eip' <program>");
    this->close_hci_();
    return;
  }
  this->hci_ok_ = true;
  ESP_LOGI(TAG, "BLE scan started on %s", this->hci_device_name_.c_str());

  uint8_t buf[1024];
  while (!this->stop_thread_.load()) {
    ssize_t n = read(this->hci_fd_, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ESP_LOGW(TAG, "HCI read error: %s", std::strerror(errno));
      break;
    }
    if (n < 3)
      continue;
    if (buf[0] != HCI_EVENT_PKT)
      continue;
    uint8_t evt_code = buf[1];
    uint8_t plen = buf[2];
    if (static_cast<size_t>(n) < 3u + plen)
      continue;
    if (evt_code == EVT_LE_META_EVENT) {
      this->handle_le_meta_event_(buf + 3, plen);
    }
  }
  this->send_le_set_scan_enable_(false);
  this->close_hci_();
}

}  // namespace esp32_ble_tracker
}  // namespace esphome
