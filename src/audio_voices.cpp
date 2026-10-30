// Sound effects whose XAudio2 voice can't be created. Starting a sample
// (SampleInst360's voice, start at retail sub_82B37728 / DX sub_82B663A8)
// creates a source voice into +80 and uses it unchecked, so a failed
// CreateSourceVoice was a null read that froze the game. The voice's other
// methods (stop, volume, is-playing) already allow a missing voice, which is
// how a stopped one looks: leave the sample stopped and silent instead.
// See config/{retail,dx}/hooks.toml.

#include <atomic>
#include <chrono>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {

constexpr uint32_t kVoice = 80;

bool HasNoVoice(const PPCRegister& voice_owner) {
  const uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  return !*reinterpret_cast<const uint32_t*>(base + voice_owner.u32 + kVoice);
}

}  // namespace

// Right after the voice is created: r3 = CreateSourceVoice's HRESULT,
// r30 = the voice owner. True skips to the start function's cleanup.
bool Rb3pcVoiceCreated(PPCRegister& r3, PPCRegister& r30) {
  if (!HasNoVoice(r30)) return false;
  // At most one line a second: a menu can try many sounds a second.
  using Clock = std::chrono::steady_clock;
  static std::atomic<int64_t> last_log{0};
  static std::atomic<uint32_t> skipped{0};
  uint32_t count = ++skipped;
  int64_t now = Clock::now().time_since_epoch() / std::chrono::milliseconds(1);
  int64_t last = last_log.load();
  if (now - last >= 1000 && last_log.compare_exchange_strong(last, now)) {
    REXLOG_WARN("Audio: a sound effect couldn't get a voice (0x{:08X}); skipped it ({} so far)", r3.u32, count);
  }
  return true;
}

// The caller, after the start function returns: true skips marking the
// sample as playing.
bool Rb3pcVoiceStarted(PPCRegister& r30) {
  return HasNoVoice(r30);
}
