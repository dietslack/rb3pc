#include "display_size.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>

#include "display_options.h"

REXCVAR_DEFINE_INT32(fullscreen_resolution, 0, "rb3pc",
                     "Internal resolution in fullscreen: 0 matches the display, 1-4 renders at "
                     "that multiple of 1280x720. Windowed always follows the window size")
    .range(0, 4);

REXCVAR_DEFINE_INT32(aspect_ratio, 0, "rb3pc",
                     "Shape the scene is drawn in fullscreen, showing more or less of it, never "
                     "stretched: 0 matches the display, 1 4:3, 2 16:10, 3 16:9 (the original), "
                     "4 21:9, 5 32:9 (bars fill the rest). Windowed always matches the window")
    .range(0, 5);

REXCVAR_DEFINE_INT32(debug_resolution_scale, 0, "rb3pc",
                     "Testing aid: render at this multiple of 1280x720 whatever the window or "
                     "display size (0 = follow them)")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

Scale current_scale;
bool scale_live = true;
Shape current_shape;

// Read by the hooks below on game threads.
// Widescreen YRatio (height / width), or 0 to keep 0.5625.
std::atomic<double> view_ratio{0};
// Narrower than 16:9: tan(half fov) scale for screen cameras, or 0.
std::atomic<double> view_taller{0};
std::atomic<uint32_t> view_generation{0};

SDL_Window* GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

void SetViewShape(Shape shape) {
  double ratio = shape.width * 9 == shape.height * 16 ? 0.0 : double(shape.height) / shape.width;
  if (ratio == view_ratio.load()) return;
  view_taller = ratio > 0.5625 ? ratio / 0.5625 : 0.0;
  view_ratio = ratio;
  ++view_generation;
}

}  // namespace

int AspectRatioSetting() {
  return std::clamp(REXCVAR_GET(aspect_ratio), 0, kAspectChoiceCount - 1);
}

void SetAspectRatioSetting(int index) {
  rex::cvar::SetFlagByName("aspect_ratio",
                           std::to_string(std::clamp(index, 0, kAspectChoiceCount - 1)));
}

Shape ViewShape(bool fullscreen) {
  Shape fixed = kAspectChoices[AspectRatioSetting()].shape;
  if (fullscreen && fixed.width) return fixed;
  uint32_t width = 0, height = 0;
  if (!OutputPixelSize(fullscreen, width, height)) return {};
  return {width, height};
}

double CurrentViewAspect() {
  double ratio = view_ratio.load(std::memory_order_relaxed);
  return ratio > 0 ? 1.0 / ratio : 16.0 / 9.0;
}

std::string AspectLabel(Shape shape) {
  double aspect = double(shape.width) / shape.height;
  static constexpr struct {
    double value;
    const char* label;
  } kKnown[] = {{5.0 / 4, "5:4"},   {4.0 / 3, "4:3"},   {3.0 / 2, "3:2"},   {16.0 / 10, "16:10"},
                {16.0 / 9, "16:9"}, {21.0 / 9, "21:9"}, {64.0 / 27, "21:9"}, {32.0 / 9, "32:9"}};
  for (const auto& known : kKnown) {
    if (std::abs(aspect / known.value - 1.0) < 0.02) return known.label;
  }
  char label[16];
  std::snprintf(label, sizeof(label), "%.2f:1", aspect);
  return label;
}

std::string ScaleShortLabel(Scale scale) {
  return scale.x == scale.y ? std::to_string(scale.x) + "X"
                            : std::to_string(scale.x) + "X" + std::to_string(scale.y);
}

std::string ScaleLabel(Scale scale) {
  std::string size = std::to_string(1280 * scale.x) + "x" + std::to_string(720 * scale.y);
  if (scale.x == scale.y) {
    return std::to_string(scale.x) + "x (" + size + (scale.x == 1 ? ", native)" : ")");
  }
  return std::to_string(scale.x) + "x" + std::to_string(scale.y) + " (" + size + ")";
}

int FullscreenResolutionSetting() { return std::clamp(REXCVAR_GET(fullscreen_resolution), 0, 4); }

void SetFullscreenResolutionSetting(int value) {
  rex::cvar::SetFlagByName("fullscreen_resolution", std::to_string(std::clamp(value, 0, 4)));
}

Scale ScaleForOutput(bool fullscreen, uint32_t width, uint32_t height) {
  // 1088 lines still count as 1080.
  auto axis = [](double size, double frame) {
    return std::clamp(int(std::ceil(size / frame - 0.02)), 1, kScaleCount);
  };
  Shape shape = fullscreen ? kAspectChoices[AspectRatioSetting()].shape : Shape{0, 0};
  double picture_width = width, picture_height = height;
  if (shape.width) {
    picture_width = std::min<double>(width, double(height) * shape.width / shape.height);
    picture_height = std::min<double>(height, double(width) * shape.height / shape.width);
  }
  return {axis(picture_width, 1280), axis(picture_height, 720)};
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

Scale WantedResolutionScale(bool fullscreen) {
  if (int debug = REXCVAR_GET(debug_resolution_scale); debug > 0) return {debug, debug};
  if (fullscreen && FullscreenResolutionSetting() > 0) {
    return {FullscreenResolutionSetting(), FullscreenResolutionSetting()};
  }
  uint32_t width = 0, height = 0;
  if (!OutputPixelSize(fullscreen, width, height)) return {};
  return ScaleForOutput(fullscreen, width, height);
}

void ApplyResolutionScale(bool fullscreen) {
  current_shape = ViewShape(fullscreen);
  SetViewShape(current_shape);
  Scale scale = WantedResolutionScale(fullscreen);
  // Per-axis cvars override resolution_scale only when != 1.
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(std::min(scale.x, scale.y)));
  rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale.x));
  rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale.y));
  current_scale = scale;
  uint32_t width = 0, height = 0;
  OutputPixelSize(fullscreen, width, height);
  REXLOG_INFO("Display: {} {}x{}, scene {}, internal resolution {}",
              fullscreen ? "fullscreen on" : "window", width, height, AspectLabel(current_shape),
              ScaleLabel(scale));
}

