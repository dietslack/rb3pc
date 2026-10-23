// XNetRandom: the SDK fills the buffer with 0xBB, and RB3 makes each local
// player's UserGuid from it, so every player had the same id. The game finds
// players by id (BandUserMgr::GetUserFromSlot looks the slot's id up), so a
// second controller joining the overshell was taken for player 1: same name,
// player 1's instrument. Real random bytes instead.

#include <cstdint>
#include <mutex>
#include <random>

#include <rex/hook.h>

namespace {

std::mutex g_mutex;
std::mt19937_64 g_random{std::random_device{}()};

}  // namespace

// INT NetDll_XNetRandom(u32 caller, u8* buffer, u32 length)
REX_HOOK_RAW(__imp__NetDll_XNetRandom) {
  uint32_t buffer = ctx.r4.u32, length = ctx.r5.u32;
  if (buffer) {
    std::lock_guard lock(g_mutex);
    for (uint32_t i = 0; i < length; ++i) base[buffer + i] = uint8_t(g_random());
  }
  ctx.r3.u64 = 0;
}
