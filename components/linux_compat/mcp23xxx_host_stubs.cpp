// MCP23XXXBase<N> declares pin_mode and pin_interrupt_mode as non-pure
// virtual but provides no definitions. With RTTI enabled (host builds), the
// base vtable stays referenced and the linker requires these methods to
// exist, so define them here as explicit instantiations. Compiles to nothing
// unless mcp23xxx_base's header is present in the build.

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
