#include "esp32_ble_tracker.h"

#include "esphome/core/log.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

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

// Append one AD structure (len, type, data) to `out`. AD length is one byte, so
// oversized payloads are truncated to fit.
void append_ad(std::vector<uint8_t> &out, uint8_t type, const uint8_t *data, size_t len) {
  if (len > 254)
    len = 254;
  out.push_back(static_cast<uint8_t>(len + 1));
  out.push_back(type);
  out.insert(out.end(), data, data + len);
}

// Encode a BlueZ UUID string ("0000180f-0000-1000-8000-00805f9b34fb") as AD
// little-endian bytes: 2 bytes when it is a Bluetooth Base UUID with a 16-bit
// value, 4 for a 32-bit value, 16 otherwise. Returns 0 on a malformed string.
size_t encode_uuid_str(const char *str, uint8_t out[16]) {
  uint8_t be[16];
  size_t n = 0;
  for (const char *c = str; *c != '\0' && n < 32; c++) {
    if (*c == '-')
      continue;
    char hex[3] = {c[0], c[1], '\0'};
    if (c[1] == '\0')
      return 0;
    char *end = nullptr;
    long v = std::strtol(hex, &end, 16);
    if (end != hex + 2)
      return 0;
    be[n / 2] = static_cast<uint8_t>(v);
    n += 2;
    c++;
  }
  if (n != 32)
    return 0;
  static const uint8_t BASE_TAIL[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  if (std::memcmp(be + 4, BASE_TAIL, sizeof(BASE_TAIL)) == 0) {
    if (be[0] == 0 && be[1] == 0) {
      out[0] = be[3];
      out[1] = be[2];
      return 2;
    }
    for (int i = 0; i < 4; i++)
      out[i] = be[3 - i];
    return 4;
  }
  for (int i = 0; i < 16; i++)
    out[i] = be[15 - i];
  return 16;
}

}  // namespace

const char *esp_err_to_name(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return "ESP_OK";
    case ESP_GATT_NOT_CONNECTED:
      return "NOT_CONNECTED";
    case ESP_GATT_NOT_FOUND:
      return "NOT_FOUND";
    case ESP_GATT_WRITE_NOT_PERMIT:
      return "WRITE_NOT_PERMITTED";
    case ESP_GATT_READ_NOT_PERMIT:
      return "READ_NOT_PERMITTED";
    case ESP_GATT_INSUF_AUTHORIZATION:
      return "INSUF_AUTHORIZATION";
    case ESP_GATT_INVALID_OFFSET:
      return "INVALID_OFFSET";
    case ESP_GATT_INVALID_ATTR_LEN:
      return "INVALID_ATTR_LEN";
    case ESP_GATT_REQ_NOT_SUPPORTED:
      return "REQ_NOT_SUPPORTED";
    case ESP_GATT_ERROR:
      return "GATT_ERROR";
    default: {
      // Not reentrant, but only used for one-shot log lines.
      static char buf[20];
      std::snprintf(buf, sizeof(buf), "host-err(%d)", err);
      return buf;
    }
  }
}

float ESP32BLETracker::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void ESP32BLETracker::setup() {
  this->stop_thread_ = false;
  if (this->use_hci_backend_) {
    this->scanner_thread_ = std::thread([this] { this->scanner_thread_main_(); });
  } else {
    this->scanner_thread_ = std::thread([this] { this->dbus_scanner_thread_main_(); });
  }
  // Notify scanner-state listeners that scanning is running (host scan is
  // always-on once the worker starts).
  for (auto *l : this->scanner_state_listeners_)
    l->on_scanner_state(ScannerState::RUNNING);
}

