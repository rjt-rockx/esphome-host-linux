// Unit tests for ESPBTUUID (components/esp32_ble/ble_uuid.{h,cpp}).
//
// ESPBTUUID is pure logic — no D-Bus, no hardware — so it can be compiled and
// run standalone with g++. These tests lock the byte-level UUID contract that
// the GATT server, GATT client, scanner and beacon all depend on for
// service/characteristic matching.
//
// In particular they pin the regression behind commit b715122: the host shim's
// from_raw(const char *) / from_raw(std::string) used to memcpy the *ASCII* of a
// canonical UUID string instead of hex-parsing it, so a custom 128-bit
// characteristic UUID was exported to BlueZ as the bytes "a1b2..." and no central
// ever matched it. See test_regression_from_raw_string_parses_hex below.
//
// Build + run: tests/unit/run.sh  (or see that script for the exact g++ line).

#include "ble_uuid.h"

#include <cstdio>
#include <cstring>
#include <string>

using esphome::esp32_ble::ESPBTUUID;
using esphome::esp32_ble::esp_bt_uuid_t;
using esphome::esp32_ble::ESP_UUID_LEN_16;
using esphome::esp32_ble::ESP_UUID_LEN_32;
using esphome::esp32_ble::ESP_UUID_LEN_128;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);  \
    }                                                                     \
  } while (0)

#define CHECK_STR_EQ(actual, expected)                                              \
  do {                                                                              \
    g_checks++;                                                                     \
    std::string a_((actual));                                                       \
    std::string e_((expected));                                                     \
    if (a_ != e_) {                                                                 \
      g_failures++;                                                                 \
      std::printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__,       \
                  a_.c_str(), e_.c_str());                                          \
    }                                                                              \
  } while (0)

static std::string to_str(const ESPBTUUID &u) {
  char buf[ESPBTUUID::UUID_STR_LEN];
  u.to_str(buf);
  return std::string(buf);
}

// ---------------------------------------------------------------------------

static void test_128bit_canonical_roundtrip() {
  // from_uuid_str(canonical) -> to_str() must reproduce the canonical lowercase
  // string for full 128-bit UUIDs. This is the property BlueZ relies on.
  const char *uuids[] = {
      "a1b2c3d4-0001-1000-8000-00805f9b34fb",
      "0000fdf7-0000-1000-8000-00805f9b34fb",
      "12345678-9abc-def0-1234-56789abcdef0",
      "ffffffff-ffff-ffff-ffff-ffffffffffff",
      "00000000-0000-0000-0000-000000000000",
  };
  for (const char *s : uuids) {
    ESPBTUUID u = ESPBTUUID::from_uuid_str(s);
    CHECK(u.length() == 16);
    CHECK_STR_EQ(to_str(u), s);
  }
}

static void test_lsb_first_storage() {
  // raw_ is stored LSB-first (reverse of the canonical big-endian string).
  ESPBTUUID u = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  const uint8_t *r = u.raw();
  CHECK(r[0] == 0xfb);
  CHECK(r[1] == 0x34);
  CHECK(r[2] == 0x9b);
  CHECK(r[3] == 0x5f);
  CHECK(r[4] == 0x80);
  CHECK(r[5] == 0x00);
  CHECK(r[12] == 0xd4);
  CHECK(r[13] == 0xc3);
  CHECK(r[14] == 0xb2);
  CHECK(r[15] == 0xa1);
}

static void test_regression_from_raw_string_parses_hex() {
  // THE bug fix: from_raw(const char *) and from_raw(std::string) must HEX-PARSE
  // a canonical UUID string, not memcpy its ASCII. The old buggy behavior left
  // raw()[0] == 'a' (0x61) and length() == 16 (min(strlen,16)).
  const char *s = "a1b2c3d4-0001-1000-8000-00805f9b34fb";

  ESPBTUUID from_cstr = ESPBTUUID::from_raw(s);
  CHECK(from_cstr.length() == 16);
  CHECK(from_cstr.raw()[0] == 0xfb);   // hex-parsed LSB, NOT 'a'/0x61
  CHECK(from_cstr.raw()[0] != 'a');
  CHECK(from_cstr == ESPBTUUID::from_uuid_str(s));

  ESPBTUUID from_std = ESPBTUUID::from_raw(std::string(s));
  CHECK(from_std.length() == 16);
  CHECK(from_std.raw()[0] == 0xfb);
  CHECK(from_std == ESPBTUUID::from_uuid_str(s));

  // A central matching this characteristic compares the canonical string;
  // round-tripping it back out must reproduce the input exactly.
  CHECK_STR_EQ(to_str(from_cstr), s);
}

static void test_case_insensitive_parse() {
  ESPBTUUID lower = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  ESPBTUUID upper = ESPBTUUID::from_uuid_str("A1B2C3D4-0001-1000-8000-00805F9B34FB");
  CHECK(lower == upper);
}

static void test_from_uint16() {
  ESPBTUUID u = ESPBTUUID::from_uint16(0x1234);
  CHECK(u.length() == 2);
  CHECK(u.raw()[0] == 0x34);  // little-endian
  CHECK(u.raw()[1] == 0x12);
  CHECK(u.get_16bit() == 0x1234);
  CHECK_STR_EQ(u.to_string(), "0x1234");
}

static void test_from_uint32() {
  ESPBTUUID u = ESPBTUUID::from_uint32(0x12345678u);
  CHECK(u.length() == 4);
  CHECK(u.raw()[0] == 0x78);
  CHECK(u.raw()[3] == 0x12);
  CHECK(u.get_32bit() == 0x12345678u);
  CHECK_STR_EQ(u.to_string(), "0x12345678");
}

