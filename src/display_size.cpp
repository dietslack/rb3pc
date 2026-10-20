#include "display_size.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>

#include "display_options.h"

REXCVAR_DEFINE_INT32(fullscreen_resolution, 0, "rb3pc",
                     "Internal resolution in fullscreen: 0 matches the display, 1-4 renders at "
                     "that multiple of 1280x720. Windowed always follows the window size")
    .range(0, 4);

REXCVAR_DEFINE_INT32(debug_resolution_scale, 0, "rb3pc",
                     "Testing aid: render at this multiple of 1280x720 whatever the window or "
                     "display size (0 = follow them)")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

int current_scale = 1;
bool scale_live = true;

SDL_Window* GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

}  // namespace

int FullscreenResolutionSetting() { return std::clamp(REXCVAR_GET(fullscreen_resolution), 0, 4); }

void SetFullscreenResolutionSetting(int value) {
  rex::cvar::SetFlagByName("fullscreen_resolution", std::to_string(std::clamp(value, 0, 4)));
}

int ScaleForOutput(uint32_t width, uint32_t height) {
  // The 16:9 picture fitted into the area.
  double picture_height = std::min<double>(height, width * 9.0 / 16.0);
  int scale = int(std::ceil(picture_height / 720.0 - 0.02));  // 1088 lines still counts as 1080
  return std::clamp(scale, 1, kScaleCount);
}

bool OutputPixelSize(bool fullscreen, uint32_t& width, uint32_t& height) {
  SDL_Window* window = GameWindow();
  if (!window) return false;
  if (fullscreen) {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
    if (!mode) return false;
    width = uint32_t(std::lround(mode->w * mode->pixel_density));
    height = uint32_t(std::lround(mode->h * mode->pixel_density));
    return width && height;
  }
  int w = 0, h = 0;
  if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w <= 0 || h <= 0) return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

int WantedResolutionScale(bool fullscreen) {
  if (REXCVAR_GET(debug_resolution_scale) > 0) return REXCVAR_GET(debug_resolution_scale);
  if (fullscreen && FullscreenResolutionSetting() > 0) return FullscreenResolutionSetting();
  uint32_t width = 0, height = 0;
  if (!OutputPixelSize(fullscreen, width, height)) return 1;
  return ScaleForOutput(width, height);
}

void ApplyResolutionScale(bool fullscreen) {
  int scale = WantedResolutionScale(fullscreen);
  uint32_t width = 0, height = 0;
  OutputPixelSize(fullscreen, width, height);
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
  current_scale = scale;
  REXLOG_INFO("Display: {} {}x{}, internal resolution {}", fullscreen ? "fullscreen on" : "window",
              width, height, ScaleLabel(scale));
}

void FollowResolutionScale(rex::system::IGraphicsSystem* graphics, bool fullscreen) {
  if (!graphics || !scale_live) return;
  int scale = WantedResolutionScale(fullscreen);
  if (scale == current_scale) return;
  if (!graphics->RequestDrawResolutionScale(uint32_t(scale), uint32_t(scale))) {
    REXLOG_WARN("Display: the GPU can't change resolution while running; restart to apply");
    scale_live = false;
    return;
  }
  uint32_t width = 0, height = 0;
  OutputPixelSize(fullscreen, width, height);
  REXLOG_INFO("Display: {} {}x{}, internal resolution {} (was {})",
              fullscreen ? "fullscreen on" : "window", width, height, ScaleLabel(scale),
              ScaleLabel(current_scale));
  current_scale = scale;
}

bool ResolutionScaleLive() { return scale_live; }

int CurrentResolutionScale() { return current_scale; }

std::string ScaleLabel(int scale) {
  for (int i = 0; i < kScaleCount; ++i) {
    if (kScales[i] == scale) return kScaleLabels[i];
  }
  return std::to_string(scale) + "x";
}

void FitWindowToUsableArea() {
  SDL_Window* window = GameWindow();
  if (!window || (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) return;
  SDL_Rect usable;
  if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) return;
  int top = 0, left = 0, bottom = 0, right = 0;
  if (!SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right)) {
    top = 32;  // unknown on some WMs: guess a title bar
    left = right = bottom = 0;
  }
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  int room_w = usable.w - left - right, room_h = usable.h - top - bottom;
  if (w <= room_w && h <= room_h) return;
  int fit_w = std::min(room_w, room_h * 16 / 9);
  int fit_h = fit_w * 9 / 16;
  if (fit_w <= 0 || fit_h <= 0) return;
  SDL_SetWindowSize(window, fit_w, fit_h);
  SDL_SetWindowPosition(window, usable.x + left + (room_w - fit_w) / 2,
                        usable.y + top + (room_h - fit_h) / 2);
  REXLOG_INFO("Display: window {}x{} didn't fit the usable {}x{}; now {}x{}", w, h, usable.w,
              usable.h, fit_w, fit_h);
}

}  // namespace rb3pc
