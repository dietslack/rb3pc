// Holds 60 FPS on the lag calibration screens, which assume the 360's 60 Hz.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

#include "calibration_fps.h"

REXCVAR_DEFINE_BOOL(calibration_60fps, true, "rb3pc",
                    "Lock the frame rate to 60 FPS on the lag calibration screens");

namespace rb3pc {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kFrame = std::chrono::nanoseconds(1'000'000'000 / 60);
// A calibration screen polls every frame; past this, it's gone.
constexpr auto kPollTimeout = std::chrono::milliseconds(250);

std::atomic<Clock::rep> g_last_poll{0};

}  // namespace

// Called once per game frame, just before it presents (see frame_stats.cpp).
void PaceCalibrationFrame() {
  static Clock::time_point next{};
  auto now = Clock::now();
  if (!REXCVAR_GET(calibration_60fps) ||
      now - Clock::time_point(Clock::duration(g_last_poll.load())) > kPollTimeout) {
    next = {};
    return;
  }
  if (next == Clock::time_point{} || now - next > kFrame) {
    // Don't catch up.
    next = now + kFrame;
    return;
  }
  // Sleep most of the way, then spin the last bit for an even cadence.
  if (next - now > std::chrono::milliseconds(2)) {
    std::this_thread::sleep_until(next - std::chrono::milliseconds(1));
  }
  while (Clock::now() < next) std::this_thread::yield();
  next += kFrame;
}

}  // namespace rb3pc

// Mid-asm hook in CalibrationPanel::Poll (0x825F0710), after its prologue.
void Rb3pcCalibrationPoll() {
  rb3pc::g_last_poll.store(rb3pc::Clock::now().time_since_epoch().count());
}
