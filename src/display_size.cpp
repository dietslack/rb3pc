#include "display_size.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

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
REXCVAR_DEFINE_INT32(debug_resolution_scale_y, 0, "rb3pc",
                     "Testing aid: with debug_resolution_scale, a different vertical multiple "
                     "(0 = the same), as wide windows get")
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
// The game's frame (guest video mode), chosen at boot: as wide as the view
// when that's wider than 16:9, so its effects get enough pixels across.
Shape guest_mode{1280, 720};
// guest_mode's height / width.
std::atomic<double> guest_ratio{0.5625};

SDL_Window* GameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

Shape GuestModeFor(Shape view) {
  if (!view.width || !view.height || uint64_t(view.width) * 9 <= uint64_t(view.height) * 16) {
    return {1280, 720};
  }
  // The game lays out EDRAM for 1280x720: 16 x 45 tiles of 80x16 pixels. A
  // wider frame keeps within that many tiles (more columns, fewer rows), or
  // its color and depth overlap. Its width also stays a multiple of 64 like
  // 1280's, as bloom shrinks the frame 64 times over and a fraction there
  // left a dark strip at the right edge: so 4 columns at a time. The frame
  // is stretched to the view, so the closest shape will do.
  constexpr int kTiles = 16 * 45;
  double aspect = double(view.width) / view.height;
  Shape best{1280, 720};
  double best_error = std::abs(std::log(aspect * 720 / 1280));
  for (int columns = 20; columns <= 48; columns += 4) {
    int rows = std::min(45, kTiles / columns);
    double error = std::abs(std::log(aspect * rows * 16 / (columns * 80)));
    if (error < best_error) {
      best_error = error;
      best = {uint32_t(columns * 80), uint32_t(rows * 16)};
    }
  }
  return best;
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
  std::string size = std::to_string(guest_mode.width * scale.x) + "x" +
                     std::to_string(guest_mode.height * scale.y);
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
  return {axis(picture_width, guest_mode.width), axis(picture_height, guest_mode.height)};
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
  if (int debug = REXCVAR_GET(debug_resolution_scale); debug > 0) {
    int y = REXCVAR_GET(debug_resolution_scale_y);
    return {debug, y > 0 ? y : debug};
  }
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
  guest_mode = GuestModeFor(current_shape);
  guest_ratio = double(guest_mode.height) / guest_mode.width;
  Scale scale = WantedResolutionScale(fullscreen);
  // Per-axis cvars override resolution_scale only when != 1.
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(std::min(scale.x, scale.y)));
  rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale.x));
  rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale.y));
  current_scale = scale;
  uint32_t width = 0, height = 0;
  OutputPixelSize(fullscreen, width, height);
  REXLOG_INFO("Display: {} {}x{}, scene {}, game frame {}x{}, internal resolution {}",
              fullscreen ? "fullscreen on" : "window", width, height, AspectLabel(current_shape),
              guest_mode.width, guest_mode.height, ScaleLabel(scale));
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

// Height / width the scene is drawn at now (YRatio).
double DrawnRatio() {
  double ratio = rb3pc::view_ratio.load(std::memory_order_relaxed);
  return ratio > 0 ? ratio : 0.5625;
}

// Effect buffers cover the screen at some fraction of the game frame (a
// quarter, ...), rounded to whole pixels, or stay 16:9 while the frame is.
// For a buffer this size: that fraction, or 0 if it isn't screen-shaped.
double FrameFraction(double width, double height) {
  if (!(width > 0 && height > 0)) return 0;
  rb3pc::Shape guest = rb3pc::guest_mode;
  for (rb3pc::Shape frame : {guest, rb3pc::Shape{1280, 720}}) {
    if (std::abs(height - width * frame.height / frame.width) <= 1) return width / frame.width;
  }
  return 0;
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
  static std::atomic<uint64_t> logged_size{0};
  uint64_t size = uint64_t(Load32(base + ctx.r3.u32 + 60)) << 32 | Load32(base + ctx.r3.u32 + 64);
  if (logged_size.exchange(size) != size) {  // Rnd: Width +60, Height +64
    REXLOG_INFO("Display: the game draws {}x{}", uint32_t(size >> 32), uint32_t(size));
  }
  double ratio = rb3pc::view_ratio.load(std::memory_order_relaxed);
  if (ratio > 0 && Load32(base + ctx.r3.u32 + 0xF8) == 2) {  // kWidescreen
    ctx.f1.f64 = ratio;
    return;
  }
  DS_RND_YRATIO_ORIG(ctx, base);
}

