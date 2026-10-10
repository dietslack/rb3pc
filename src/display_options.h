// Display options shared by the F8 window and the launcher.

#pragma once

#include <cstdlib>
#include <string>

#include <rex/cvar.h>

namespace rb3pc {

// Internal resolution: resolution_scale (SDK, needs a restart). RB3 renders at
// 1280x720, so each step multiplies that.
constexpr int kScales[] = {1, 2, 3, 4};
constexpr const char* kScaleLabels[] = {"1x (1280x720, native)", "2x (2560x1440)",
                                        "3x (3840x2160)", "4x (5120x2880)"};
constexpr int kScaleCount = 4;

// Frame-rate limit: the emulated vblank rate (video_mode_refresh_rate) the game
// paces itself on, or no limit (vsync off: vblanks every millisecond). Needs a
// restart; the vblank thread reads the rate once.
constexpr int kLimits[] = {30, 60, 120, 144, 240, 0};
constexpr const char* kLimitLabels[] = {"30", "60 (original)", "120", "144", "240",
                                        "Unlimited"};
constexpr int kLimitCount = 6;

inline int GetIntFlag(const char* name) {
  return std::atoi(rex::cvar::GetFlagByName(name).c_str());
}
inline bool GetBoolFlag(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }

inline int ScaleIndex() {
  int scale = GetIntFlag("resolution_scale");
  for (int i = 0; i < kScaleCount; ++i) {
    if (kScales[i] == scale) return i;
  }
  return 0;
}
inline void SetScaleIndex(int index) {
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(kScales[index]));
}

inline int LimitIndex() {
  int limit = GetBoolFlag("vsync") ? GetIntFlag("video_mode_refresh_rate") : 0;
  for (int i = 0; i < kLimitCount; ++i) {
    if (kLimits[i] == limit) return i;
  }
  return 1;
}
inline void SetLimitIndex(int index) {
  int value = kLimits[index];
  rex::cvar::SetFlagByName("vsync", value ? "true" : "false");
  if (value) rex::cvar::SetFlagByName("video_mode_refresh_rate", std::to_string(value));
}

}  // namespace rb3pc
