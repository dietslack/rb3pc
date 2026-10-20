#include "display_size.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>

#include <rex/hook.h>

#include "display_options.h"
#include "exe.h"

REXCVAR_DEFINE_INT32(fullscreen_resolution, 0, "rb3pc",
                     "Internal resolution in fullscreen: 0 matches the display, 1-4 renders at "
                     "that multiple of 1280x720. Windowed always follows the window size")
    .range(0, 4);

REXCVAR_DEFINE_BOOL(wide_view, true, "rb3pc",
                    "Show more or less of the scene to fit the window or display's shape (4:3 "
                    "to 32:9 and beyond) instead of RB3's 16:9 picture with bars. Set at boot");

REXCVAR_DEFINE_INT32(debug_resolution_scale, 0, "rb3pc",
                     "Testing aid: render at this multiple of 1280x720 whatever the window or "
                     "display size (0 = follow them)")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

int current_scale = 1;
bool scale_live = true;
bool mode_set = false;
GuestMode current_mode;
// Rnd::YRatio's height / width for widescreen, or 0 to keep 0.5625.
double wide_view_ratio = 0;
// Narrower than 16:9: how much taller screen cameras' field of view is made
// (tan of half of it), or 0.
double wide_view_taller = 0;

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

GuestMode GuestModeFor(bool fullscreen) {
  GuestMode mode;
  uint32_t width = 0, height = 0;
  if (!REXCVAR_GET(wide_view) || !OutputPixelSize(fullscreen, width, height)) return mode;
  // A multiple of 8 wide, within what the SDK accepts (640-4095).
  uint32_t mode_width = uint32_t(std::lround(double(width) * mode.height / height / 8.0)) * 8;
  mode.width = std::clamp<uint32_t>(mode_width, 640, 4088);
  return mode;
}

GuestMode CurrentGuestMode() { return mode_set ? current_mode : GuestModeFor(false); }

std::string AspectLabel(GuestMode mode) {
  double aspect = double(mode.width) / mode.height;
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

void ApplyWideView() {
  GuestMode mode = CurrentGuestMode();
  wide_view_ratio = mode.width * 9 == mode.height * 16 ? 0.0 : double(mode.height) / mode.width;
  wide_view_taller = wide_view_ratio > 0.5625 ? wide_view_ratio / 0.5625 : 0.0;
  if (wide_view_ratio > 0) {
    REXLOG_INFO("Display: the scene is drawn {} (height / width {:.4f})", AspectLabel(mode),
                wide_view_ratio);
  }
}

int ScaleForOutput(uint32_t width, uint32_t height) {
  // The game's picture fitted into the area.
  GuestMode mode = CurrentGuestMode();
  double picture_height = std::min<double>(height, double(width) * mode.height / mode.width);
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
  current_mode = GuestModeFor(fullscreen);
  mode_set = true;
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(current_mode.width));
  rex::cvar::SetFlagByName("video_mode_height", std::to_string(current_mode.height));
  int scale = WantedResolutionScale(fullscreen);
  uint32_t width = 0, height = 0;
  OutputPixelSize(fullscreen, width, height);
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
  current_scale = scale;
  REXLOG_INFO("Display: {} {}x{}, video mode {}x{} ({}), internal resolution {}",
              fullscreen ? "fullscreen on" : "window", width, height, current_mode.width,
              current_mode.height, AspectLabel(current_mode), ScaleLabel(scale));
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
  GuestMode mode = CurrentGuestMode();
  return std::to_string(scale) + "x (" + std::to_string(mode.width * scale) + "x" +
         std::to_string(mode.height * scale) + (scale == 1 ? ", native)" : ")");
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

// Rnd::YRatio: height / width for TheRnd's aspect (Rnd +0xF8: square,
// regular, widescreen, letterbox), from the table { 1, 0.75, 0.5625, 0.5625 }.
// Screen cameras' projections use it (RndCam::UpdateLocal), so with the
// mode's own ratio the scene is drawn wider or narrower, not stretched.
#ifdef RB3PC_DX
#define DS_RND_YRATIO sub_8240EDF8
#define DS_RND_YRATIO_ORIG __imp__sub_8240EDF8
#else
#define DS_RND_YRATIO sub_823FC1C0
#define DS_RND_YRATIO_ORIG __imp__sub_823FC1C0
#endif
REX_EXTERN(DS_RND_YRATIO_ORIG);
REX_HOOK_RAW(DS_RND_YRATIO) {
  const uint8_t* aspect = base + ctx.r3.u32 + 0xF8;
  bool widescreen = aspect[0] == 0 && aspect[1] == 0 && aspect[2] == 0 && aspect[3] == 2;
  if (widescreen && rb3pc::wide_view_ratio > 0) {
    ctx.f1.f64 = rb3pc::wide_view_ratio;
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
  auto load32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  auto store32 = [](uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
  };
  uint32_t yfov_bits = load32(cam + 0x2BC);
  float yfov;
  std::memcpy(&yfov, &yfov_bits, 4);
  if (rb3pc::wide_view_taller <= 0 || load32(cam + 0x2E4) != 0 || !(yfov > 0)) {
    DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
    return;
  }
  float taller = float(2 * std::atan(std::tan(yfov * 0.5) * rb3pc::wide_view_taller));
  uint32_t taller_bits;
  std::memcpy(&taller_bits, &taller, 4);
  store32(cam + 0x2BC, taller_bits);
  DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
  store32(cam + 0x2BC, yfov_bits);
}