// A camera with a target texture (+0x2E4) takes its aspect from the texture
// instead of YRatio. Venue shots also render the scene at half size into a
// 640x360 texture that screen effects (smoke depth fade, depth of field) read
// back at screen positions, so such copies must match the screen camera's
// shape or the lookups drift off toward the sides. Screen-sized 16:9 targets
// count as the screen, as do ones in the game frame's shape (wide views draw
// a wide frame); others (shadow maps, in-venue screens) keep their own.
// RndTex: width +0x4C, height +0x50.
bool IsScreenCopy(const uint8_t* base, uint32_t tex) {
  if (tex < 0x40000000 || tex >= 0x50000000) return false;
  uint32_t width = Load32(base + tex + 0x4C), height = Load32(base + tex + 0x50);
  rb3pc::Shape guest = rb3pc::guest_mode;
  for (uint32_t div : {1u, 2u, 4u}) {
    if ((width == 1280 / div && height == 720 / div) ||
        (width == guest.width / div && height == guest.height / div)) {
      return true;
    }
  }
  return false;
}

// RndCam::UpdateLocal: mYFov (+0x2BC) is vertical, so narrower than 16:9
// would crop the HUD at the sides. For perspective screen cameras widen
// mYFov for the call, then restore it; a screen copy's target texture is
// hidden for the call so its ratio comes from YRatio, like the screen's.
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
  bool reshaped = taller_by > 0 || rb3pc::view_ratio.load(std::memory_order_relaxed) > 0;
  uint32_t yfov_bits = Load32(cam + 0x2BC);
  float yfov;
  std::memcpy(&yfov, &yfov_bits, 4);
  uint32_t tex = Load32(cam + 0x2E4);
  if (!reshaped || !(yfov > 0) || (tex && !IsScreenCopy(base, tex))) {
    DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
    return;
  }
  if (taller_by > 0) {
    float taller = float(2 * std::atan(std::tan(yfov * 0.5) * taller_by));
    uint32_t taller_bits;
    std::memcpy(&taller_bits, &taller, 4);
    Store32(cam + 0x2BC, taller_bits);
  }
  if (tex) Store32(cam + 0x2E4, 0);
  DS_CAM_UPDATE_LOCAL_ORIG(ctx, base);
  if (tex) Store32(cam + 0x2E4, tex);
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

// NgDOFProc's blur passes (r3 = pass 0 or 1; r4/r5 = blur texture
// width/height). Their 8 Poisson tap offsets scale with r4 along x and r5
// along y, in buffer pixels. When the drawn shape differs from the buffer's
// (a view reshaped after boot, the game frame being fixed then), its pixels
// are wider or narrower on screen than tall, so the blur spread unevenly and
// its taps showed as offset ghost copies. Scale the width so the taps cover
// the same screen distance across as down.
#ifdef RB3PC_DX
#define DS_DOF_BLUR_PASS sub_82B8BC90
#define DS_DOF_BLUR_PASS_ORIG __imp__sub_82B8BC90
#else
#define DS_DOF_BLUR_PASS sub_82B5D480
#define DS_DOF_BLUR_PASS_ORIG __imp__sub_82B5D480
#endif
REX_EXTERN(DS_DOF_BLUR_PASS_ORIG);
REX_HOOK_RAW(DS_DOF_BLUR_PASS) {
  int32_t width = int32_t(ctx.r4.u32), height = int32_t(ctx.r5.u32);
  if (double fraction = FrameFraction(width, height); fraction > 0) {
    // What the 16:9 frame's buffer would pass, for the drawn shape.
    double height_16x9 = 720 * fraction;
    ctx.r5.u64 = uint32_t(std::max(1L, std::lround(height_16x9)));
    ctx.r4.u64 = uint32_t(std::max(1L, std::lround(height_16x9 * DrawnRatio() / (0.5625 * 0.5625))));
  }
  DS_DOF_BLUR_PASS_ORIG(ctx, base);
}

