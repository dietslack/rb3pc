// XeKeysSetKey / XeKeysAesCbc (SDK stubs them). Mogg versions 0x0C-0x10 run
// Grinder::HvDecrypt for the per-song key mask through wrappers at 0x82818790 and
// 0x82818828 (key into slot 0xE0+n, decrypt one block); stubbed, songs hang.
// The hypervisor transforms keys with a console secret, so the game's four
// HvDecrypt keys map to known effective values (hv_keys.h).

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

#include "hv_keys.h"

// AES-128 from the SDK's bundled thirdparty/aes_128 (exported by librexruntime).
extern "C" {
void aes_key_schedule_128(const uint8_t* key, uint8_t* roundkeys);
void aes_encrypt_128(const uint8_t* roundkeys, const uint8_t* plaintext, uint8_t* ciphertext);
void aes_decrypt_128(const uint8_t* roundkeys, const uint8_t* ciphertext, uint8_t* plaintext);
}

namespace {

constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
constexpr uint32_t kStatusNotFound = 0xC0000225;
constexpr size_t kBlock = 16;
constexpr size_t kRoundKeySize = 176;

struct KeySlot {
  bool set = false;
  std::array<uint8_t, kRoundKeySize> round_keys{};
};

std::mutex g_mutex;
std::array<KeySlot, 0x10000> g_slots;

}  // namespace

// NTSTATUS XeKeysSetKey(u16 key_id, const void* key, u32 key_size)
REX_HOOK_RAW(__imp__XeKeysSetKey) {
  uint32_t key_id = ctx.r3.u32 & 0xFFFF;
  uint32_t key_ptr = ctx.r4.u32;
  uint32_t key_size = ctx.r5.u32;
  if (!key_ptr || key_size != kBlock) {
    REXLOG_WARN("XeKeysSetKey: unsupported key {:#x} size {}", key_id, key_size);
    ctx.r3.u64 = kStatusInvalidParameter;
    return;
  }
  const uint8_t* key = base + key_ptr;
  int hv_index = -1;
  for (int i = 0; i < 4; ++i) {
    if (std::memcmp(key, rb3pc::kGameHvKeys[i], kBlock) == 0) {
      key = rb3pc::kEffectiveHvKeys[i];
      hv_index = i;
      break;
    }
  }
  std::lock_guard lock(g_mutex);
  auto& slot = g_slots[key_id];
  aes_key_schedule_128(key, slot.round_keys.data());
  if (!slot.set) {
    REXLOG_INFO("XeKeysSetKey: key {:#x} set ({})", key_id,
                hv_index >= 0 ? fmt::format("mogg HvDecrypt key {}", hv_index) : "as given");
  }
  slot.set = true;
  ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS XeKeysAesCbc(u32 key_id, const void* input, u32 input_size,
//                       void* output, void* feed /* IV, updated */, u32 encrypt)
REX_HOOK_RAW(__imp__XeKeysAesCbc) {
  uint32_t key_id = ctx.r3.u32 & 0xFFFF;
  uint32_t input = ctx.r4.u32;
  uint32_t size = ctx.r5.u32;
  uint32_t output = ctx.r6.u32;
  uint32_t feed = ctx.r7.u32;
  bool encrypt = ctx.r8.u32 != 0;
  if (!input || !output || size % kBlock) {
    ctx.r3.u64 = kStatusInvalidParameter;
    return;
  }

  std::array<uint8_t, kRoundKeySize> round_keys;
  {
    std::lock_guard lock(g_mutex);
    if (!g_slots[key_id].set) {
      REXLOG_WARN("XeKeysAesCbc: key {:#x} was never set", key_id);
      ctx.r3.u64 = kStatusNotFound;
      return;
    }
    round_keys = g_slots[key_id].round_keys;
  }

  uint8_t iv[kBlock] = {};
  if (feed) {
    std::memcpy(iv, base + feed, kBlock);
  }
  for (uint32_t offset = 0; offset < size; offset += kBlock) {
    uint8_t in[kBlock], out[kBlock];
    std::memcpy(in, base + input + offset, kBlock);
    if (encrypt) {
      for (size_t i = 0; i < kBlock; ++i) {
        in[i] ^= iv[i];
      }
      aes_encrypt_128(round_keys.data(), in, out);
      std::memcpy(iv, out, kBlock);
    } else {
      aes_decrypt_128(round_keys.data(), in, out);
      for (size_t i = 0; i < kBlock; ++i) {
        out[i] ^= iv[i];
      }
      std::memcpy(iv, in, kBlock);
    }
    std::memcpy(base + output + offset, out, kBlock);
  }
  if (feed) {
    std::memcpy(base + feed, iv, kBlock);
  }
  static std::atomic<uint32_t> calls{0};
  uint32_t n = ++calls;
  if (n <= 3 || (n & (n - 1)) == 0) {  // first few, then powers of two
    REXLOG_INFO("XeKeysAesCbc: key {:#x}, {} bytes, {} (call {})", key_id, size,
                encrypt ? "encrypt" : "decrypt", n);
  }
  ctx.r3.u64 = kStatusSuccess;
}
