// Backs off D3D's GPU wait spins (config/retail/hooks.toml), which ran at low
// SMT priority on the 360 but burn a full host core here.

#include <sched.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gpu_wait_sleep, true, "rb3pc",
                    "Sleep instead of busy-spinning while the game waits for the GPU")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

using Clock = std::chrono::steady_clock;

// Polls closer together than this belong to the same wait.
constexpr auto kSameWait = std::chrono::microseconds(500);
// Yield before sleeping so short waits cost no latency.
constexpr auto kYieldPhase = std::chrono::microseconds(200);
constexpr auto kSleep = std::chrono::microseconds(100);

}  // namespace

namespace rb3pc {
void RecordWaitTime(const char* what, uint64_t ns);
// Time the game spent waiting for the GPU this frame (see frame_stats.cpp).
std::atomic<uint64_t> g_frame_gpu_wait_ns{0};
}

// Mid-asm hook: the poll found the GPU not done yet; the caller polls again.
void Rb3pcOnGpuWaitPoll() {
  static const bool enabled = REXCVAR_GET(gpu_wait_sleep);
  if (!enabled) return;
  thread_local Clock::time_point wait_start;
  thread_local Clock::time_point last_poll;
  auto now = Clock::now();
  if (now - last_poll > kSameWait) wait_start = now;
  if (now - wait_start < kYieldPhase) {
    sched_yield();
  } else {
    std::this_thread::sleep_for(kSleep);
  }
  last_poll = Clock::now();
  rb3pc::g_frame_gpu_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(last_poll - now).count());
  rb3pc::RecordWaitTime(
      "GpuWait", uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(last_poll - now).count()));
}
