// Hooks only rb3pc-dx (DX's executable) has; see config/dx/hooks.toml.

#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

// Stands in for the code cave at 0x82516324 that sub_8275D670 branches to:
//   lwz r4,4(r3); cmpwi r4,2; beq -> mr r4,r30, back to 0x8275D6E4 (true)
//                             else -> back to 0x8275D6F0 (false)
// DX executables before 2025-03 have no cave: 0x8275D6E0 holds the original
// `mr r4,r30`, and execution goes on to 0x8275D6E4.
bool Rb3pcDxCave8275D6E0(PPCRegister& r3, PPCRegister& r4, PPCRegister& r30) {
  const uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  constexpr uint32_t kBranchToCave = 0x4BDB8C44;  // b 0x82516324
  if (__builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + 0x8275D6E0)) != kBranchToCave) {
    r4.u64 = r30.u64;
    return true;
  }
  r4.u64 = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + r3.u32 + 4));
  if (r4.u32 != 2) return false;
  r4.u64 = r30.u64;
  return true;
}