void ESP32BLETracker::dump_config() {
  if (this->use_hci_backend_) {
    ESP_LOGCONFIG(TAG, "BLE Tracker (Linux raw HCI):");
    ESP_LOGCONFIG(TAG, "  HCI device: %s", this->hci_device_name_.c_str());
    ESP_LOGCONFIG(TAG, "  Scan interval: %u ms", this->scan_interval_ms_);
    ESP_LOGCONFIG(TAG, "  Scan window: %u ms", this->scan_window_ms_);
    ESP_LOGCONFIG(TAG, "  Active scan: %s", this->scan_active_ ? "yes" : "no");
  } else {
    ESP_LOGCONFIG(TAG, "BLE Tracker (Linux BlueZ D-Bus):");
    ESP_LOGCONFIG(TAG, "  Adapter: %s", this->hci_device_name_.c_str());
    ESP_LOGCONFIG(TAG, "  Active scan: %s", this->scan_active_ ? "yes" : "no");
  }
  ESP_LOGCONFIG(TAG, "  Listeners: %zu", this->listeners_.size());
}

void ESP32BLETracker::loop() {
  std::deque<PendingAdvertisement> drained;
  {
    std::lock_guard<std::mutex> g(this->queue_mu_);
    drained.swap(this->queue_);
  }
  for (auto &adv : drained) {
    if (this->raw_advertisement_callback_.is_set()) {
      ble_device_base::RawAdvertisement raw{};
      raw.address = ble_device_base::mac_lsb_first_to_uint64(adv.mac);
      raw.data = adv.data.data();
      raw.data_len = static_cast<uint16_t>(adv.data.size());
      raw.rssi = adv.rssi;
      raw.addr_type = adv.addr_type;
      this->raw_advertisement_callback_.invoke(raw);
    }
    if (this->listeners_.empty())
      continue;
    ESPBTDevice device;
    device.from_scan_result(adv.mac, adv.rssi, adv.addr_type, adv.data.data(), static_cast<uint16_t>(adv.data.size()));
    for (auto *listener : this->listeners_) {
      listener->parse_device(device);
    }
  }
  // Promote any client a listener just moved to DISCOVERED. Cheap fast-path:
  // only scan clients when a state change was signalled.
  if (!this->clients_.empty() && this->state_version_ != this->last_state_version_) {
    this->last_state_version_ = this->state_version_;
    this->try_promote_discovered_clients_();
  }
}

void ESP32BLETracker::try_promote_discovered_clients_() {
  for (auto *client : this->clients_) {
    if (client->state() == ClientState::DISCOVERED) {
      // BlueZ connects while discovery is active, so the scan is not stopped here.
      client->connect();
    }
  }
}

void ESP32BLETracker::deliver_(PendingAdvertisement adv) {
  ESP_LOGV(TAG, "Advertisement: %02X:%02X:%02X:%02X:%02X:%02X RSSI=%d len=%zu", adv.mac[5], adv.mac[4], adv.mac[3],
           adv.mac[2], adv.mac[1], adv.mac[0], adv.rssi, adv.data.size());
  std::lock_guard<std::mutex> g(this->queue_mu_);
  if (this->queue_.size() >= QUEUE_MAX)
    this->queue_.pop_front();
  this->queue_.push_back(std::move(adv));
}

