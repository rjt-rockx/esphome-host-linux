#pragma once

// BTHome v2 service-data encoder. Pure logic — no D-Bus, no esphome core, no
// hardware — so it is unit-testable standalone (see tests/unit/test_bthome_encoder.cpp).
//
// BTHome v2 (https://bthome.io/format/) is broadcast as a Service Data AD for the
// 16-bit UUID 0xFCD2. The payload is a device-information byte followed by a
// sequence of measurement objects, each = object-id byte + the value encoded
// little-endian and scaled by the object's factor. This encoder turns a list of
// (object id, human-scale value) into those bytes; the host advertiser then hands
// them to BlueZ as LEAdvertisement1.ServiceData{0xFCD2}.

#include <cstdint>
#include <vector>

namespace esphome {
namespace bthome_advertiser {

// One configured measurement: a BTHome object id and the human-scale value
// (e.g. object_id 0x02 / value 25.06 for 25.06 °C).
struct BTHomeObject {
  uint8_t object_id;
  double value;
};

// BTHome v2 device-information byte: protocol version 2 lives in bits 5-7
// (0b010 << 5 = 0x40); bit 0 is the encryption flag.
static constexpr uint8_t BTHOME_V2_DEVICE_INFO = 0x40;

// The 16-bit Service Data UUID BTHome broadcasts under, in canonical 128-bit form.
static constexpr const char *BTHOME_SERVICE_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb";

// Look up an object's wire spec (byte width, signedness, scale factor). Returns
// false if the id is not in the supported table.
bool bthome_object_spec(uint8_t object_id, uint8_t &size_out, bool &signed_out, double &factor_out);

// Build the BTHome v2 service-data payload (the bytes that follow the 0xFCD2 UUID
// in the Service Data AD): device-info byte, then each object's id + little-endian
// scaled value. Objects are emitted sorted ascending by id per the spec; unknown
// ids are skipped. Values are clamped to each field's representable range.
std::vector<uint8_t> build_bthome_payload(const std::vector<BTHomeObject> &objects, bool encrypted = false);

}  // namespace bthome_advertiser
}  // namespace esphome
