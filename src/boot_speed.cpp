// Boot loading speed (hooks in config/retail/hooks.toml).

#include <chrono>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(throttle_boot_logos, true, "rb3pc",
                    "Draw the boot logos at 30 FPS instead of as often as the loader polls, "
                    "so it spends that time loading");

// Splash::Poll, before it draws a logo frame: true skips this poll.
bool Rb3pcSplashPollThrottle() {
  using Clock = std::chrono::steady_clock;
  constexpr auto kFrame = std::chrono::milliseconds(33);
  static Clock::time_point last;
  if (!REXCVAR_GET(throttle_boot_logos)) return false;
  auto now = Clock::now();
  if (now - last < kFrame) return true;
  last = now;
  return false;
}