void ESP32BLETracker::set_adapter_mac_(const uint8_t mac_lsb[6]) {
  std::lock_guard<std::mutex> g(this->queue_mu_);
  for (int i = 0; i < 6; i++)
    this->adapter_mac_[i] = mac_lsb[5 - i];
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

  // struct hci_dev_info: dev_id (u16), name[8], then bdaddr (6 bytes, LSB first).
  uint8_t info[128]{};
  info[0] = static_cast<uint8_t>(dev_id & 0xff);
  info[1] = static_cast<uint8_t>(dev_id >> 8);
  if (ioctl(fd, HCIGETDEVINFO_LOCAL, info) == 0)
    this->set_adapter_mac_(info + 10);
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
    // evt[pos] is the PDU type (ADV_IND, SCAN_RSP, ...); listeners don't need it.
    uint8_t addr_type = evt[pos + 1];
    const uint8_t *addr = evt + pos + 2;  // 6 bytes, little-endian (LSB first)
    uint8_t data_len = evt[pos + 8];
    if (pos + 9 + data_len + 1 > len)
      return;
    const uint8_t *data = evt + pos + 9;

    PendingAdvertisement adv;
    std::memcpy(adv.mac, addr, 6);
    adv.rssi = static_cast<int8_t>(evt[pos + 9 + data_len]);
    adv.addr_type = addr_type;
    adv.data.assign(data, data + data_len);
    this->deliver_(std::move(adv));

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

// ---------------------------------------------------------------------------
// BlueZ D-Bus backend (default).
//
// Talks to bluetoothd over the system bus, so it coexists with anything else
// using the adapter. It asks bluetoothd to discover, then reads the *parsed*
// org.bluez.Device1 properties (Address/RSSI/Name/UUIDs/ServiceData/
// ManufacturerData), re-encodes them as AD structures and feeds them into the
// same deliver_() path as the raw-HCI backend.
//
// BlueZ does NOT expose the raw advertising PDU here (only decoded fields); for
// byte-exact raw advertisements use the hci_backend opt-in.
// ---------------------------------------------------------------------------

namespace {

// Parse "AA:BB:CC:DD:EE:FF" into a controller-order (LSB-first) MAC.
bool parse_bdaddr(const char *str, uint8_t out[6]) {
  unsigned vals[6];
  if (std::sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6)
    return false;
  // String is big-endian (MSB first).
  for (int i = 0; i < 6; i++)
    out[i] = static_cast<uint8_t>(vals[5 - i]);
  return true;
}

// Read a D-Bus "ay" (array of bytes) at the current message position into vec.
int read_byte_array(sd_bus_message *m, std::vector<uint8_t> &vec) {
  const void *data = nullptr;
  size_t len = 0;
  int r = sd_bus_message_read_array(m, 'y', &data, &len);
  if (r < 0)
    return r;
  const uint8_t *p = static_cast<const uint8_t *>(data);
  vec.assign(p, p + len);
  return 0;
}

}  // namespace

// Parse a single org.bluez.Device1 property dictionary (a{sv}) that the message
// is currently positioned to enter, appending the decoded fields to adv.data as
// AD structures (ble_device_base parses those into ESPBTDevice). Returns true
// if an Address was found.
bool ESP32BLETracker::parse_device1_props_(sd_bus_message *m, PendingAdvertisement &adv) {
  bool have_address = false;
  std::vector<uint8_t> uuids16, uuids32, uuids128;
  int r = sd_bus_message_enter_container(m, 'a', "{sv}");
  if (r < 0)
    return false;
  for (;;) {
    r = sd_bus_message_enter_container(m, 'e', "sv");
    if (r <= 0)
      break;  // 0 = end of array, <0 = error
    const char *key = nullptr;
    if (sd_bus_message_read(m, "s", &key) < 0)
      break;

    if (std::strcmp(key, "Address") == 0) {
      const char *addr = nullptr;
      sd_bus_message_read(m, "v", "s", &addr);
      if (addr != nullptr && parse_bdaddr(addr, adv.mac))
        have_address = true;
    } else if (std::strcmp(key, "AddressType") == 0) {
      const char *type = nullptr;
      sd_bus_message_read(m, "v", "s", &type);
      if (type != nullptr && std::strcmp(type, "random") == 0)
        adv.addr_type = ble_device_base::BLE_ADDR_TYPE_RANDOM;
    } else if (std::strcmp(key, "Name") == 0) {
      const char *name = nullptr;
      sd_bus_message_read(m, "v", "s", &name);
      if (name != nullptr)
        append_ad(adv.data, AD_COMPLETE_LOCAL_NAME, reinterpret_cast<const uint8_t *>(name), std::strlen(name));
    } else if (std::strcmp(key, "RSSI") == 0) {
      int16_t rssi = 0;
      sd_bus_message_read(m, "v", "n", &rssi);
      adv.rssi = static_cast<int8_t>(rssi);
    } else if (std::strcmp(key, "TxPower") == 0) {
      int16_t tx = 0;
      sd_bus_message_read(m, "v", "n", &tx);
      uint8_t b = static_cast<uint8_t>(static_cast<int8_t>(tx));
      append_ad(adv.data, AD_TX_POWER_LEVEL, &b, 1);
    } else if (std::strcmp(key, "Appearance") == 0) {
      uint16_t app = 0;
      sd_bus_message_read(m, "v", "q", &app);
      uint8_t b[2] = {static_cast<uint8_t>(app & 0xff), static_cast<uint8_t>(app >> 8)};
      append_ad(adv.data, AD_APPEARANCE, b, 2);
    } else if (std::strcmp(key, "UUIDs") == 0) {
      sd_bus_message_enter_container(m, 'v', "as");
      sd_bus_message_enter_container(m, 'a', "s");
      const char *uuid = nullptr;
      while (sd_bus_message_read(m, "s", &uuid) > 0) {
        uint8_t le[16];
        size_t n = uuid != nullptr ? encode_uuid_str(uuid, le) : 0;
        auto &list = n == 2 ? uuids16 : n == 4 ? uuids32 : uuids128;
        if (n != 0)
          list.insert(list.end(), le, le + n);
      }
      sd_bus_message_exit_container(m);
      sd_bus_message_exit_container(m);
    } else if (std::strcmp(key, "ManufacturerData") == 0) {
      // v -> a{qv}, value variant is 'ay'. AD 0xFF carries the company id (LE)
      // ahead of the payload.
      sd_bus_message_enter_container(m, 'v', "a{qv}");
      sd_bus_message_enter_container(m, 'a', "{qv}");
      for (;;) {
        r = sd_bus_message_enter_container(m, 'e', "qv");
        if (r <= 0)
          break;
        uint16_t company = 0;
        sd_bus_message_read(m, "q", &company);
        sd_bus_message_enter_container(m, 'v', "ay");
        std::vector<uint8_t> field = {static_cast<uint8_t>(company & 0xff), static_cast<uint8_t>(company >> 8)};
        std::vector<uint8_t> payload;
        read_byte_array(m, payload);
        field.insert(field.end(), payload.begin(), payload.end());
        append_ad(adv.data, AD_MANUFACTURER_DATA, field.data(), field.size());
        sd_bus_message_exit_container(m);  // v
        sd_bus_message_exit_container(m);  // e
      }
      sd_bus_message_exit_container(m);  // a
      sd_bus_message_exit_container(m);  // v
    } else if (std::strcmp(key, "ServiceData") == 0) {
      // v -> a{sv}, key is UUID string, value variant is 'ay'
      sd_bus_message_enter_container(m, 'v', "a{sv}");
      sd_bus_message_enter_container(m, 'a', "{sv}");
      for (;;) {
        r = sd_bus_message_enter_container(m, 'e', "sv");
        if (r <= 0)
          break;
        const char *uuid = nullptr;
        sd_bus_message_read(m, "s", &uuid);
        sd_bus_message_enter_container(m, 'v', "ay");
        std::vector<uint8_t> payload;
        read_byte_array(m, payload);
        uint8_t le[16];
        size_t n = uuid != nullptr ? encode_uuid_str(uuid, le) : 0;
        if (n != 0) {
          std::vector<uint8_t> field(le, le + n);
          field.insert(field.end(), payload.begin(), payload.end());
          uint8_t type = n == 2 ? AD_SERVICE_DATA_UUID16 : n == 4 ? AD_SERVICE_DATA_UUID32 : AD_SERVICE_DATA_UUID128;
          append_ad(adv.data, type, field.data(), field.size());
        }
        sd_bus_message_exit_container(m);  // v
        sd_bus_message_exit_container(m);  // e
      }
      sd_bus_message_exit_container(m);  // a
      sd_bus_message_exit_container(m);  // v
    } else {
      // Skip the variant value of any property we don't care about.
      sd_bus_message_skip(m, "v");
    }
    sd_bus_message_exit_container(m);  // dict entry
  }
  sd_bus_message_exit_container(m);  // a{sv}
  if (!uuids16.empty())
    append_ad(adv.data, AD_COMPLETE_LIST_UUID16, uuids16.data(), uuids16.size());
  if (!uuids32.empty())
    append_ad(adv.data, AD_COMPLETE_LIST_UUID32, uuids32.data(), uuids32.size());
  if (!uuids128.empty())
    append_ad(adv.data, AD_COMPLETE_LIST_UUID128, uuids128.data(), uuids128.size());
  return have_address;
}

void ESP32BLETracker::dbus_scanner_thread_main_() {
  sd_bus *bus = nullptr;
  int r = sd_bus_open_system(&bus);
  if (r < 0) {
    ESP_LOGW(TAG, "D-Bus: cannot open system bus: %s", std::strerror(-r));
    return;
  }

  std::string adapter_path = "/org/bluez/" + this->hci_device_name_;

  // SetDiscoveryFilter: LE transport, keep duplicate adverts so we see the
  // advertisement firehose (otherwise BlueZ coalesces unchanged data).
  {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = nullptr;
    r = sd_bus_message_new_method_call(bus, &msg, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1",
                                       "SetDiscoveryFilter");
    if (r >= 0) {
      sd_bus_message_open_container(msg, 'a', "{sv}");
      sd_bus_message_open_container(msg, 'e', "sv");
      sd_bus_message_append(msg, "s", "Transport");
      sd_bus_message_append(msg, "v", "s", "le");
      sd_bus_message_close_container(msg);
      sd_bus_message_open_container(msg, 'e', "sv");
      sd_bus_message_append(msg, "s", "DuplicateData");
      sd_bus_message_append(msg, "v", "b", 1);
      sd_bus_message_close_container(msg);
      sd_bus_message_close_container(msg);
      r = sd_bus_call(bus, msg, 0, &err, nullptr);
      if (r < 0)
        ESP_LOGW(TAG, "D-Bus: SetDiscoveryFilter failed: %s", err.message ? err.message : std::strerror(-r));
      sd_bus_error_free(&err);
      sd_bus_message_unref(msg);
    }
  }

  // Subscribe to InterfacesAdded (new devices) and PropertiesChanged (updates).
  // The handlers are member-function trampolines via a static dispatcher.
  sd_bus_slot *slot_added = nullptr;
  sd_bus_slot *slot_changed = nullptr;
  sd_bus_match_signal(bus, &slot_added, "org.bluez", nullptr, "org.freedesktop.DBus.ObjectManager",
                      "InterfacesAdded", &ESP32BLETracker::on_interfaces_added_, this);
  sd_bus_match_signal(bus, &slot_changed, "org.bluez", nullptr, "org.freedesktop.DBus.Properties",
                      "PropertiesChanged", &ESP32BLETracker::on_properties_changed_, this);

  // StartDiscovery.
  {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bus, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", "StartDiscovery", &err,
                           nullptr, "");
    if (r < 0) {
      ESP_LOGW(TAG, "D-Bus: StartDiscovery failed: %s. Is bluetoothd running and the adapter powered?",
               err.message ? err.message : std::strerror(-r));
      sd_bus_error_free(&err);
      sd_bus_slot_unref(slot_added);
      sd_bus_slot_unref(slot_changed);
      sd_bus_unref(bus);
      return;
    }
    sd_bus_error_free(&err);
  }

  {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *addr = nullptr;
    uint8_t mac[6];
    if (sd_bus_get_property_string(bus, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", "Address", &err,
                                   &addr) >= 0 &&
        addr != nullptr && parse_bdaddr(addr, mac))
      this->set_adapter_mac_(mac);
    free(addr);  // NOLINT(cppcoreguidelines-no-malloc) -- sd-bus allocates with malloc
    sd_bus_error_free(&err);
  }

  this->hci_ok_ = true;
  ESP_LOGI(TAG, "BLE scan started via BlueZ D-Bus on %s", this->hci_device_name_.c_str());

  while (!this->stop_thread_.load()) {
    r = sd_bus_process(bus, nullptr);
    if (r < 0) {
      ESP_LOGW(TAG, "D-Bus process error: %s", std::strerror(-r));
      break;
    }
    if (r > 0)
      continue;  // more work queued, process it before waiting
    sd_bus_wait(bus, 200000);  // 200 ms, so stop_thread_ is checked promptly
  }

  {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_call_method(bus, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", "StopDiscovery", &err, nullptr,
                       "");
    sd_bus_error_free(&err);
  }
  sd_bus_slot_unref(slot_added);
  sd_bus_slot_unref(slot_changed);
  sd_bus_unref(bus);
}

