// Unit tests for the BTHome v2 encoder (components/bthome_advertiser/bthome_encoder.{h,cpp}).
//
// Pure logic — no D-Bus, no esphome core — so it compiles and runs standalone.
// These lock the exact on-air payload bytes the host advertiser hands to BlueZ as
// LEAdvertisement1.ServiceData{0xFCD2}, including the object sort order, scaling,
// signedness and clamping that must match the receive parser.

#include "bthome_encoder.h"

#include <cstdio>
#include <string>
#include <vector>

using esphome::bthome_advertiser::BTHomeObject;
using esphome::bthome_advertiser::build_bthome_payload;

static int g_checks = 0;
static int g_failures = 0;

static std::string hex(const std::vector<uint8_t> &v) {
  std::string s;
  char buf[4];
  for (size_t i = 0; i < v.size(); i++) {
    std::snprintf(buf, sizeof(buf), "%02x", v[i]);
    if (i)
      s += ' ';
    s += buf;
  }
  return s;
}

#define CHECK_PAYLOAD(objs, encrypted, expected)                                              \
  do {                                                                                        \
    g_checks++;                                                                               \
    std::string got = hex(build_bthome_payload((objs), (encrypted)));                         \
    std::string want = (expected);                                                            \
    if (got != want) {                                                                        \
      g_failures++;                                                                           \
      std::printf("FAIL %s:%d  got \"%s\"  want \"%s\"\n", __FILE__, __LINE__, got.c_str(),   \
                  want.c_str());                                                              \
    }                                                                                        \
  } while (0)

// Object ids used below: battery 0x01, temperature 0x02, humidity 0x03,
// pressure 0x04, voltage 0x0C, binary power 0x10.

int main() {
  // Canonical BTHome example: battery 93%, temp 25.06 °C, humidity 50.55 %.
  // Objects are emitted sorted ascending by id (battery, temp, humidity).
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x02, 25.06}, {0x03, 50.55}, {0x01, 93}}), false,
                "40 01 5d 02 ca 09 03 bf 13");

  // Sort independence: any input order yields the same sorted payload.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x01, 93}, {0x02, 25.06}, {0x03, 50.55}}), false,
                "40 01 5d 02 ca 09 03 bf 13");

  // Empty measurement list -> just the device-info byte.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{}), false, "40");

  // Encryption flag sets bit 0 of the device-info byte.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{}), true, "41");

  // Signed negative temperature: -10.0 -> -1000 -> 0xFC18 little-endian.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x02, -10.0}}), false, "40 02 18 fc");

  // Signed clamp: 400 °C clamps to int16 max 0x7FFF.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x02, 400.0}}), false, "40 02 ff 7f");

  // Unsigned clamp: 1000 % humidity clamps to uint16 max 0xFFFF.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x03, 1000.0}}), false, "40 03 ff ff");

  // Unsigned floor: negative humidity clamps to 0.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x03, -5.0}}), false, "40 03 00 00");

  // 3-byte pressure: 1013.25 hPa -> 101325 -> 0x018BCD little-endian.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x04, 1013.25}}), false, "40 04 cd 8b 01");

  // Voltage scale .001: 3.0 V -> 3000 -> 0x0BB8 little-endian.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x0C, 3.0}}), false, "40 0c b8 0b");

  // Binary types are 1 byte 0/1.
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x10, 1.0}}), false, "40 10 01");
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x10, 0.0}}), false, "40 10 00");

  // Unknown object id is skipped (only battery survives).
  CHECK_PAYLOAD((std::vector<BTHomeObject>{{0x99, 1.0}, {0x01, 93}}), false, "40 01 5d");

  std::printf("\nbthome_encoder: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
