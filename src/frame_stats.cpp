#include "frame_stats.h"

#include "calibration_fps.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(log_frame_spikes, false, "rb3pc",
                    "Log each frame over 20 ms, with how much of it the game waited for the GPU");

namespace rb3pc {

extern std::atomic<uint64_t> g_frame_gpu_wait_ns;

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kWindow = std::chrono::seconds(1);

std::mutex g_mutex;
std::deque<Clock::time_point> g_frames;  // presents within the last kWindow

void Trim(Clock::time_point now) {
  while (!g_frames.empty() && now - g_frames.front() > kWindow) {
    g_frames.pop_front();
  }
}

}  // namespace

rex::ui::FrameStats GuestFrameStats() {
  std::lock_guard lock(g_mutex);
  auto now = Clock::now();
  Trim(now);
  rex::ui::FrameStats stats;
  if (g_frames.size() >= 2) {
    double span_ms =
        std::chrono::duration<double, std::milli>(g_frames.back() - g_frames.front()).count();
    stats.frame_time_ms = span_ms / double(g_frames.size() - 1);
    stats.fps = stats.frame_time_ms > 0 ? 1000.0 / stats.frame_time_ms : 0;
  }
  return stats;
}

double GuestMaxFrameGapMs() {
  std::lock_guard lock(g_mutex);
  Trim(Clock::now());
  double worst = 0;
  for (size_t i = 1; i < g_frames.size(); ++i) {
    worst = std::max(worst,
                     std::chrono::duration<double, std::milli>(g_frames[i] - g_frames[i - 1]).count());
  }
  return worst;
}

}  // namespace rb3pc

// Mid-asm hook before the game's VdSwap (0x828374BC): one call per frame.
void Rb3pcOnGuestFrame() {
  rb3pc::PaceCalibrationFrame();
  std::lock_guard lock(rb3pc::g_mutex);
  auto now = rb3pc::Clock::now();
  // Long stalls get a log line, so a reported hitch can be found in the log.
  if (!rb3pc::g_frames.empty()) {
    double gap = std::chrono::duration<double, std::milli>(now - rb3pc::g_frames.back()).count();
    uint64_t gpu_wait_ns = rb3pc::g_frame_gpu_wait_ns.exchange(0);
    if (gap > 100) {
      REXLOG_WARN("Frame hitch: {:.0f} ms", gap);
    } else if (gap > 20 && REXCVAR_GET(log_frame_spikes)) {
      REXLOG_INFO("Frame spike: {:.1f} ms, {:.1f} ms of it waiting for the GPU", gap, gpu_wait_ns / 1e6);
    }
  }
  rb3pc::g_frames.push_back(now);
  rb3pc::Trim(now);
}