// InterfacesAdded(o path, a{sa{sv}} interfaces) — a new device appeared. Find
// the org.bluez.Device1 entry and parse its properties.
int ESP32BLETracker::on_interfaces_added_(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<ESP32BLETracker *>(userdata);
  const char *obj_path = nullptr;
  if (sd_bus_message_read(m, "o", &obj_path) < 0)
    return 0;
  if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0)
    return 0;
  for (;;) {
    int r = sd_bus_message_enter_container(m, 'e', "sa{sv}");
    if (r <= 0)
      break;
    const char *iface = nullptr;
    sd_bus_message_read(m, "s", &iface);
    if (iface != nullptr && std::strcmp(iface, "org.bluez.Device1") == 0) {
      PendingAdvertisement adv{};
      if (self->parse_device1_props_(m, adv))
        self->deliver_(std::move(adv));
    } else {
      sd_bus_message_skip(m, "a{sv}");
    }
    sd_bus_message_exit_container(m);  // dict entry
  }
  sd_bus_message_exit_container(m);
  return 0;
}

// PropertiesChanged(s iface, a{sv} changed, as invalidated) on a device path —
// an existing device's RSSI / data updated. We only get the changed props, but
// for ble_rssi/ble_presence that's enough (RSSI + the MAC from the path).
int ESP32BLETracker::on_properties_changed_(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/) {
  auto *self = static_cast<ESP32BLETracker *>(userdata);
  const char *iface = nullptr;
  if (sd_bus_message_read(m, "s", &iface) < 0)
    return 0;
  if (iface == nullptr || std::strcmp(iface, "org.bluez.Device1") != 0)
    return 0;

  // The device MAC is in the signal's object path: /org/bluez/hciN/dev_AA_BB_..
  const char *path = sd_bus_message_get_path(m);
  PendingAdvertisement adv{};
  bool have_address = false;
  if (path != nullptr) {
    const char *dev = std::strstr(path, "/dev_");
    if (dev != nullptr) {
      char macbuf[18];
      // dev_AA_BB_CC_DD_EE_FF -> AA:BB:CC:DD:EE:FF
      std::snprintf(macbuf, sizeof(macbuf), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", dev[5], dev[6], dev[8], dev[9], dev[11],
                    dev[12], dev[14], dev[15], dev[17], dev[18], dev[20], dev[21]);
      have_address = parse_bdaddr(macbuf, adv.mac);
    }
  }
  if (!have_address)
    return 0;

  // Reuse the a{sv} parser for the changed-properties dict (same signature).
  self->parse_device1_props_(m, adv);
  self->deliver_(std::move(adv));
  return 0;
}

}  // namespace esp32_ble_tracker
}  // namespace esphome