static void test_as_128bit_base_uuid_expansion() {
  // 16-bit 0x1234 expands using the Bluetooth Base UUID.
  ESPBTUUID u16 = ESPBTUUID::from_uint16(0x1234).as_128bit();
  CHECK(u16.length() == 16);
  CHECK_STR_EQ(to_str(u16), "00001234-0000-1000-8000-00805f9b34fb");

  // 32-bit expansion places the value in the high word.
  ESPBTUUID u32 = ESPBTUUID::from_uint32(0x12345678u).as_128bit();
  CHECK_STR_EQ(to_str(u32), "12345678-0000-1000-8000-00805f9b34fb");

  // Already-128-bit is identity.
  ESPBTUUID u128 = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  CHECK(u128.as_128bit() == u128);
}

static void test_from_raw_binary_vs_reversed() {
  // from_raw(const uint8_t*) stores 16 bytes verbatim (already LSB-first).
  uint8_t lsb[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x10, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1};
  ESPBTUUID u = ESPBTUUID::from_raw(lsb);
  CHECK(u.length() == 16);
  CHECK(std::memcmp(u.raw(), lsb, 16) == 0);
  // It equals parsing the canonical (big-endian) string of the same UUID.
  CHECK(u == ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb"));

  // from_raw_reversed flips byte order.
  uint8_t be[16] = {0xa1, 0xb2, 0xc3, 0xd4, 0x00, 0x01, 0x10, 0x00,
                    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
  ESPBTUUID rev = ESPBTUUID::from_raw_reversed(be);
  CHECK(rev == u);
}

static void test_equality_and_length_sensitivity() {
  CHECK(ESPBTUUID::from_uint16(0x1234) == ESPBTUUID::from_uint16(0x1234));
  CHECK(ESPBTUUID::from_uint16(0x1234) != ESPBTUUID::from_uint16(0x1235));
  // Same numeric value but different lengths must NOT be equal (raw vs expanded).
  CHECK(ESPBTUUID::from_uint16(0x1234) != ESPBTUUID::from_uint32(0x1234));
  CHECK(ESPBTUUID::from_uint16(0x1234) != ESPBTUUID::from_uint16(0x1234).as_128bit());

  ESPBTUUID a = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  ESPBTUUID b = ESPBTUUID::from_uuid_str("a1b2c3d4-0002-1000-8000-00805f9b34fb");
  CHECK(a != b);
  CHECK(a == ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb"));
}

static void test_contains() {
  // Service-data parsers (atc/pvvx/ruuvi) use contains() to match e.g. 0x181A
  // little-endian regardless of UUID length.
  ESPBTUUID u16 = ESPBTUUID::from_uint16(0x181a);
  CHECK(u16.contains(0x1a, 0x18));   // little-endian pair present
  CHECK(!u16.contains(0x18, 0x1a));  // wrong order absent

  ESPBTUUID u128 = ESPBTUUID::from_uuid_str("0000181a-0000-1000-8000-00805f9b34fb");
  CHECK(u128.contains(0x1a, 0x18));  // 0x181A LE appears at raw_[12..13]
}

static void test_get_uuid_union_view() {
  esp_bt_uuid_t v16 = ESPBTUUID::from_uint16(0x1234).get_uuid();
  CHECK(v16.len == ESP_UUID_LEN_16);
  CHECK(v16.uuid.uuid16 == 0x1234);

  esp_bt_uuid_t v32 = ESPBTUUID::from_uint32(0x12345678u).get_uuid();
  CHECK(v32.len == ESP_UUID_LEN_32);
  CHECK(v32.uuid.uuid32 == 0x12345678u);

  ESPBTUUID u128 = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  esp_bt_uuid_t v128 = u128.get_uuid();
  CHECK(v128.len == ESP_UUID_LEN_128);
  CHECK(std::memcmp(v128.uuid.uuid128, u128.raw(), 16) == 0);
}

static void test_from_uuid_roundtrip() {
  // from_uuid(esp_bt_uuid_t) is the GATT-server/advertising codegen path.
  for (ESPBTUUID orig : {ESPBTUUID::from_uint16(0xabcd), ESPBTUUID::from_uint32(0xdeadbeef),
                         ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb")}) {
    ESPBTUUID rt = ESPBTUUID::from_uuid(orig.get_uuid());
    CHECK(rt == orig);
  }
}

static void test_to_string_128bit_debug_form() {
  // to_string() emits raw_ in storage order (LSB-first) — a debug representation,
  // distinct from the canonical to_str(). Locked here so it doesn't silently drift.
  ESPBTUUID u = ESPBTUUID::from_uuid_str("a1b2c3d4-0001-1000-8000-00805f9b34fb");
  CHECK_STR_EQ(u.to_string(), "FB349B5F-8000-0080-0010-0100D4C3B2A1");
}

int main() {
  test_128bit_canonical_roundtrip();
  test_lsb_first_storage();
  test_regression_from_raw_string_parses_hex();
  test_case_insensitive_parse();
  test_from_uint16();
  test_from_uint32();
  test_as_128bit_base_uuid_expansion();
  test_from_raw_binary_vs_reversed();
  test_equality_and_length_sensitivity();
  test_contains();
  test_get_uuid_union_view();
  test_from_uuid_roundtrip();
  test_to_string_128bit_debug_form();

  std::printf("\nble_uuid: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
