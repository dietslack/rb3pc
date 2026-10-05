// Temporary diagnostic mid-asm hooks (see config/retail/debug.toml).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {

uint32_t GuestU32(uint32_t addr) {
  auto* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t v;
  std::memcpy(&v, base + addr, sizeof(v));
  return __builtin_bswap32(v);
}

uint8_t GuestU8(uint32_t addr) {
  return rex::system::kernel_state()->memory()->virtual_membase()[addr];
}

}  // namespace

void DebugD3DAllocResult(PPCRegister& r3, PPCRegister& r26, PPCRegister& r31) {
  uint32_t dev = r31.u32;
  // stderr, not the async logger: the interesting call is the one right
  // before a crash, and buffered log lines are lost when the process dies.
  std::fprintf(stderr,
               "[dbg] D3D alloc -> r3=%08X dwords=%u dev=%08X flags=%02X pool=%08X "
               "used=%08X limit=%08X\n",
               r3.u32, r26.u32, dev, GuestU8(dev + 10941), GuestU32(dev + 13728),
               GuestU32(dev + 15144), GuestU32(dev + 15148));
}

void DebugJoypadType(PPCRegister& r26, PPCRegister& r30) {
  static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
  uint32_t pad = r26.u32 & 3;
  if (last[pad] != r30.u32) {
    last[pad] = r30.u32;
    std::fprintf(stderr, "[dbg] RB3 joypad %u classified as type %u\n", r26.u32, r30.u32);
  }
}

void DebugAudioCallbackRate() {
  static std::atomic<uint32_t> count{0};
  static std::atomic<int64_t> window_start{0};
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  int64_t start = window_start.load();
  if (start == 0) {
    window_start = now;
  } else if (now != start && window_start.compare_exchange_strong(start, now)) {
    std::fprintf(stderr, "[dbg] audio callback: %u/s\n", count.exchange(0));
  }
  ++count;
}

void DebugAudioFramePeak(PPCRegister& r4) {
  static float peak = 0;
  static uint32_t frames = 0;
  auto* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t i = 0; i < 6 * 256; ++i) {
    uint32_t bits;
    std::memcpy(&bits, base + r4.u32 + i * 4, 4);
    bits = __builtin_bswap32(bits);
    float v;
    std::memcpy(&v, &bits, 4);
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  if (++frames == 188) {
    std::fprintf(stderr, "[dbg] audio peak over last second: %.4f\n", peak);
    peak = 0;
    frames = 0;
  }
}