void FollowResolutionScale(rex::system::IGraphicsSystem* graphics, bool fullscreen) {
  if (!graphics) return;
  Shape shape = ViewShape(fullscreen);
  SetViewShape(shape);
  graphics->SetGuestDisplayAspect(shape.width, shape.height);
  if (shape.width != current_shape.width || shape.height != current_shape.height) {
    REXLOG_INFO("Display: scene {} (was {})", AspectLabel(shape), AspectLabel(current_shape));
    current_shape = shape;
  }

  if (!scale_live) return;
  Scale scale = WantedResolutionScale(fullscreen);
  if (scale == current_scale) return;
  if (!graphics->RequestDrawResolutionScale(uint32_t(scale.x), uint32_t(scale.y))) {
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

Scale CurrentResolutionScale() { return current_scale; }

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
  if (w <= 0 || h <= 0) return;
  int fit_w = std::min(room_w, int(int64_t(room_h) * w / h));
  int fit_h = int(int64_t(fit_w) * h / w);
  if (fit_w <= 0 || fit_h <= 0) return;
  SDL_SetWindowSize(window, fit_w, fit_h);
  SDL_SetWindowPosition(window, usable.x + left + (room_w - fit_w) / 2,
                        usable.y + top + (room_h - fit_h) / 2);
  REXLOG_INFO("Display: window {}x{} didn't fit the usable {}x{}; now {}x{}", w, h, usable.w,
              usable.h, fit_w, fit_h);
}

}  // namespace rb3pc

namespace {

uint32_t Load32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}

}  // namespace

// Rnd::YRatio: height / width by aspect (Rnd +0xF8: square, regular,
// widescreen, letterbox) = { 1, 0.75, 0.5625, 0.5625 }. Used by
// RndCam::UpdateLocal.
#ifdef RB3PC_DX
#define DS_RND_YRATIO sub_8240EDF8
#define DS_RND_YRATIO_ORIG __imp__sub_8240EDF8
#else
#define DS_RND_YRATIO sub_823FC1C0
#define DS_RND_YRATIO_ORIG __imp__sub_823FC1C0
#endif
REX_EXTERN(DS_RND_YRATIO_ORIG);
REX_HOOK_RAW(DS_RND_YRATIO) {
  double ratio = rb3pc::view_ratio.load(std::memory_order_relaxed);
  if (ratio > 0 && Load32(base + ctx.r3.u32 + 0xF8) == 2) {  // kWidescreen
    ctx.f1.f64 = ratio;
    return;
  }
  DS_RND_YRATIO_ORIG(ctx, base);
}

// RndCam::UpdateLocal: mYFov (+0x2BC) is vertical, so narrower than 16:9
// would crop the HUD at the sides. For perspective screen cameras (no target
// texture, +0x2E4) widen mYFov for the call, then restore it.
#ifdef RB3PC_DX
#define DS_CAM_UPDATE_LOCAL sub_82433628
#define DS_CAM_UPDATE_LOCAL_ORIG __imp__sub_82433628
#else
#define DS_CAM_UPDATE_LOCAL sub_82420ED8
#define DS_CAM_UPDATE_LOCAL_ORIG __imp__sub_82420ED8
#endif
REX_EXTERN(DS_CAM_UPDATE_LOCAL_ORIG);
REX_HOOK_RAW(DS_CAM_UPDATE_LOCAL) {
  uint8_t* cam = base + ctx.r3.u32;
  double taller_by = rb3pc::view_taller.load(std::memory_order_relaxed);
  uint32_t yfov_bits = Load32(cam + 0x2BC);
  float yfov;
  std::memcpy(&yfov, &yfov_bits, 4);
  if (taller_by <= 0 || Load32(cam + 0x2E4) != 0 || !(yfov > 0)) {
    DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
    return;
  }
  float taller = float(2 * std::atan(std::tan(yfov * 0.5) * taller_by));
  uint32_t taller_bits;
  std::memcpy(&taller_bits, &taller, 4);
  Store32(cam + 0x2BC, taller_bits);
  DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
  Store32(cam + 0x2BC, yfov_bits);
}

// RndCam::Select reprojects when the camera's aspect (+0x2E8) differs from
// TheRnd's; force a mismatch once per camera after a shape change.
#ifdef RB3PC_DX
#define DS_CAM_SELECT sub_82433D18
#define DS_CAM_SELECT_ORIG __imp__sub_82433D18
#else
#define DS_CAM_SELECT sub_82421318
#define DS_CAM_SELECT_ORIG __imp__sub_82421318
#endif
REX_EXTERN(DS_CAM_SELECT_ORIG);
REX_HOOK_RAW(DS_CAM_SELECT) {
  static std::mutex mutex;
  static std::unordered_map<uint32_t, uint32_t> camera_generation;
  uint32_t generation = rb3pc::view_generation.load(std::memory_order_relaxed);
  if (generation) {
    std::lock_guard<std::mutex> lock(mutex);
    uint32_t& seen = camera_generation[ctx.r3.u32];
    if (seen != generation) {
      seen = generation;
      Store32(base + ctx.r3.u32 + 0x2E8, 0xFFFFFFFF);
    }
  }
  DS_CAM_SELECT_ORIG(ctx, base);
}
