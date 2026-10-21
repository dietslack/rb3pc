// Picture shape and resolution follow the window's drawable area (windowed)
// or the display (fullscreen), live.
// Shape: RB3 still renders 1280x720, but its cameras draw the target shape
// into it and the presenter stretches it back; wider shows more at the sides.
// Resolution: per-axis whole multiple of 1280x720, smallest at least as sharp
// as the output (max 4x): 2560x1080 -> 2x2, 3840x1080 -> 3x2.

#pragma once

#include <cstdint>
#include <string>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace rb3pc {

struct Shape {
  uint32_t width = 16, height = 9;
};
Shape ViewShape(bool fullscreen);

// aspect_ratio, for fullscreen: 0 = match the display, then fixed shapes.
struct AspectChoice {
  const char* label;
  Shape shape;  // 0 x 0: the display's
};
inline constexpr AspectChoice kAspectChoices[] = {
    {"Match the display", {0, 0}}, {"4:3", {4, 3}},   {"16:10", {16, 10}},
    {"16:9 (original)", {16, 9}}, {"21:9", {64, 27}}, {"32:9", {32, 9}}};
inline constexpr int kAspectChoiceCount = 6;
int AspectRatioSetting();
void SetAspectRatioSetting(int index);
// "16:9", "21:9", "4:3", or "2.10:1".
std::string AspectLabel(Shape shape);
// Width / height of the shape the game draws now. Any thread.
double CurrentViewAspect();

struct Scale {
  int x = 1, y = 1;
  bool operator==(const Scale& other) const { return x == other.x && y == other.y; }
  bool operator!=(const Scale& other) const { return !(*this == other); }
};
// "2x (2560x1440)", "3x2 (3840x1440)".
std::string ScaleLabel(Scale scale);
// "2X", "3X2".
std::string ScaleShortLabel(Scale scale);

// fullscreen_resolution: 0 = match the display, 1-4 = that multiple.
int FullscreenResolutionSetting();
void SetFullscreenResolutionSetting(int value);

Scale ScaleForOutput(bool fullscreen, uint32_t width, uint32_t height);

// False if there's no window yet.
bool OutputPixelSize(bool fullscreen, uint32_t& width, uint32_t& height);

Scale WantedResolutionScale(bool fullscreen);

// Before the game and GPU start.
void ApplyResolutionScale(bool fullscreen);

// On window resize, fullscreen toggle or display setting change; takes effect
// next frame. UI thread.
void FollowResolutionScale(rex::system::IGraphicsSystem* graphics, bool fullscreen);

// False if a change needs a restart.
bool ResolutionScaleLive();

Scale CurrentResolutionScale();

// Windowed: shrinks (same shape) and centres the window if it doesn't fit the
// display's usable area.
void FitWindowToUsableArea();

}  // namespace rb3pc
