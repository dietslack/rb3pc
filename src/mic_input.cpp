#include "mic_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(mic_1, "", "rb3pc",
                      "Microphone 1 (lead vocals / harmony 1): a recording device's name, empty "
                      "for none");
REXCVAR_DEFINE_STRING(mic_2, "", "rb3pc", "Microphone 2 (harmony 2): a recording device's name");
REXCVAR_DEFINE_STRING(mic_3, "", "rb3pc", "Microphone 3 (harmony 3): a recording device's name");

namespace rb3pc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;  // what the game's mics take

struct Mic {
  std::string device;  // the device open (or wanted but missing)
  SDL_AudioStream* stream = nullptr;
  float level = 0;
  Clock::time_point game_read{};  // the game read it last
};

std::mutex g_mutex;
std::array<Mic, kMicCount> g_mics;
Clock::time_point g_synced{};

bool InitAudio() {
  static bool ok = [] {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return true;
    REXLOG_ERROR("Microphones: SDL audio init failed: {}", SDL_GetError());
    return false;
  }();
  return ok;
}

std::vector<std::pair<SDL_AudioDeviceID, std::string>> Devices() {
  std::vector<std::pair<SDL_AudioDeviceID, std::string>> devices;
  if (!InitAudio()) return devices;
  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
  for (int i = 0; ids && i < count; ++i) {
    if (const char* name = SDL_GetAudioDeviceName(ids[i])) devices.emplace_back(ids[i], name);
  }
  SDL_free(ids);
  return devices;
}

// Follows settings and hotplug, at most once a second. g_mutex held.
void SyncLocked() {
  Clock::time_point now = Clock::now();
  if (now - g_synced < std::chrono::seconds(1)) return;
  g_synced = now;
  auto devices = Devices();
  static bool logged = false;
  if (!logged) {
    logged = true;
    for (auto& device : devices) REXLOG_INFO("Microphones: recording device {}", device.second);
  }
  for (int i = 0; i < kMicCount; ++i) {
    Mic& mic = g_mics[size_t(i)];
    std::string wanted = MicSetting(i);
    auto found = std::find_if(devices.begin(), devices.end(), [&](auto& d) { return d.second == wanted; });
    bool present = !wanted.empty() && found != devices.end();
    if (mic.stream && (mic.device != wanted || !present)) {
      REXLOG_INFO("Microphones: mic {} closed {}", i + 1, mic.device);
      SDL_DestroyAudioStream(mic.stream);
      mic.stream = nullptr;
      mic.level = 0;
    }
    mic.device = wanted;
    if (!mic.stream && present) {
      SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};  // SDL converts the device's format
      mic.stream = SDL_OpenAudioDeviceStream(found->first, &spec, nullptr, nullptr);
      if (!mic.stream) {
        REXLOG_ERROR("Microphones: mic {} couldn't open {}: {}", i + 1, wanted, SDL_GetError());
        continue;
      }
      SDL_ResumeAudioStreamDevice(mic.stream);
      REXLOG_INFO("Microphones: mic {} is {}", i + 1, wanted);
    }
  }
}

size_t ReadLocked(Mic& mic, int16_t* out, size_t max) {
  if (!mic.stream) return 0;
  int got = SDL_GetAudioStreamData(mic.stream, out, int(max * sizeof(int16_t)));
  if (got <= 0) return 0;
  size_t n = size_t(got) / sizeof(int16_t);
  int peak = 0;
  for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(out[i])));
  mic.level = std::max(mic.level * 0.8f, float(peak) / 32768.0f);
  return n;
}

}  // namespace

std::vector<std::string> MicDeviceNames() {
  std::vector<std::string> names;
  for (auto& device : Devices()) names.push_back(device.second);
  return names;
}

std::string MicSetting(int mic) {
  switch (mic) {
    case 0: return REXCVAR_GET(mic_1);
    case 1: return REXCVAR_GET(mic_2);
    case 2: return REXCVAR_GET(mic_3);
    default: return "";
  }
}