// Bloom's blurs set their tap offsets as tap / texture size, in guest
// texels: the 15-tap Gaussian (r3 = horizontal, f1/f2 = width/height) and
// the directional light streaks (same f1/f2; their x offsets use 1 / f1).
// Like the depth of field taps, a buffer squeezed on screen spread them
// unevenly, so glows and haze smeared sideways. A larger width shrinks the
// x offsets back to the same screen distance as down.
#ifdef RB3PC_DX
#define DS_BLOOM_GAUSS sub_8243BF98
#define DS_BLOOM_GAUSS_ORIG __imp__sub_8243BF98
#define DS_BLOOM_STREAK sub_8243C090
#define DS_BLOOM_STREAK_ORIG __imp__sub_8243C090
#else
#define DS_BLOOM_GAUSS sub_82429598
#define DS_BLOOM_GAUSS_ORIG __imp__sub_82429598
#define DS_BLOOM_STREAK sub_82429690
#define DS_BLOOM_STREAK_ORIG __imp__sub_82429690
#endif
namespace {
void WidenBlurWidth(PPCContext& ctx) {
  double fraction = FrameFraction(ctx.f1.f64, ctx.f2.f64);
  if (fraction <= 0) return;
  // Texels as the 16:9 frame's buffer has down, and as wide on screen.
  double height_16x9 = 720 * fraction;
  ctx.f2.f64 = double(float(height_16x9));
  ctx.f1.f64 = double(float(height_16x9 / DrawnRatio()));
}
}  // namespace
REX_EXTERN(DS_BLOOM_GAUSS_ORIG);
REX_HOOK_RAW(DS_BLOOM_GAUSS) {
  WidenBlurWidth(ctx);
  DS_BLOOM_GAUSS_ORIG(ctx, base);
}
REX_EXTERN(DS_BLOOM_STREAK_ORIG);
REX_HOOK_RAW(DS_BLOOM_STREAK) {
  WidenBlurWidth(ctx);
  DS_BLOOM_STREAK_ORIG(ctx, base);
}

// Camera shot framing (config/{retail,dx}/hooks.toml): x = the projection's
// x scale, 1 / tan(half the horizontal view). Taller views keep 16:9's.
void Rb3pcShotOffsetScale(PPCRegister& x) {
  double ratio = rb3pc::view_ratio.load(std::memory_order_relaxed);
  if (ratio > 0 && ratio < 0.5625) x.f64 = double(float(x.f64 * 0.5625 / ratio));
}

// RndFlare::CalcRect (r3 = flare, r5 = out visible area): the glow's rect
// (+260 x, y, w, h, in TheRnd pixels) is its size times the guest width, so a
// wide view's squeezed frame drew flares twice as big at 32:9. Shrink it about
// its centre to the 16:9 size and redo the on-screen area, as the original
// does. TheRnd: Width +60, Height +64.
#ifdef RB3PC_DX
#define DS_FLARE_CALC_RECT sub_824768E0
#define DS_FLARE_CALC_RECT_ORIG __imp__sub_824768E0
constexpr uint32_t kTheRnd = 0x82C76B68;
#else
#define DS_FLARE_CALC_RECT sub_82463F38
#define DS_FLARE_CALC_RECT_ORIG __imp__sub_82463F38
constexpr uint32_t kTheRnd = 0x82C45BA8;
#endif
REX_EXTERN(DS_FLARE_CALC_RECT_ORIG);
REX_HOOK_RAW(DS_FLARE_CALC_RECT) {
  uint32_t flare = ctx.r3.u32, out_area = ctx.r5.u32;
  DS_FLARE_CALC_RECT_ORIG(ctx, base);
  double ratio = rb3pc::view_ratio.load(std::memory_order_relaxed);
  uint32_t rnd = Load32(base + kTheRnd);
  if (!(ratio > 0 && ratio < 0.5625) || !rnd) return;
  auto get = [&](uint32_t at) {
    uint32_t bits = Load32(base + at);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };
  auto set = [&](uint32_t at, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Store32(base + at, bits);
  };
  float scale = float(ratio / 0.5625);
  float x = get(flare + 260), y = get(flare + 264), w = get(flare + 268), h = get(flare + 272);
  float cx = x + w * 0.5f, cy = y + h * 0.5f;
  w *= scale;
  h *= scale;
  x = cx - w * 0.5f;
  y = cy - h * 0.5f;
  set(flare + 260, x);
  set(flare + 264, y);
  set(flare + 268, w);
  set(flare + 272, h);
  float width = float(int32_t(Load32(base + rnd + 60))), height = float(int32_t(Load32(base + rnd + 64)));
  float seen_w = std::min(width, x + w) - std::max(0.0f, x);
  float seen_h = std::min(height, y + h) - std::max(0.0f, y);
  set(out_area, seen_w * seen_h);
}

// DxRnd's init, before it stores the width (config/{retail,dx}/hooks.toml):
// r30 = Rnd, r28 = height (already in +64), r7 = width for Rnd, r29 = width
// for the back buffer. Wider than 16:9, the game draws a frame in the view's
// shape (guest_mode), so everything sized from it (effect buffers, screen
// copies) gets pixels across in proportion.
void Rb3pcDxRndWidth(PPCRegister& rnd, PPCRegister& width, PPCRegister& buffer_width,
                     PPCRegister& height) {
  rb3pc::Shape mode = rb3pc::guest_mode;
  if (height.u32 != 720 || width.u32 != 1280 || mode.width == 1280) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  Store32(base + rnd.u32 + 64, mode.height);
  height.u64 = mode.height;
  width.u64 = mode.width;
  buffer_width.u64 = mode.width;
}
