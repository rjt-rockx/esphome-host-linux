#include "bthome_encoder.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace bthome_advertiser {

namespace {

// BTHome v2 object table: object id -> (byte width, signed, scale factor).
struct Spec {
  uint8_t id;
  uint8_t size;
  bool is_signed;
  double factor;
};

constexpr Spec TABLE[] = {
    {0x00, 1, false, 1.0},     // packet id
    {0x01, 1, false, 1.0},     // battery %
    {0x02, 2, true, 0.01},     // temperature  °C   (sint16)
    {0x03, 2, false, 0.01},    // humidity     %    (uint16)
    {0x04, 3, false, 0.01},    // pressure     hPa  (uint24)
    {0x05, 3, false, 0.01},    // illuminance  lux  (uint24)
    {0x0C, 2, false, 0.001},   // voltage      V    (uint16, mV on the wire)
    {0x12, 2, false, 1.0},     // CO2          ppm  (uint16)
    {0x13, 2, false, 1.0},     // TVOC         µg/m³(uint16)
    {0x14, 2, false, 0.01},    // moisture     %    (uint16)
    {0x10, 1, false, 1.0},     // binary: power
    {0x21, 1, false, 1.0},     // binary: motion
    {0x23, 1, false, 1.0},     // binary: occupancy
};

}  // namespace

bool bthome_object_spec(uint8_t object_id, uint8_t &size_out, bool &signed_out, double &factor_out) {
  for (const auto &s : TABLE) {
    if (s.id == object_id) {
      size_out = s.size;
      signed_out = s.is_signed;
      factor_out = s.factor;
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> build_bthome_payload(const std::vector<BTHomeObject> &objects, bool encrypted) {
  // BTHome v2 recommends measurement objects sorted ascending by id.
  std::vector<BTHomeObject> sorted(objects.begin(), objects.end());
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const BTHomeObject &a, const BTHomeObject &b) { return a.object_id < b.object_id; });

  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(BTHOME_V2_DEVICE_INFO | (encrypted ? 0x01 : 0x00)));

  for (const auto &o : sorted) {
    uint8_t size;
    bool is_signed;
    double factor;
    if (!bthome_object_spec(o.object_id, size, is_signed, factor))
      continue;  // unknown id — skip rather than emit garbage

    long long raw = std::llround(o.value / factor);

    // Clamp to the field's representable range so an out-of-range config can't
    // corrupt later objects in the payload.
    if (is_signed) {
      const long long lo = -(1LL << (size * 8 - 1));
      const long long hi = (1LL << (size * 8 - 1)) - 1;
      if (raw < lo)
        raw = lo;
      if (raw > hi)
        raw = hi;
    } else {
      if (raw < 0)
        raw = 0;
      const unsigned long long umax = (size >= 8) ? ~0ULL : ((1ULL << (size * 8)) - 1);
      if (static_cast<unsigned long long>(raw) > umax)
        raw = static_cast<long long>(umax);
    }

    const unsigned long long u = static_cast<unsigned long long>(raw);
    out.push_back(o.object_id);
    for (uint8_t i = 0; i < size; i++)
      out.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));  // little-endian
  }
  return out;
}

}  // namespace bthome_advertiser
}  // namespace esphome