void SetMicSetting(int mic, const std::string& device) {
  if (mic < 0 || mic >= kMicCount) return;
  rex::cvar::SetFlagByName(("mic_" + std::to_string(mic + 1)).c_str(), device);
  std::lock_guard lock(g_mutex);
  g_synced = {};  // follow it now
  SyncLocked();
}

bool MicOpen(int mic) {
  if (mic < 0 || mic >= kMicCount) return false;
  std::lock_guard lock(g_mutex);
  SyncLocked();
  return g_mics[size_t(mic)].stream != nullptr;
}

size_t ReadMic(int mic, int16_t* out, size_t max) {
  if (mic < 0 || mic >= kMicCount) return 0;
  std::lock_guard lock(g_mutex);
  SyncLocked();
  Mic& m = g_mics[size_t(mic)];
  m.game_read = Clock::now();
  return ReadLocked(m, out, max);
}

float MicLevel(int mic) {
  if (mic < 0 || mic >= kMicCount) return 0;
  std::lock_guard lock(g_mutex);
  SyncLocked();
  Mic& m = g_mics[size_t(mic)];
  if (Clock::now() - m.game_read > std::chrono::milliseconds(250)) {
    int16_t samples[2048];
    while (ReadLocked(m, samples, std::size(samples)) == std::size(samples)) {
    }
  }
  return m.stream ? m.level : 0;
}

}  // namespace rb3pc

// Synth360, one per USB port (context: +4 port, +8 stop flag, +9 connected).

#ifdef RB3PC_DX
#define MIC_PORT_THREAD sub_82B67288
#define MIC_PORT_SLOT sub_82B66D68     // port -> a free game mic slot (0 if none)
#define MIC_CONNECT sub_82B66EE8       // (slot, rate mask 256 = 48 kHz, 0, Symbol* type)
#define MIC_PUSH sub_82B66F40          // (port, big-endian int16 samples, bytes)
#define MIC_DISCONNECT sub_82B67008    // (port)
#define MIC_SYMBOL sub_827C0728        // Symbol::Symbol(Symbol*, const char*)
constexpr uint32_t kGenericUsb = 0x820FE36C;  // "generic_usb", the mic type with no synth.cfg entry
constexpr uint32_t kSlotMics = 0x82E121BC;    // slot -> MicXbox*
#else
#define MIC_PORT_THREAD sub_82B385C8
#define MIC_PORT_SLOT sub_82B380E8
#define MIC_CONNECT sub_82B38268
#define MIC_PUSH sub_82B382C0
#define MIC_DISCONNECT sub_82B38388
#define MIC_SYMBOL sub_8279B788
constexpr uint32_t kGenericUsb = 0x820FAF14;
constexpr uint32_t kSlotMics = 0x82DDFC94;
#endif
REX_EXTERN(MIC_PORT_SLOT);
REX_EXTERN(MIC_CONNECT);
REX_EXTERN(MIC_PUSH);
REX_EXTERN(MIC_DISCONNECT);
REX_EXTERN(MIC_SYMBOL);

