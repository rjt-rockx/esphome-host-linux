// Upstream's MCP23XXXBase<N> declares pin_mode and pin_interrupt_mode as
// non-pure virtual but never defines them. ESP32 builds with -fno-rtti, so
// the derived-class typeinfo doesn't chain to the base and --gc-sections can
// drop the orphan base vtable. Host builds keep RTTI, so the base vtable
// stays referenced and the linker demands the methods exist. Provide empty
// definitions here as an explicit template instantiation. Only compiled when
// mcp23xxx_base is actually in the build (the external_components copier
// pulls in its header iff a config references the component).

#ifdef USE_HOST
#if __has_include("esphome/components/mcp23xxx_base/mcp23xxx_base.h")

#include "esphome/components/mcp23xxx_base/mcp23xxx_base.h"

namespace esphome {
namespace mcp23xxx_base {

template<> void MCP23XXXBase<8>::pin_mode(uint8_t, gpio::Flags) {}
template<> void MCP23XXXBase<8>::pin_interrupt_mode(uint8_t, MCP23XXXInterruptMode) {}
template<> void MCP23XXXBase<16>::pin_mode(uint8_t, gpio::Flags) {}
template<> void MCP23XXXBase<16>::pin_interrupt_mode(uint8_t, MCP23XXXInterruptMode) {}

}  // namespace mcp23xxx_base
}  // namespace esphome

#endif
#endif