namespace {

constexpr uint32_t kMicGain = 0x9070;  // MicXbox: the game's gain, 0-1 (float)
// The original sets the USB mic's hardware volume; here gain scales samples,
// unity at the game's default.
constexpr float kDefaultGain = 0.225f;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

float LoadFloat(const uint8_t* base, uint32_t addr) {
  uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

bool GuestPointer(uint32_t addr) { return addr >= 0x40000000 && addr < 0xA0000000; }

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

REX_HOOK_RAW(MIC_PORT_THREAD) {
  const uint32_t thread = ctx.r3.u32;
  const int port = int(Load32(base, thread + 4));
  const PPCContext saved = ctx;
  // Guest scratch below the thread's frame: the type Symbol and a buffer.
  const uint32_t sp = (saved.r1.u32 - 0x2000) & ~0xFu;
  const uint32_t symbol = sp + 0x100, buffer = sp + 0x200;
  // The game's playback ring expects 96 samples every 2 ms; PC devices burst
  // (20+ ms periods), so samples are queued and paced. Target grows on each
  // underrun; beyond target + slack the oldest are dropped.
  constexpr size_t kPacket = 96;
  constexpr size_t kMinTarget = 1440, kMaxTarget = 4800, kSlack = 2400;  // 30 ms, 100 ms, 50 ms
  auto stopping = [&] { return base[thread + 8] != 0; };
  auto call = [&](auto fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    fn(ctx, base);
    return ctx.r3.u32;
  };

  while (!stopping()) {
    if (port >= rb3pc::kMicCount || !rb3pc::MicOpen(port)) {
      SleepMs(50);
      continue;
    }
    call(MIC_SYMBOL, symbol, kGenericUsb);
    uint32_t slot = call(MIC_PORT_SLOT, uint32_t(port));
    if (!slot) {  // more microphones than the game has mics
      SleepMs(500);
      continue;
    }
    // Synth360 registers its mics just after starting these threads.
    uint32_t mics = Load32(base, kSlotMics);
    uint32_t mic = GuestPointer(mics) ? Load32(base, mics + 4 * Load32(base, slot)) : 0;
    if (!GuestPointer(mic)) {
      SleepMs(100);
      continue;
    }
    base[thread + 9] = 1;
    call(MIC_CONNECT, slot, 256, 0, symbol);
    REXLOG_INFO("Microphones: mic {} connected ({})", port + 1, rb3pc::MicSetting(port));

    int16_t samples[2048];
    while (rb3pc::ReadMic(port, samples, std::size(samples)) == std::size(samples)) {  // flush backlog
    }
    std::deque<int16_t> queue;
    size_t target = kMinTarget, burst = 0, underruns = 0, dropped = 0, clipped = 0;
    bool playing = false;
    auto next = std::chrono::steady_clock::now(), report = next;
    while (!stopping() && rb3pc::MicOpen(port)) {
      while (size_t n = rb3pc::ReadMic(port, samples, std::size(samples))) {
        burst = std::max(burst, n);
        for (size_t i = 0; i < n; ++i) clipped += std::abs(int(samples[i])) >= 32000;
        queue.insert(queue.end(), samples, samples + n);
      }
      if (queue.size() > target + kSlack) {
        dropped += queue.size() - target;
        queue.erase(queue.begin(), queue.end() - std::ptrdiff_t(target));
      }
      auto now = std::chrono::steady_clock::now();
      if (!playing) {
        if (queue.size() >= target) {
          playing = true;
          next = now;
        }
      }
      while (playing && next <= now) {
        if (queue.size() < kPacket) {
          playing = false;
          ++underruns;
          target = std::min(target * 3 / 2, kMaxTarget);
          break;
        }
        float gain = std::clamp(LoadFloat(base, mic + kMicGain), 0.0f, 1.0f) / kDefaultGain;
        uint16_t* out = reinterpret_cast<uint16_t*>(base + buffer);
        for (size_t i = 0; i < kPacket; ++i) {
          int v = std::clamp(int(std::lround(queue[i] * gain)), -32768, 32767);
          out[i] = __builtin_bswap16(uint16_t(int16_t(v)));
        }
        queue.erase(queue.begin(), queue.begin() + kPacket);
        call(MIC_PUSH, uint32_t(port), buffer, uint32_t(kPacket * 2));
        next += std::chrono::microseconds(2000);
      }
      if (now - report > std::chrono::seconds(30)) {
        report = now;
        REXLOG_INFO("Microphones: mic {}: bursts up to {} samples, queue {} of {}, {} underruns, {} dropped, "
                    "{} clipped (at the device)",
                    port + 1, burst, queue.size(), target, underruns, dropped, clipped);
        burst = 0;
      }
      SleepMs(1);
    }

    call(MIC_DISCONNECT, uint32_t(port));
    base[thread + 9] = 0;
    REXLOG_INFO("Microphones: mic {} disconnected", port + 1);
  }
  ctx = saved;
  ctx.r3.u64 = 0;
}
