// Mouse pointer in RB3's menus: hover moves the highlight, click selects (the
// mouse already sends A, see ApplyMouseKeyboardDefaults). rb3pc and rb3pc-dx.
//
// DrawShowing hooks record which camera (RndCam::sCurrent) drew each
// component; candidates are the focused one plus what its nav links reach.
// Hit test: pointer ray vs. each candidate's local x-z plane, then the
// text's lines or the list's spacing. CollideShowing misses text (glyph
// meshes keep no CPU-side triangles).
// Same panel: PanelDir::SetFocusComponent (vtable slot 15) with gNullStr as
// nav_type (null breaks the main hub's focus script). Otherwise d-pad presses
// queued on the mnk device, one at a time.
// Lists: UIList::SetSelected keeping first-showing, then CompleteScroll for
// UIComponentScrollMsg. The song list (MusicLibrary) is wheel-only.
// Help bar labels, review picks and scrollbar thumbs are clickable; an
// ImGui overlay (PointerOverlay) highlights them.
//
// Xbox 360 layouts (disassembly and memory dumps; the Wii decomp's differ;
// addresses are retail's):
// UIComponent: primary vtable at +0; RndTransformable at +36 (world
// transform at +36+92, a 64-byte Transform: three 16-byte rows, then the
// translation); RndPollable at +216; state byte at +0xE3 (0 normal,
// 1 focused, 2 disabled, 3 selecting, 4 selected); navigation targets at
// +0xEC (right) and +0xF8 (down); the Hmx::Object part (its mDir, the
// owning ObjectDir, at +0x1C) through the pointer at +0x30.
// PanelDir: vtable 0x8211F1FC; focused component at +0x1DC.
// UILabel: RndText* at +324. RndText: lines at +216 (vector of 120-byte
// records: style size at +4, top-left position x +0x64 / z +0x6C, width
// +0x74).
// UIList: UIListDir* at +376 (orientation +0x1E4, spacing +0x1EC);
// UIListState at +392 (shown count +4, provider +0x20 (NumData slot 10,
// IsActive slot 11), hidden items +0x24, first showing +0x30, selected
// display +0x38). UIList::SetSelected 0x827D3218; CompleteScroll
// 0x827D3FE8 on its UIListStateCallback at +336.
// AppInlineHelp (vtable 0x820BBE04): icon types +0x140, actions +0x14C
// (36-byte ActionElements, the JoypadAction first), labels +0x158.
// RndCam: world-to-projection Transform at +0x174, its inverse at +0x1B4.
// Projection: v = p * WorldProject, screen = (v.xy / v.z + 1) / 2, y down.

#include "mouse_menus.h"

#include <rex/ui/imgui_dialog.h>


#include <SDL3/SDL.h>
#include <imgui.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string_view>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/presenter.h>

#include "display_size.h"
#include "exe.h"
#include "instrument_input.h"
#include "rb3_assets.h"
#include "rb3_ui.h"

REXCVAR_DEFINE_BOOL(mouse_menu_debug, false, "rb3pc", "Log what the mouse pointer hovers in menus");
REXCVAR_DEFINE_STRING(debug_pointer_script, "", "rb3pc",
                      "Testing aid: pointer positions over time instead of the mouse, "
                      "\"58:0.15,0.5;64:0.15,0.37;66:a\" (seconds after launch: x,y in 0-1, or "
                      "a / b for a click, u / d for a wheel notch, p / r to press / release "
                      "the left button, U D L R A S K for d-pad / A / Start / Back presses)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, mnk_mode);

// Retail vs. rb3pc-dx (Deluxe TU5, found by masked-code matching; same class
// layouts). MM_x_ORIG is the original when hooked.
#ifdef RB3PC_DX
#define MM_COMPONENT_POLL sub_827FFAA8
#define MM_BANDBUTTON_DRAW sub_823446B0
#define MM_BANDLIST_DRAW sub_8233B7F8
#define MM_LABEL_DRAW sub_827F4910
#define MM_UILIST_DRAW sub_827F8140
#define MM_SET_FOCUS_COMPONENT sub_82806AB0
#define MM_LIST_SET_SELECTED sub_827F8770
#define MM_LIST_COMPLETE_SCROLL sub_827F9540
#define MM_MAX_FIRST_SHOWING sub_8280E0E0
#define MM_NUM_SHOWING sub_8280DEB8
#define MM_HELP_DRAW sub_823144D0
#define MM_SCROLLBAR_DRAW sub_82322580
#define MM_REVIEW_DRAW sub_8231EC58
#define MM_DXMESH_DRAW sub_82738E38
#define MM_CONTROLLER_TYPE sub_82525C40
#define MM_USER_FROM_PAD sub_82524AF8
#define MM_USER_FROM_SLOT sub_82682B60
#define MM_SLIDER_DRAW sub_8280A260
#define MM_COMPONENT_POLL_ORIG __imp__sub_827FFAA8
#define MM_BANDBUTTON_DRAW_ORIG __imp__sub_823446B0
#define MM_BANDLIST_DRAW_ORIG __imp__sub_8233B7F8
#define MM_LABEL_DRAW_ORIG __imp__sub_827F4910
#define MM_UILIST_DRAW_ORIG __imp__sub_827F8140
#define MM_SET_FOCUS_COMPONENT_ORIG __imp__sub_82806AB0
#define MM_LIST_SET_SELECTED_ORIG __imp__sub_827F8770
#define MM_LIST_COMPLETE_SCROLL_ORIG __imp__sub_827F9540
#define MM_MAX_FIRST_SHOWING_ORIG __imp__sub_8280E0E0
#define MM_NUM_SHOWING_ORIG __imp__sub_8280DEB8
#define MM_HELP_DRAW_ORIG __imp__sub_823144D0
#define MM_SCROLLBAR_DRAW_ORIG __imp__sub_82322580
#define MM_REVIEW_DRAW_ORIG __imp__sub_8231EC58
#define MM_DXMESH_DRAW_ORIG __imp__sub_82738E38
#define MM_CONTROLLER_TYPE_ORIG __imp__sub_82525C40
#define MM_USER_FROM_PAD_ORIG __imp__sub_82524AF8
#define MM_SLIDER_DRAW_ORIG __imp__sub_8280A260
#else
#define MM_COMPONENT_POLL sub_827DA580
#define MM_BANDBUTTON_DRAW sub_82331C40
#define MM_BANDLIST_DRAW sub_82329348
#define MM_LABEL_DRAW sub_827CF478
#define MM_UILIST_DRAW sub_827D2BE8
#define MM_SET_FOCUS_COMPONENT sub_827E1568
#define MM_LIST_SET_SELECTED sub_827D3218
#define MM_LIST_COMPLETE_SCROLL sub_827D3FE8
#define MM_MAX_FIRST_SHOWING sub_827E8C78
#define MM_NUM_SHOWING sub_827E8A50
#define MM_HELP_DRAW sub_823020F8
#define MM_SCROLLBAR_DRAW sub_823100F0
#define MM_REVIEW_DRAW sub_8230C7B0
#define MM_DXMESH_DRAW sub_82713E08
#define MM_CONTROLLER_TYPE sub_82512708
#define MM_USER_FROM_PAD sub_82511618
#define MM_USER_FROM_SLOT sub_82664DD0
#define MM_SLIDER_DRAW sub_827E4C98
#define MM_COMPONENT_POLL_ORIG __imp__sub_827DA580
#define MM_BANDBUTTON_DRAW_ORIG __imp__sub_82331C40
#define MM_BANDLIST_DRAW_ORIG __imp__sub_82329348
#define MM_LABEL_DRAW_ORIG __imp__sub_827CF478
#define MM_UILIST_DRAW_ORIG __imp__sub_827D2BE8
#define MM_SET_FOCUS_COMPONENT_ORIG __imp__sub_827E1568
#define MM_LIST_SET_SELECTED_ORIG __imp__sub_827D3218
#define MM_LIST_COMPLETE_SCROLL_ORIG __imp__sub_827D3FE8
#define MM_MAX_FIRST_SHOWING_ORIG __imp__sub_827E8C78
#define MM_NUM_SHOWING_ORIG __imp__sub_827E8A50
#define MM_HELP_DRAW_ORIG __imp__sub_823020F8
#define MM_SCROLLBAR_DRAW_ORIG __imp__sub_823100F0
#define MM_REVIEW_DRAW_ORIG __imp__sub_8230C7B0
#define MM_DXMESH_DRAW_ORIG __imp__sub_82713E08
#define MM_CONTROLLER_TYPE_ORIG __imp__sub_82512708
#define MM_USER_FROM_PAD_ORIG __imp__sub_82511618
#define MM_SLIDER_DRAW_ORIG __imp__sub_827E4C98
#endif

REX_EXTERN(MM_COMPONENT_POLL_ORIG);      // UIComponent::Poll (this = RndPollable, component + 216)
REX_EXTERN(MM_BANDBUTTON_DRAW_ORIG);     // BandButton::DrawShowing
REX_EXTERN(MM_BANDLIST_DRAW_ORIG);       // BandList::DrawShowing
REX_EXTERN(MM_LABEL_DRAW_ORIG);          // UILabel::DrawShowing (UIButton too)
REX_EXTERN(MM_UILIST_DRAW_ORIG);         // UIList::DrawShowing
REX_EXTERN(MM_SET_FOCUS_COMPONENT_ORIG); // PanelDir::SetFocusComponent(UIComponent*, Symbol)
REX_EXTERN(MM_LIST_SET_SELECTED);        // UIList::SetSelected(int pos, int first_showing)
REX_EXTERN(MM_LIST_COMPLETE_SCROLL);     // UIList::CompleteScroll(const UIListState&) (this = list + 336)
REX_EXTERN(MM_MAX_FIRST_SHOWING);        // UIListState::MaxFirstShowing() const
REX_EXTERN(MM_NUM_SHOWING);              // UIListState::NumShowing() const
REX_EXTERN(MM_HELP_DRAW_ORIG);           // AppInlineHelp::DrawShowing
REX_EXTERN(MM_SCROLLBAR_DRAW_ORIG);      // ScrollbarDisplay::DrawShowing
REX_EXTERN(MM_REVIEW_DRAW_ORIG);         // ReviewDisplay::DrawShowing
REX_EXTERN(MM_DXMESH_DRAW_ORIG);         // DxMesh::DrawShowing
REX_EXTERN(MM_CONTROLLER_TYPE);          // JoypadControllerTypePadNum(int pad) -> Symbol (r3 = result)
REX_EXTERN(MM_USER_FROM_PAD);            // JoypadGetUserFromPadNum(int pad) -> LocalUser*
REX_EXTERN(MM_USER_FROM_SLOT);           // BandUserMgr::GetUserFromSlot(int slot) -> BandUser*
REX_EXTERN(MM_SLIDER_DRAW_ORIG);         // UISlider::DrawShowing

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCurrentCam = rb3pc::exe::kDeluxe ? 0x82CC2928 : 0x82C91520;  // RndCam::sCurrent
constexpr uint32_t kCompXfm = 36 + 92;         // world Transform
constexpr uint32_t kCompState = 0xE3;
constexpr uint8_t kFocused = 1, kDisabled = 2;
constexpr uint32_t kCamWorldProject = 0x174, kCamInvWorldProject = 0x1B4;

// Pointable classes, by primary vtable.
constexpr uint32_t kBandButton = rb3pc::exe::kDeluxe ? 0x82039004 : 0x820384BC;
constexpr uint32_t kUIButton = rb3pc::exe::kDeluxe ? 0x82125794 : 0x8211FF1C;
constexpr uint32_t kBandList = rb3pc::exe::kDeluxe ? 0x82037E3C : 0x820372EC;
constexpr uint32_t kUIList = rb3pc::exe::kDeluxe ? 0x82121F3C : 0x8211C724;

constexpr uint32_t kUISlider = rb3pc::exe::kDeluxe ? 0x82124C9C : 0x8211F41C;

bool IsList(uint32_t vtable) { return vtable == kBandList || vtable == kUIList; }
bool IsPointable(uint32_t vtable) {
  return vtable == kBandButton || vtable == kUIButton || IsList(vtable) || vtable == kUISlider;
}

// UISlider: ScrollSelect at +0x140 (select-to-scroll +0x144, stored step while
// selected +0x148, else -1); current step +0x14C, step count +0x150, vertical +0x154.
constexpr uint32_t kSliderSelectToScroll = 0x144, kSliderSelectedAux = 0x148, kSliderCurrent = 0x14C,
                   kSliderSteps = 0x150, kSliderVertical = 0x154;

struct Vec3 {
  float x, y, z;
};

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
float LoadF(const uint8_t* base, uint32_t addr) {
  uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + addr, &v, 4);
}
Vec3 LoadVec(const uint8_t* base, uint32_t addr) {
  return {LoadF(base, addr), LoadF(base, addr + 4), LoadF(base, addr + 8)};
}
void StoreVec(uint8_t* base, uint32_t addr, Vec3 v) {
  StoreF(base, addr, v.x);
  StoreF(base, addr + 4, v.y);
  StoreF(base, addr + 8, v.z);
  StoreF(base, addr + 12, 0);
}

// p * xfm (row vector; the translation is the fourth row).
Vec3 Transform(const uint8_t* base, uint32_t xfm, Vec3 p) {
  Vec3 x = LoadVec(base, xfm), y = LoadVec(base, xfm + 16), z = LoadVec(base, xfm + 32),
       t = LoadVec(base, xfm + 48);
  return {p.x * x.x + p.y * y.x + p.z * z.x + t.x, p.x * x.y + p.y * y.y + p.z * z.y + t.y,
          p.x * x.z + p.y * y.z + p.z * z.z + t.z};
}

// World point to the camera's [0,1] screen space (y down) and its depth.
bool Project(const uint8_t* base, uint32_t cam, Vec3 p, float& sx, float& sy, float& depth) {
  Vec3 v = Transform(base, cam + kCamWorldProject, p);
  if (v.z <= 1e-6f) return false;
  sx = (v.x / v.z + 1) * 0.5f;
  sy = (v.y / v.z + 1) * 0.5f;
  depth = v.z;
  return true;
}

Vec3 Unproject(const uint8_t* base, uint32_t cam, float sx, float sy, float depth) {
  float nx = sx * 2 - 1, ny = sy * 2 - 1;
  return Transform(base, cam + kCamInvWorldProject, {nx * depth, ny * depth, depth});
}

Vec3 ToLocal(const uint8_t* base, uint32_t comp, Vec3 p) {
  uint32_t xfm = comp + kCompXfm;
  Vec3 a = LoadVec(base, xfm), b = LoadVec(base, xfm + 16), c = LoadVec(base, xfm + 32),
       t = LoadVec(base, xfm + 48);
  Vec3 d{p.x - t.x, p.y - t.y, p.z - t.z};
  // d = local * M with M's rows a, b, c: Cramer's rule.
  auto det3 = [](Vec3 r0, Vec3 r1, Vec3 r2) {
    return r0.x * (r1.y * r2.z - r1.z * r2.y) - r0.y * (r1.x * r2.z - r1.z * r2.x) +
           r0.z * (r1.x * r2.y - r1.y * r2.x);
  };
  float det = det3(a, b, c);
  if (std::fabs(det) < 1e-12f) return {0, 0, 0};
  return {det3(d, b, c) / det, det3(a, d, c) / det, det3(a, b, d) / det};
}

struct Drawn {
  uint32_t cam;
  Clock::time_point when;
  uint64_t order;  // draw order: later ones are on top (a dialog over a menu)
};
uint64_t g_draw_count = 0;
std::unordered_map<uint32_t, Drawn> g_drawn;  // component -> camera that last drew it
std::unordered_map<uint32_t, Drawn> g_help_bars;  // AppInlineHelp -> its last draw

void NoteDrawn(uint32_t comp, uint8_t* base) {
  if (comp) g_drawn[comp] = {Load32(base, kCurrentCam), Clock::now(), ++g_draw_count};
}

// Wheel notches over a list, applied on the game thread.
std::atomic<bool> g_pointer_on_list{false};
std::atomic<int> g_wheel_notches{0};

// Buttons a left click presses, set on the game thread for the input
// thread's click handler: -1 plain A, 0 nothing.
std::atomic<int> g_click_buttons{-1};
// Pad the click presses on (an overshell tab targets its slot's player).
std::atomic<int> g_click_player{0};

const auto g_launch = Clock::now();

std::atomic<bool> g_script_left{false};

bool ScriptedPointer(float& x, float& y) {
  struct Step {
    float t, x, y;
    char action;
  };
  static const auto steps = [] {
    std::vector<Step> out;
    std::string script = REXCVAR_GET(debug_pointer_script);
    size_t pos = 0;
    while (pos < script.size()) {
      size_t end = script.find(';', pos);
      std::string item = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      pos = end == std::string::npos ? script.size() : end + 1;
      float t, px, py;
      char action = 0;
      if (std::sscanf(item.c_str(), "%f:%f,%f", &t, &px, &py) == 3) {
        out.push_back({t, px, py, 0});
      } else if (std::sscanf(item.c_str(), "%f:%c", &t, &action) == 2) {
        out.push_back({t, 0, 0, action});
      }
    }
    return out;
  }();
  float now = std::chrono::duration<float>(Clock::now() - g_launch).count();
  static size_t actions_done = 0;
  size_t action_index = 0;
  bool any = false;
  for (const auto& step : steps) {
    if (step.action) {
      if (now >= step.t && action_index++ >= actions_done) {
        ++actions_done;
        int click = g_click_buttons;
        switch (step.action) {
          case 'u':
          case 'd':
            if (g_pointer_on_list) g_wheel_notches += step.action == 'd' ? -1 : 1;
            break;
          case 'p':
          case 'r':
            g_script_left = step.action == 'p';
            break;
          case 'b':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_B);
            break;
          case 'U':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_DPAD_UP);
            break;
          case 'D':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_DPAD_DOWN);
            break;
          case 'L':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_DPAD_LEFT);
            break;
          case 'R':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT);
            break;
          case 'A':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_A);
            break;
          case 'S':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_START);
            break;
          case 'K':
            rex::input::mnk::QueuePress(rex::input::X_INPUT_GAMEPAD_BACK);
            break;
          default:  // left click
            if (REXCVAR_GET(mouse_menu_debug)) REXLOG_INFO("Pointer: scripted click, buttons {}", click);
            if (click) {
              rb3pc::PressOnPlayer(g_click_player, click < 0 ? rex::input::X_INPUT_GAMEPAD_A : uint16_t(click));
            }
            break;
        }
      }
      continue;
    }
    if (now >= step.t) {
      x = step.x;
      y = step.y;
      any = true;
    }
  }
  return any;
}

bool LeftButtonDown() {
  if (!REXCVAR_GET(debug_pointer_script).empty()) return g_script_left;
  return SDL_GetMouseFocus() && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
}

// Where the presenter last painted the game picture (SDK patch 0025),
// letterboxing included.
struct GameArea {
  float x, y, width, height;
};
GameArea GameAreaIn(float window_width, float window_height) {
  float x, y, width, height;
  if (!rex::ui::GetLastGuestOutputRect(x, y, width, height) || width <= 0 || height <= 0) {
    return {0, 0, window_width, window_height};
  }
  return {x * window_width, y * window_height, width * window_width, height * window_height};
}

bool PointerPosition(float& x, float& y) {
  if (!REXCVAR_GET(debug_pointer_script).empty()) return ScriptedPointer(x, y);
  SDL_Window* window = SDL_GetMouseFocus();
  if (!window) return false;
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  if (w <= 0 || h <= 0) return false;
  float mx, my;
  SDL_GetMouseState(&mx, &my);
  GameArea area = GameAreaIn(float(w), float(h));
  x = (mx - area.x) / area.width;
  y = (my - area.y) / area.height;
  return x >= 0 && x <= 1 && y >= 0 && y <= 1;
}

// What the pointer was last moved onto (component, list display position).
struct Target {
  uint32_t comp = 0;
  int item = -1;
  int presses = 0;
  // The next press waits for the last one's change, or overshoots while the
  // menu animates.
  uint32_t waiting_focus = 0;
  int waiting_selected = -1;
  Clock::time_point pressed{};
};
Target g_target;
float g_last_x = -1, g_last_y = -1;
constexpr int kMaxPresses = 12;

bool ScreenPos(const uint8_t* base, uint32_t comp, float& x, float& y) {
  auto it = g_drawn.find(comp);
  if (it == g_drawn.end() || !it->second.cam) return false;
  float depth;
  return Project(base, it->second.cam, LoadVec(base, comp + kCompXfm + 48), x, y, depth);
}

// Items are `spacing` apart from the list's leading edge: along -z when
// vertical (orientation 0), +x when horizontal.
constexpr uint32_t kListDir = 376, kListState = 392;
constexpr uint32_t kDirOrientation = 0x1E4, kDirSpacing = 0x1EC;
constexpr uint32_t kStateNumDisplay = 4, kStateFirstShowing = 0x30, kStateSelectedDisplay = 0x38;

// Row placement learned from the meshes a list draws (DxMesh hook): row i's
// centre is (i + centre) spacings along. Art varies (song list 0.5, My Goals
// a fifth of a row higher), so centre is the circular mean of mesh positions.
// across_min/max bound the drawn meshes, so My Goals' category column beside
// the grid doesn't count as the list.
struct ListRows {
  double sum_cos = 0, sum_sin = 0;
  float top = -1e30f;
  float low = 1e30f, high = -1e30f;
  std::vector<float> alongs;
  std::vector<float> acrosses;
  int meshes = 0;
  float centre = 0.5f;
  float across_min = -1e30f, across_max = 1e30f;
  // Median, not mean: My Goals' rows draw one stray mesh 25 below their icons.
  float across_mid = 0;
};
std::unordered_map<uint32_t, ListRows> g_list_rows;
uint32_t g_list_drawing = 0;

int ListItemAt(const uint8_t* base, uint32_t list, Vec3 local) {
  uint32_t dir = Load32(base, list + kListDir);
  if (!dir) return -1;
  bool vertical = Load32(base, dir + kDirOrientation) == 0;
  float spacing = LoadF(base, dir + kDirSpacing);
  int num_display = int(Load32(base, list + kListState + kStateNumDisplay));
  if (!(spacing > 0) || num_display <= 0) return -1;
  auto rows = g_list_rows.find(list);
  float centre = rows != g_list_rows.end() ? rows->second.centre : 0.5f;
  if (rows != g_list_rows.end()) {
    float across = vertical ? local.x : local.z, margin = spacing * 0.25f;
    if (across < rows->second.across_min - margin || across > rows->second.across_max + margin) return -1;
  }
  int item = int(std::floor((vertical ? -local.z : local.x) / spacing - centre + 0.5f));
  return item >= 0 && item < num_display ? item : -1;
}

void NoteListMesh(const uint8_t* base, uint32_t list, uint32_t mesh, Vec3 world, float radius) {
  uint32_t dir = Load32(base, list + kListDir);
  if (!dir) return;
  float spacing = LoadF(base, dir + kDirSpacing);
  if (!(spacing > 0)) return;
  bool vertical = Load32(base, dir + kDirOrientation) == 0;
  Vec3 local = ToLocal(base, list, world);
  float along = (vertical ? -local.z : local.x) / spacing;
  ListRows& rows = g_list_rows[list];
  if (rows.alongs.size() < 256) rows.alongs.push_back(along);
  rows.top = std::max(rows.top, -along);
  float across = vertical ? local.x : local.z;
  rows.low = std::min(rows.low, across - radius);
  rows.high = std::max(rows.high, across + radius);
  if (rows.acrosses.size() < 256) rows.acrosses.push_back(across);
  ++rows.meshes;
  (void)mesh;
}

void FinishListRows(uint32_t list) {
  auto it = g_list_rows.find(list);
  if (it == g_list_rows.end()) return;
  ListRows& rows = it->second;
  rows.sum_cos = rows.sum_sin = 0;
  for (float along : rows.alongs) {
    double angle = along * 2 * 3.14159265358979;
    rows.sum_cos += std::cos(angle);
    rows.sum_sin += std::sin(angle);
  }
  if (rows.meshes >= 2 && std::hypot(rows.sum_cos, rows.sum_sin) > 0.2 * rows.meshes) {
    double phase = std::atan2(rows.sum_sin, rows.sum_cos) / (2 * 3.14159265358979);
    phase -= std::floor(phase);  // [0, 1)
    // Row 0 is at phase or phase - 1, whichever is nearer the topmost mesh.
    float first = -rows.top;
    rows.centre = std::fabs(first - float(phase)) <= std::fabs(first - float(phase - 1)) ? float(phase)
                                                                                         : float(phase - 1);
  }
  if (rows.meshes >= 2 && rows.high > rows.low) {
    rows.across_min = rows.low;
    rows.across_max = rows.high;
    if (!rows.acrosses.empty()) {
      auto mid = rows.acrosses.begin() + rows.acrosses.size() / 2;
      std::nth_element(rows.acrosses.begin(), mid, rows.acrosses.end());
      rows.across_mid = *mid;
    }
  }
  rows.alongs.clear();
  rows.acrosses.clear();
  rows.sum_cos = rows.sum_sin = 0;
  rows.top = -1e30f;
  rows.low = 1e30f;
  rows.high = -1e30f;
  rows.meshes = 0;
}

// Active menu: what the focused component's NavRight / NavDown links reach,
// either way. A hub's faded-out pages are drawn but unlinked.
constexpr uint32_t kNavRight = 0xEC, kNavDown = 0xF8;

// UIList::mParent (+484): set on the row lists of a list of lists (My Goals).
constexpr uint32_t kListParent = 484;
uint32_t ListParent(const uint8_t* base, uint32_t list) {
  if (!IsList(Load32(base, list))) return 0;
  uint32_t parent = Load32(base, list + kListParent);
  return parent && g_drawn.count(parent) && IsList(Load32(base, parent)) ? parent : 0;
}

std::vector<uint32_t> ActiveMenu(const uint8_t* base, uint32_t focused) {
  std::vector<uint32_t> menu{focused};
  for (size_t i = 0; i < menu.size(); ++i) {
    uint32_t c = menu[i];
    auto add = [&](uint32_t other) {
      if (other && g_drawn.count(other) &&
          std::find(menu.begin(), menu.end(), other) == menu.end()) {
        menu.push_back(other);
      }
    };
    add(Load32(base, c + kNavRight));
    add(Load32(base, c + kNavDown));
    add(ListParent(base, c));
    for (const auto& [other, drawn] : g_drawn) {
      if (Load32(base, other + kNavRight) == c || Load32(base, other + kNavDown) == c) add(other);
      if (ListParent(base, other) == c) add(other);
    }
  }
  return menu;
}

// Ray a-b (world) vs. the component's local x-z plane.
bool PlaneHit(const uint8_t* base, uint32_t comp, Vec3 a, Vec3 b, Vec3& local) {
  Vec3 la = ToLocal(base, comp, a), lb = ToLocal(base, comp, b);
  if (std::fabs(la.y - lb.y) < 1e-6f) return false;
  float t = la.y / (la.y - lb.y);
  if (t < 0 || t > 1) return false;
  local = {la.x + (lb.x - la.x) * t, 0, la.z + (lb.z - la.z) * t};
  return true;
}

struct LocalBox {
  float left, top, right, bottom;  // x, z (z up)
};

// Slider extent and knob travel, from the bounding spheres of the meshes it
// drew (RndDrawable: centre +0xC, radius +0x1C, mesh space; world xfm +0x80).
// Knob is the smallest; track the largest sphere holding the knob's centre.
std::unordered_map<uint32_t, LocalBox> g_slider_boxes;
struct SliderTravel {
  float from = 0, to = 0;
};
std::unordered_map<uint32_t, SliderTravel> g_slider_travel;
struct SliderMesh {
  Vec3 centre;
  float radius;
};
uint32_t g_slider_drawing = 0;
std::vector<SliderMesh> g_slider_meshes;
constexpr uint32_t kMeshSphere = 0xC, kMeshRadius = 0x1C;

bool InSlider(uint32_t slider, Vec3 local) {
  auto it = g_slider_boxes.find(slider);
  if (it == g_slider_boxes.end()) return false;
  const LocalBox& b = it->second;
  constexpr float kMargin = 4;
  return b.right > b.left && local.x >= b.left - kMargin && local.x <= b.right + kMargin &&
         local.z <= b.top + kMargin && local.z >= b.bottom - kMargin;
}

bool InButton(const uint8_t* base, uint32_t comp, Vec3 local, LocalBox* line = nullptr) {
  uint32_t text = Load32(base, comp + 324);
  if (!text) return false;
  uint32_t lines = Load32(base, text + 216), end = Load32(base, text + 220);
  for (uint32_t l = lines; l && l < end && l - lines < 120 * 16; l += 120) {
    float x = LoadF(base, l + 0x64), top = LoadF(base, l + 0x6C);
    float width = LoadF(base, l + 0x74), height = LoadF(base, l + 4) * 0.83f;
    if (local.x >= x && local.x <= x + width && local.z <= top && local.z >= top - height) {
      if (line) *line = {x, top, x + width, top - height};
      return true;
    }
  }
  return false;
}

uint32_t FocusedComponent(const uint8_t* base);
bool g_log_hover = false;

// Distance across a list from its middle, in spacings: overlapping lists (a
// grid's rows) go to the nearest.
float ListAcrossDistance(const uint8_t* base, uint32_t list, Vec3 local) {
  auto rows = g_list_rows.find(list);
  uint32_t dir = Load32(base, list + kListDir);
  if (rows == g_list_rows.end() || !dir) return 0;
  float spacing = std::max(LoadF(base, dir + kDirSpacing), 1.f);
  bool vertical = Load32(base, dir + kDirOrientation) == 0;
  return std::fabs((vertical ? local.x : local.z) - rows->second.across_mid) / spacing;
}

// Buttons beat lists.
uint32_t Hover(const uint8_t* base, float px, float py, Vec3& local) {
  uint32_t focused = FocusedComponent(base);
  if (!focused) return 0;
  uint32_t best_list = 0;
  float best_distance = 1e30f;
  Vec3 best_local{};
  for (uint32_t comp : ActiveMenu(base, focused)) {
    uint32_t vtable = Load32(base, comp), cam = g_drawn[comp].cam;
    if (!cam || !IsPointable(vtable) || base[comp + kCompState] == kDisabled) continue;
    float sx, sy, depth;
    if (!Project(base, cam, LoadVec(base, comp + kCompXfm + 48), sx, sy, depth)) continue;
    Vec3 a = Unproject(base, cam, px, py, depth * 0.25f);
    Vec3 b = Unproject(base, cam, px, py, depth * 4.0f);
    Vec3 hit;
    if (!PlaneHit(base, comp, a, b, hit)) continue;
    bool inside = IsList(vtable)          ? ListItemAt(base, comp, hit) >= 0
                  : vtable == kUISlider ? InSlider(comp, hit)
                                        : InButton(base, comp, hit);
    if (g_log_hover) {
      REXLOG_INFO("Pointer: {:08X} vt {:08X} state {} local {:.1f},{:.1f}: {}", comp, vtable,
                  base[comp + kCompState], hit.x, hit.z, inside ? "inside" : "outside");
    }
    if (!inside) continue;
    // A list of lists is pointed at through its rows' lists.
    if (IsList(vtable) && std::any_of(g_drawn.begin(), g_drawn.end(),
                                      [&](const auto& d) { return ListParent(base, d.first) == comp; })) {
      continue;
    }
    if (!IsList(vtable)) {
      local = hit;
      return comp;
    }
    float distance = ListAcrossDistance(base, comp, hit);
    if (distance < best_distance) {
      best_list = comp;
      best_distance = distance;
      best_local = hit;
    }
  }
  if (best_list) local = best_local;
  return best_list;
}

// Each panel keeps a focused component; the latest drawn (a dialog) wins.
uint32_t FocusedComponent(const uint8_t* base) {
  uint32_t best = 0;
  uint64_t best_order = 0;
  for (const auto& [comp, drawn] : g_drawn) {
    if (base[comp + kCompState] == kFocused && IsPointable(Load32(base, comp)) &&
        drawn.order >= best_order) {
      best = comp;
      best_order = drawn.order;
    }
  }
  return best;
}

void Press(uint16_t button) { rex::input::mnk::QueuePress(button); }

constexpr uint32_t kPanelDirVtable = rb3pc::exe::kDeluxe ? 0x82124A5C : 0x8211F1FC;
constexpr uint32_t kSetFocusComponentSlot = 15;  // (UIComponent*, Symbol nav_type)

// gNullStr, captured from the game's own SetFocusComponent calls.
uint32_t g_null_symbol = 0;

uint32_t PanelDirOf(const uint8_t* base, uint32_t comp) {
  uint32_t object = Load32(base, comp + 0x30);
  if (object < comp || object > comp + 0x400) return 0;  // not the usual layout
  uint32_t dir = Load32(base, object + 0x1C);
  return dir && Load32(base, dir) == kPanelDirVtable ? dir : 0;
}

// As the game's set_focus does; sends UIComponentFocusChangeMsg.
bool SetFocus(PPCContext& ctx, uint8_t* base, uint32_t dir, uint32_t comp) {
  if (!g_null_symbol) return false;
  PPCFunc* func = rex::runtime::ResolveIndirectFunction(
      Load32(base, Load32(base, dir) + 4 * kSetFocusComponentSlot));
  if (!func) return false;
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;  // below the caller's frame
  ctx.r3.u64 = dir;
  ctx.r4.u64 = comp;
  ctx.r5.u64 = g_null_symbol;
  func(ctx, base);
  ctx = saved;
  return true;
}

// In every drawn panel focused on `focused` (PanelDir +0x1DC); `dir` if none.
constexpr uint32_t kPanelFocus = 0x1DC;
bool SetFocusEverywhere(PPCContext& ctx, uint8_t* base, uint32_t focused, uint32_t target, uint32_t dir) {
  std::vector<uint32_t> dirs;
  for (const auto& [comp, drawn] : g_drawn) {
    uint32_t d = PanelDirOf(base, comp);
    if (d && Load32(base, d + kPanelFocus) == focused && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) {
      dirs.push_back(d);
    }
    // The outer panel a page sits in.
    uint32_t object = d ? Load32(base, d + 0x30) : 0;
    uint32_t outer = object > d && object < d + 0x400 ? Load32(base, object + 0x1C) : 0;
    if (outer >= 0x40000000 && outer < 0x50000000 && Load32(base, outer) == kPanelDirVtable &&
        Load32(base, outer + kPanelFocus) == focused && std::find(dirs.begin(), dirs.end(), outer) == dirs.end()) {
      dirs.push_back(outer);
    }
  }
  if (dirs.empty()) dirs.push_back(dir);
  bool any = false;
  for (uint32_t d : dirs) any = SetFocus(ctx, base, d, target) || any;
  return any;
}

void NotePress(const uint8_t* base) {
  ++g_target.presses;
  g_target.waiting_focus = FocusedComponent(base);
  g_target.waiting_selected = g_target.waiting_focus && IsList(Load32(base, g_target.waiting_focus))
                                  ? int(Load32(base, g_target.waiting_focus + kListState + kStateSelectedDisplay))
                                  : -1;
  g_target.pressed = Clock::now();
  if (REXCVAR_GET(mouse_menu_debug)) {
    REXLOG_INFO("Pointer: press {} toward {:08X}, focused {:08X}", g_target.presses, g_target.comp,
                g_target.waiting_focus);
  }
}

int MaxFirstShowing(PPCContext& ctx, uint8_t* base, uint32_t list) {
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = list + kListState;
  MM_MAX_FIRST_SHOWING(ctx, base);
  int max = int(ctx.r3.s32);
  ctx = saved;
  return std::max(max, 0);
}

// UIList::mUser (LocalUser*, +480), carried by UIComponentScrollMsg. The
// overshell forwards it only to that user's slot and its lists never set one,
// so fill in player 1 (pad 0, mnk).
constexpr uint32_t kListUser = 480;

bool IsUser(uint32_t user) { return user >= 0x40000000 && user < 0x50000000; }

uint32_t PlayerOneUser(PPCContext& ctx, uint8_t* base) {
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = 0;
  MM_USER_FROM_PAD(ctx, base);
  uint32_t user = ctx.r3.u32;
  ctx = saved;
  return IsUser(user) ? user : 0;
}

void SelectListItem(PPCContext& ctx, uint8_t* base, uint32_t list, int pos, int first) {
  if (!IsUser(Load32(base, list + kListUser))) {
    if (uint32_t user = PlayerOneUser(ctx, base)) {
      user = __builtin_bswap32(user);
      std::memcpy(base + list + kListUser, &user, 4);
    }
  }
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = list;
  ctx.r4.s64 = pos;
  ctx.r5.s64 = first;
  MM_LIST_SET_SELECTED(ctx, base);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = list + 336;  // its UIListStateCallback
  ctx.r4.u64 = list + kListState;
  MM_LIST_COMPLETE_SCROLL(ctx, base);
  ctx = saved;
}


// Lists whose highlight is fixed: circular (UIListState::mCircular, +0; DX's
// overshell modifiers) or seen scrolling on a pointer select. Hover-select
// would run away to the end, so these are wheel + click, like the song list.
std::unordered_set<uint32_t> g_pinned_lists;
bool IsPinnedList(const uint8_t* base, uint32_t list) {
  return base[list + kListState] != 0 || g_pinned_lists.count(list);
}

void ScrollList(PPCContext& ctx, uint8_t* base, uint32_t list, int notches) {
  int first = int(Load32(base, list + kListState + kStateFirstShowing));
  int display = int(Load32(base, list + kListState + kStateSelectedDisplay));
  int new_first = std::clamp(first - notches, 0, MaxFirstShowing(ctx, base, list));
  if (new_first == first) return;
  // Keep the selection on the same screen row.
  SelectListItem(ctx, base, list, new_first + display, new_first);
}

// UIListState: provider +0x20, hidden items +0x24 (vector<int>).
constexpr uint32_t kStateProvider = 0x20, kStateHidden = 0x24;
constexpr uint32_t kProviderNumDataSlot = 10, kProviderIsActiveSlot = 11;
constexpr uint32_t kMusicLibraryVtable = rb3pc::exe::kDeluxe ? 0x82090E54 : 0x8209002C;  // the song list's provider

bool IsSongList(const uint8_t* base, uint32_t list) {
  uint32_t provider = Load32(base, list + kListState + kStateProvider);
  return provider && Load32(base, provider) == kMusicLibraryVtable;
}

int NumShowing(PPCContext& ctx, uint8_t* base, uint32_t list) {
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = list + kListState;
  MM_NUM_SHOWING(ctx, base);
  int num = int(ctx.r3.s32);
  ctx = saved;
  return num;
}

// UIListProvider::IsActive (song list headers aren't). Positions are data
// indices only when nothing is hidden; otherwise assume active.
bool IsActive(PPCContext& ctx, uint8_t* base, uint32_t list, int pos) {
  uint32_t state = list + kListState;
  if (Load32(base, state + kStateHidden) != Load32(base, state + kStateHidden + 4)) return true;
  uint32_t provider = Load32(base, state + kStateProvider);
  PPCFunc* func = provider ? rex::runtime::ResolveIndirectFunction(
                                 Load32(base, Load32(base, provider) + 4 * kProviderIsActiveSlot))
                           : nullptr;
  if (!func) return true;
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = provider;
  ctx.r4.s64 = pos;
  func(ctx, base);
  bool active = (ctx.r3.u32 & 0xFF) != 0;
  ctx = saved;
  return active;
}

// One active item per notch, like the d-pad; the view follows past the middle row.
void ScrollSongList(PPCContext& ctx, uint8_t* base, uint32_t list, int notches) {
  uint32_t state = list + kListState;
  int num_display = int(Load32(base, state + kStateNumDisplay));
  int first = int(Load32(base, state + kStateFirstShowing));
  int pos = first + int(Load32(base, state + kStateSelectedDisplay));
  int count = NumShowing(ctx, base, list);
  if (num_display <= 0 || count <= 0) return;
  int max_first = std::min(MaxFirstShowing(ctx, base, list), std::max(count - num_display, 0));
  int middle = (num_display - 1) / 2;
  int step = notches > 0 ? -1 : 1;
  int new_pos = pos;
  for (int n = std::abs(notches); n > 0; --n) {
    int next = new_pos + step;
    while (next >= 0 && next < count && !IsActive(ctx, base, list, next)) next += step;
    if (next < 0 || next >= count) break;
    new_pos = next;
  }
  if (new_pos == pos) return;
  int new_first = first;
  if (step > 0 && new_pos - first > middle) new_first = new_pos - middle;
  if (step < 0 && new_pos - first < middle) new_first = new_pos - middle;
  new_first = std::clamp(new_first, std::max(new_pos - num_display + 1, 0), std::min(new_pos, max_first));
  if (REXCVAR_GET(mouse_menu_debug)) {
    REXLOG_INFO("Pointer: song list {} of {} (first {}) -> {} (first {}), max first {}", pos, count,
                first, new_pos, new_first, max_first);
  }
  SelectListItem(ctx, base, list, new_pos, new_first);
}

// AppInlineHelp: vector of 36-byte ActionElements (JoypadAction first) at
// +0x14C; matching vector<UILabel*> at +0x158.
constexpr uint32_t kHelpConfig = 0x14C, kHelpLabels = 0x158, kActionElementSize = 36;
enum JoypadAction : uint32_t {
  kActionNone = 0, kActionConfirm, kActionCancel, kActionOption, kActionStart,
  kActionViewModify, kActionUp, kActionRight, kActionDown, kActionLeft,
  kActionPageUp, kActionPageDown, kActionExtended1, kActionExtended2, kActionShellOption,
};

bool PointerOnPlane(const uint8_t* base, uint32_t comp, uint32_t cam, float px, float py, Vec3& hit) {
  float sx, sy, depth;
  if (!cam || !Project(base, cam, LoadVec(base, comp + kCompXfm + 48), sx, sy, depth)) return false;
  return PlaneHit(base, comp, Unproject(base, cam, px, py, depth * 0.25f),
                  Unproject(base, cam, px, py, depth * 4.0f), hit);
}

// Hmx::Object name (const char* at +0x18). Only heap and image addresses are
// read; others may be unmapped.
constexpr uint32_t kObjectName = 0x18;
std::string ObjectName(const uint8_t* base, uint32_t object) {
  uint32_t name = Load32(base, object + kObjectName);
  if (!((name >= 0x40000000 && name < 0x50000000) || (name >= 0x82000000 && name < 0x83000000))) return {};
  const char* c = reinterpret_cast<const char*>(base + name);
  size_t n = 0;
  while (n < 96 && c[n] >= 0x20 && c[n] < 0x7F) ++n;
  return c[n] == 0 ? std::string(c, n) : std::string();
}

// Icon font: glyphs fitted to their ink, drawn in their own colours.
constexpr const char* kIconFont = "buttons.font";

// Glyph extent, tighter than line metrics (a lone icon like NO REVIEW's X).
// RndText::mMeshMap, one mesh per font: std::map head inline at +0x160
// (leftmost +4, root +8, rightmost +0xC, size +0x10); node MeshInfo at +0x14
// (mesh, sync flags, glyph count). 4 verts per glyph (RndMesh verts at +0xD8,
// 96 bytes: pos +0, uv +0x40).
constexpr uint32_t kRndMeshVtable = rb3pc::exe::kDeluxe ? 0x82101B14 : 0x820FC3C4;  // DxMesh
constexpr uint32_t kTextMeshMap = 0x160, kMeshVerts = 0xD8, kVertSize = 96, kVertUv = 0x40;
constexpr float kIconCellShare = 0.55f;

// Ink bounds per icon cell, measured from buttons.font in the ARK: cells are
// much bigger than the icons, by varying amounts.
struct IconInk {
  float u0, v0;              // cell top left
  float ink_u0, ink_v0, ink_u1, ink_v1;
};
std::atomic<const std::vector<IconInk>*> g_icon_ink{nullptr};

void LoadIconInk(std::filesystem::path game_root) {
  std::thread([game_root] {
    rb3pc::Rb3UiAssets assets = rb3pc::LoadRb3UiAssets(
        game_root, {}, {{"buttons", "ui/resource/fonts/gen/buttons.milo_xbox", "buttons.font", "buttons.tex"}});
    auto it = assets.fonts.find("buttons");
    if (it == assets.fonts.end() || !it->second.atlas.width) return;
    const rb3pc::BitmapFont& font = it->second;
    const rb3pc::Rgba8Image& atlas = font.atlas;
    auto* inks = new std::vector<IconInk>();
    for (const auto& [ch, glyph] : font.glyphs) {
      int x0 = int(glyph.u * atlas.width), y0 = int(glyph.v * atlas.height);
      int x1 = std::min(int((glyph.u + glyph.width * font.cell_u) * atlas.width + 0.5f), int(atlas.width));
      int y1 = std::min(int((glyph.v + font.cell_v) * atlas.height + 0.5f), int(atlas.height));
      int ix0 = x1, iy0 = y1, ix1 = x0 - 1, iy1 = y0 - 1;
      for (int y = std::max(y0, 0); y < y1; ++y) {
        for (int x = std::max(x0, 0); x < x1; ++x) {
          if (atlas.pixels[(size_t(y) * atlas.width + x) * 4 + 3] < 96) continue;
          ix0 = std::min(ix0, x);
          iy0 = std::min(iy0, y);
          ix1 = std::max(ix1, x);
          iy1 = std::max(iy1, y);
        }
      }
      if (ix1 < ix0) continue;
      inks->push_back({glyph.u, glyph.v, float(ix0) / atlas.width, float(iy0) / atlas.height,
                       float(ix1 + 1) / atlas.width, float(iy1 + 1) / atlas.height});
    }
    g_icon_ink = inks;  // kept for the process's life
  }).detach();
}

const IconInk* FindIconInk(float u0, float v0) {
  const std::vector<IconInk>* inks = g_icon_ink;
  if (!inks) return nullptr;
  for (const auto& ink : *inks) {
    if (std::fabs(ink.u0 - u0) < 0.002f && std::fabs(ink.v0 - v0) < 0.002f) return &ink;
  }
  return nullptr;
}

bool GlyphBox(const uint8_t* base, uint32_t text, LocalBox& out, bool* icons_only = nullptr) {
  if (icons_only) *icons_only = true;
  uint32_t head = text + kTextMeshMap;
  uint32_t count = Load32(base, head + 0x10);
  if (!count || count > 3) return false;
  uint32_t nodes[3] = {Load32(base, head + 4), Load32(base, head + 8), Load32(base, head + 0xC)};
  uint32_t text_font = Load32(base, text + 0x110);  // RndText::mStyle.font
  bool any = false;
  for (int n = 0; n < 3; ++n) {
    uint32_t node = nodes[n];
    if (node == head || node < 0x40000000 || node >= 0x90000000 ||
        std::find(nodes, nodes + n, node) != nodes + n) {
      continue;
    }
    uint32_t mesh = Load32(base, node + 0x14), glyphs = Load32(base, node + 0x1C);
    if (mesh < 0x40000000 || mesh >= 0x90000000 || Load32(base, mesh) != kRndMeshVtable || glyphs > 256) continue;
    uint32_t verts = Load32(base, mesh + kMeshVerts);
    if (verts < 0x40000000 || verts >= 0x90000000) continue;
    uint32_t font = Load32(base, node + 0x10);
    bool icon = font != text_font || ObjectName(base, font) == kIconFont;
    for (uint32_t g = 0; g < glyphs; ++g) {
      uint32_t q = verts + g * 4 * kVertSize;
      // Verts 0 and 2 are opposite corners.
      float x0 = LoadF(base, q), z0 = LoadF(base, q + 8);
      float x1 = LoadF(base, q + 2 * kVertSize), z1 = LoadF(base, q + 2 * kVertSize + 8);
      float u0 = LoadF(base, q + kVertUv), v0 = LoadF(base, q + kVertUv + 4);
      float u1 = LoadF(base, q + 2 * kVertSize + kVertUv), v1 = LoadF(base, q + 2 * kVertSize + kVertUv + 4);
      if (u0 == u1 || v0 == v1 || x0 == x1) continue;
      if (!icon && icons_only) *icons_only = false;
      float left = std::min(x0, x1), right = std::max(x0, x1);
      float top = std::max(z0, z1), bottom = std::min(z0, z1);
      if (icon) {
        if (const IconInk* ink = FindIconInk(std::min(u0, u1), std::min(v0, v1))) {
          auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
          float l = lerp(x0, x1, (ink->ink_u0 - u0) / (u1 - u0)), r = lerp(x0, x1, (ink->ink_u1 - u0) / (u1 - u0));
          float t = lerp(z0, z1, (ink->ink_v0 - v0) / (v1 - v0)), b = lerp(z0, z1, (ink->ink_v1 - v0) / (v1 - v0));
          left = std::min(l, r);
          right = std::max(l, r);
          top = std::max(t, b);
          bottom = std::min(t, b);
        } else {
          float trim = (top - bottom) * (1 - kIconCellShare) / 2;
          top -= trim;
          bottom += trim;
        }
      }
      if (!any) {
        out = {left, top, right, bottom};
        any = true;
      } else {
        out = {std::min(out.left, left), std::max(out.top, top), std::max(out.right, right),
               std::min(out.bottom, bottom)};
      }
    }
  }
  return any;
}

// Glyph quads redrawn over a highlight: screen corners and atlas uv.
struct GlyphQuad {
  float x[4], y[4], u[4], v[4];
};
struct GlyphRun {
  std::string font;  // RndFont object name
  bool icon;         // own colours; text is drawn black
  uint32_t color;    // ImU32
  std::vector<GlyphQuad> quads;
};
void CollectGlyphRuns(const uint8_t* base, uint32_t comp, uint32_t text, uint32_t cam,
                      std::vector<GlyphRun>& runs) {
  uint32_t head = text + kTextMeshMap;
  uint32_t count = Load32(base, head + 0x10);
  if (!count || count > 3) return;
  uint32_t nodes[3] = {Load32(base, head + 4), Load32(base, head + 8), Load32(base, head + 0xC)};
  uint32_t text_font = Load32(base, text + 0x110);
  for (int n = 0; n < 3; ++n) {
    uint32_t node = nodes[n];
    if (node == head || node < 0x40000000 || node >= 0x90000000 ||
        std::find(nodes, nodes + n, node) != nodes + n) {
      continue;
    }
    uint32_t font = Load32(base, node + 0x10);
    uint32_t mesh = Load32(base, node + 0x14), glyphs = Load32(base, node + 0x1C);
    if (font < 0x40000000 || font >= 0x90000000 || mesh < 0x40000000 || mesh >= 0x90000000 ||
        Load32(base, mesh) != kRndMeshVtable || glyphs > 256) {
      continue;
    }
    uint32_t verts = Load32(base, mesh + kMeshVerts);
    if (verts < 0x40000000 || verts >= 0x90000000) continue;
    GlyphRun run{ObjectName(base, font), font != text_font, IM_COL32_WHITE, {}};
    if (run.font.empty()) continue;
    if (run.font == kIconFont) run.icon = true;
    for (uint32_t g = 0; g < glyphs; ++g) {
      uint32_t q = verts + g * 4 * kVertSize;
      GlyphQuad quad;
      bool ok = true;
      for (int k = 0; k < 4 && ok; ++k) {
        uint32_t v = q + k * kVertSize;
        float depth;
        ok = Project(base, cam, Transform(base, comp + kCompXfm, LoadVec(base, v)), quad.x[k], quad.y[k], depth);
        quad.u[k] = LoadF(base, v + kVertUv);
        quad.v[k] = LoadF(base, v + kVertUv + 4);
      }
      if (!ok || (quad.u[0] == quad.u[2] && quad.v[0] == quad.v[2])) continue;  // spaces
      if (run.quads.empty()) {
        // Vertex colour: RGBA floats at +0x30.
        auto channel = [&](uint32_t o) { return int(std::clamp(LoadF(base, q + 0x30 + o), 0.0f, 1.0f) * 255); };
        run.color = IM_COL32(channel(0), channel(4), channel(8), channel(12));
      }
      run.quads.push_back(quad);
    }
    if (!run.quads.empty()) runs.push_back(std::move(run));
  }
}

// Hmx::Object::mDir: +0x1C of the object part pointed to by +0x30.
uint32_t OwnerDir(const uint8_t* base, uint32_t comp) {
  uint32_t object = Load32(base, comp + 0x30);
  return object > comp && object < comp + 0x2000 ? Load32(base, object + 0x1C) : 0;
}

uint32_t FocusedComponent(const uint8_t* base);

// Covered panels keep drawing (DX's song list under FILTERS, help bar and
// scrollbar included). If the focused panel has a help bar, any other panel
// with one is hidden.
bool InHiddenPanel(const uint8_t* base, uint32_t comp) {
  uint32_t focused = FocusedComponent(base);
  uint32_t focus_dir = focused ? OwnerDir(base, focused) : 0;
  uint32_t dir = OwnerDir(base, comp);
  if (!focus_dir || !dir || dir == focus_dir) return false;
  auto has_help = [&](uint32_t d) {
    return std::any_of(g_help_bars.begin(), g_help_bars.end(),
                       [&](const auto& h) { return OwnerDir(base, h.first) == d; });
  };
  return has_help(focus_dir) && has_help(dir);
}

uint32_t HelpActionAt(const uint8_t* base, float px, float py, uint32_t& label_hit, LocalBox& line) {
  uint64_t best_order = 0;
  uint32_t action = kActionNone;
  for (const auto& [help, drawn] : g_help_bars) {
    if (InHiddenPanel(base, help)) continue;
    uint32_t config = Load32(base, help + kHelpConfig), config_end = Load32(base, help + kHelpConfig + 4);
    uint32_t labels = Load32(base, help + kHelpLabels), labels_end = Load32(base, help + kHelpLabels + 4);
    if (config_end - config > kActionElementSize * 16 || labels_end - labels > 4 * 16) continue;
    for (uint32_t i = 0; config + i * kActionElementSize < config_end && labels + i * 4 < labels_end; ++i) {
      uint32_t label = Load32(base, labels + i * 4);
      auto it = g_drawn.find(label);
      if (it == g_drawn.end() || it->second.order < best_order) continue;
      Vec3 hit;
      if (!PointerOnPlane(base, label, it->second.cam, px, py, hit) || !InButton(base, label, hit, &line)) continue;
      // Glyphs, not the line: a lone icon is much smaller.
      LocalBox glyphs;
      if (GlyphBox(base, Load32(base, label + 324), glyphs)) {
        float slack = (glyphs.top - glyphs.bottom) * 0.3f;
        if (hit.x < glyphs.left - slack || hit.x > glyphs.right + slack || hit.z > glyphs.top + slack ||
            hit.z < glyphs.bottom - slack) {
          continue;
        }
      }
      {
        action = Load32(base, config + i * kActionElementSize);
        label_hit = label;
        best_order = it->second.order;
      }
    }
  }
  return action;
}

// ScrollbarDisplay (vtable 0x82031F1C): BandList +328, track height +332, min
// thumb +348, top bone (RndMesh, world pos +176) +352, thumb position 0-1
// +388, thumb share of track +392. Track runs -z from the top bone. The bone
// isn't always parented to the bar (wide view moves it), so take its world
// position into the bar's plane.
constexpr uint32_t kBarList = 328, kBarHeight = 332, kBarMinThumb = 348, kBarTopBone = 352;
constexpr uint32_t kBarPosition = 388, kBarScale = 392, kBoneWorldPos = kCompXfm + 48;
// Drawn thumb, measured on screen.
constexpr float kBarCentre = 0.35f, kBarHalfWidth = 3.05f, kBarBottomExtra = 1, kBarGrabHalfWidth = 14;

std::unordered_map<uint32_t, Drawn> g_scrollbars;

struct Scrollbar {
  uint32_t bar = 0, list = 0, cam = 0;
  float x = 0, top = 0, height = 0;  // track
  float thumb_top = 0, thumb_height = 0;
};

bool ReadScrollbar(const uint8_t* base, uint32_t bar, uint32_t cam, Scrollbar& out) {
  uint32_t bone = Load32(base, bar + kBarTopBone), list = Load32(base, bar + kBarList);
  float scale = LoadF(base, bar + kBarScale);
  if (!bone || !list || !g_drawn.count(list) || !(scale < 1.0f)) return false;  // no thumb shown
  out.bar = bar;
  out.list = list;
  out.cam = cam;
  Vec3 top = ToLocal(base, bar, LoadVec(base, bone + kBoneWorldPos));
  out.x = top.x;
  out.top = top.z;
  out.height = LoadF(base, bar + kBarHeight);
  out.thumb_height = std::max(scale * out.height, LoadF(base, bar + kBarMinThumb));
  out.thumb_top = out.top - LoadF(base, bar + kBarPosition) * (out.height - out.thumb_height);
  return out.height > 0;
}

bool ScrollbarAt(const uint8_t* base, float px, float py, Scrollbar& bar, float& z) {
  for (const auto& [b, drawn] : g_scrollbars) {
    if (InHiddenPanel(base, b)) {
      if (g_log_hover) REXLOG_INFO("Pointer: scrollbar {:08X} is in a panel that isn't up", b);
      continue;
    }
    Scrollbar sb;
    Vec3 hit;
    if (!ReadScrollbar(base, b, drawn.cam, sb) || !PointerOnPlane(base, b, sb.cam, px, py, hit)) continue;
    if (std::fabs(hit.x - sb.x) <= kBarGrabHalfWidth && hit.z <= sb.top + 4 &&
        hit.z >= sb.top - sb.height - 4) {
      bar = sb;
      z = hit.z;
      return true;
    }
  }
  return false;
}

// grab: thumb top relative to the pointer.
struct Drag {
  uint32_t bar = 0;
  float grab = 0;
};
Drag g_drag;
bool g_left_was_down = false;

void DragScrollbar(PPCContext& ctx, uint8_t* base, const Scrollbar& sb, float thumb_top) {
  uint32_t list = sb.list, state = list + kListState;
  int num_display = int(Load32(base, state + kStateNumDisplay));
  int count = NumShowing(ctx, base, list);
  int max_first = std::min(MaxFirstShowing(ctx, base, list), std::max(count - num_display, 0));
  float range = sb.height - sb.thumb_height;
  if (range <= 0 || max_first <= 0) return;
  float fraction = std::clamp((sb.top - thumb_top) / range, 0.0f, 1.0f);
  int first = int(Load32(base, state + kStateFirstShowing));
  int new_first = int(std::lround(fraction * max_first));
  if (new_first == first) return;
  int display = int(Load32(base, state + kStateSelectedDisplay));
  SelectListItem(ctx, base, list, std::min(new_first + display, count - 1), new_first);
}

// ReviewDisplay (vtable 0x820313C4): score at +328 (0-5; anim frame score *
// 10). The game steps the score with X (ShellOption), wrapping 5 to none, so
// a click queues that many presses. Pick centres measured on screen.
constexpr uint32_t kReviewScore = 328;
constexpr float kReviewFirstX = -90, kReviewSpacing = 45, kReviewZ = 2.6f, kReviewHalfHeight = 17;

std::unordered_map<uint32_t, Drawn> g_reviews;
std::atomic<uint32_t> g_review_preview_comp{0};
std::atomic<int> g_review_preview_score{-1};

bool ReviewPickAt(const uint8_t* base, float px, float py, uint32_t& review, int& score, Vec3* local = nullptr) {
  for (const auto& [comp, drawn] : g_reviews) {
    Vec3 hit;
    if (!PointerOnPlane(base, comp, drawn.cam, px, py, hit)) continue;
    if (local) *local = hit;
    if (std::fabs(hit.z - kReviewZ) > kReviewHalfHeight) continue;
    float slot = (hit.x - kReviewFirstX) / kReviewSpacing;
    int pick = int(std::lround(slot));
    if (pick < 0 || pick > 4 || std::fabs(hit.x - (kReviewFirstX + pick * kReviewSpacing)) > kReviewSpacing / 2) continue;
    review = comp;
    score = pick + 1;
    return true;
  }
  return false;
}

// Overlay highlights, [0,1] screen rects set on the game thread.
enum class HighlightStyle { kButton, kIcon, kThumb, kThumbHeld };
struct Highlight {
  float x0, y0, x1, y1;
  HighlightStyle style;
  std::vector<GlyphRun> runs;
};
std::mutex g_highlight_mutex;
std::vector<Highlight> g_highlights;
Clock::time_point g_highlights_time{};

bool ScreenRect(const uint8_t* base, uint32_t comp, uint32_t cam, LocalBox box, HighlightStyle style,
                Highlight& out) {
  out = {2, 2, -1, -1, style, {}};
  for (Vec3 corner : {Vec3{box.left, 0, box.top}, Vec3{box.right, 0, box.top},
                      Vec3{box.left, 0, box.bottom}, Vec3{box.right, 0, box.bottom}}) {
    float sx, sy, depth;
    if (!Project(base, cam, Transform(base, comp + kCompXfm, corner), sx, sy, depth)) return false;
    out.x0 = std::min(out.x0, sx);
    out.y0 = std::min(out.y0, sy);
    out.x1 = std::max(out.x1, sx);
    out.y1 = std::max(out.y1, sy);
  }
  return true;
}

// Overshell tabs: OvershellDir labels (one dir per slot) in the bottom strip
// while closed. A click presses Start for that slot's player
// (BandUserMgr::GetUserFromSlot matched with JoypadGetUserFromPadNum); an empty
// slot's JOIN tab goes to player 1 (mnk).
constexpr uint32_t kOvershellDirVtable = rb3pc::exe::kDeluxe ? 0x8202DBFC : 0x8202CFC4;
constexpr float kOvershellStrip = 0.86f;  // the closed tab is below this
constexpr uint32_t kBandUserMgr = rb3pc::exe::kDeluxe ? 0x82E023B8 : 0x82DD0C58;  // TheBandUserMgr
constexpr uint32_t kBandUserToLocalUser = 0x94;  // LocalBandUser: its LocalUser past its BandUser

bool OvershellOpen(const uint8_t* base) {
  uint32_t focused = FocusedComponent(base);
  uint32_t object = focused ? Load32(base, focused + 0x30) : 0;
  if (object <= focused || object >= focused + 0x400) return false;
  uint32_t dir = Load32(base, object + 0x1C);
  return dir >= 0x40000000 && dir < 0x50000000 && Load32(base, dir) == kOvershellDirVtable;
}

// -1 if the slot is empty.
int SlotPad(PPCContext& ctx, uint8_t* base, int slot) {
  uint32_t mgr = Load32(base, kBandUserMgr);
  if (mgr < 0x40000000 || mgr >= 0x50000000) return -1;
  PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x200) & ~0xFu;
  uint32_t sp = ctx.r1.u32;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = uint32_t(slot);
  MM_USER_FROM_SLOT(ctx, base);
  uint32_t user = ctx.r3.u32;
  int pad = -1;
  for (int p = 0; user && p < 4 && pad < 0; ++p) {
    ctx.r1.u64 = sp;
    ctx.r3.u64 = uint32_t(p);
    MM_USER_FROM_PAD(ctx, base);
    if (ctx.r3.u32 && ctx.r3.u32 == user + kBandUserToLocalUser) pad = p;
  }
  ctx = saved;
  return pad;
}

bool OvershellTabAt(PPCContext& ctx, uint8_t* base, float px, float py, Highlight& highlight, int& player) {
  struct Label {
    uint32_t comp, dir, cam;
    Highlight rect;
  };
  std::vector<Label> labels;
  for (const auto& [comp, drawn] : g_drawn) {
    uint32_t object = Load32(base, comp + 0x30);
    if (object < comp || object > comp + 0x400) continue;
    uint32_t dir = Load32(base, object + 0x1C);
    if (dir < 0x40000000 || dir >= 0x50000000 || Load32(base, dir) != kOvershellDirVtable) continue;
    uint32_t text = Load32(base, comp + 324);
    LocalBox box;
    Highlight rect;
    if (text < 0x40000000 || text >= 0x50000000 || !GlyphBox(base, text, box) ||
        !ScreenRect(base, comp, drawn.cam, box, HighlightStyle::kButton, rect) || rect.y0 < kOvershellStrip) {
      continue;
    }
    labels.push_back({comp, dir, drawn.cam, std::move(rect)});
  }
  if (labels.empty()) return false;
  struct Tab {
    uint32_t dir;
    float x0 = 2, y0 = 2, x1 = -1, y1 = -1;
  };
  std::vector<Tab> tabs;
  for (const auto& l : labels) {
    auto tab = std::find_if(tabs.begin(), tabs.end(), [&](const Tab& t) { return t.dir == l.dir; });
    if (tab == tabs.end()) tab = tabs.insert(tabs.end(), Tab{l.dir});
    tab->x0 = std::min(tab->x0, l.rect.x0), tab->y0 = std::min(tab->y0, l.rect.y0);
    tab->x1 = std::max(tab->x1, l.rect.x1), tab->y1 = std::max(tab->y1, l.rect.y1);
  }
  std::sort(tabs.begin(), tabs.end(), [](const Tab& a, const Tab& b) { return a.x0 < b.x0; });
  int slot = -1;
  for (int i = 0; i < int(tabs.size()) && slot < 0; ++i) {
    const Tab& t = tabs[size_t(i)];
    float pad = (t.y1 - t.y0) * 0.15f;
    if (px >= t.x0 - pad && px <= t.x1 + pad && py >= t.y0 - pad && py <= t.y1 + pad) slot = i;
  }
  if (slot < 0) return false;
  const Tab& tab = tabs[size_t(slot)];
  // Not while this overshell is open.
  uint32_t focused = FocusedComponent(base);
  uint32_t object = focused ? Load32(base, focused + 0x30) : 0;
  if (object > focused && object < focused + 0x400 && Load32(base, object + 0x1C) == tab.dir) return false;
  player = SlotPad(ctx, base, slot);
  if (player < 0) {
    // Leftmost empty slot joins mnk, if not already in one.
    for (int other = 0; other < int(tabs.size()); ++other) {
      if (other != slot && SlotPad(ctx, base, other) == 0) return false;
    }
    for (int left = 0; left < slot; ++left) {
      if (SlotPad(ctx, base, left) < 0) return false;
    }
    player = 0;
  }
  // Highlight the MENU row: lower half, right of the first quarter.
  highlight = {2, 2, -1, -1, HighlightStyle::kButton, {}};
  for (const auto& l : labels) {
    if (l.dir != tab.dir || (l.rect.y0 + l.rect.y1) * 0.5f < (tab.y0 + tab.y1) * 0.5f ||
        (l.rect.x0 + l.rect.x1) * 0.5f < tab.x0 + (tab.x1 - tab.x0) * 0.25f) {
      continue;
    }
    highlight.x0 = std::min(highlight.x0, l.rect.x0), highlight.y0 = std::min(highlight.y0, l.rect.y0);
    highlight.x1 = std::max(highlight.x1, l.rect.x1), highlight.y1 = std::max(highlight.y1, l.rect.y1);
    CollectGlyphRuns(base, l.comp, Load32(base, l.comp + 324), l.cam, highlight.runs);
  }
  return true;
}

// Per JoypadControllerTypePadNum. Instruments page down with LB, no page up.
bool PadIsGamepad(PPCContext& ctx, uint8_t* base) {
  PPCContext saved = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r1.u64 = sp;
  ctx.r3.u64 = sp + 0x80;  // returned Symbol
  ctx.r4.u64 = 0;
  MM_CONTROLLER_TYPE(ctx, base);
  uint32_t symbol = Load32(base, sp + 0x80);
  ctx = saved;
  if (!symbol) return true;
  std::string type(reinterpret_cast<const char*>(base + symbol));
  return type == "analog" || type == "digital" || type == "dualshock" || type == "stagekit_xbox" ||
         type == "none";
}

uint16_t ActionButton(uint32_t action, bool gamepad) {
  using namespace rex::input;
  switch (action) {
    case kActionConfirm: return X_INPUT_GAMEPAD_A;
    case kActionCancel: return X_INPUT_GAMEPAD_B;
    case kActionOption: return X_INPUT_GAMEPAD_Y;
    case kActionShellOption: return X_INPUT_GAMEPAD_X;
    case kActionStart: return X_INPUT_GAMEPAD_START;
    case kActionViewModify: return X_INPUT_GAMEPAD_BACK;
    case kActionPageDown: return gamepad ? X_INPUT_GAMEPAD_RIGHT_SHOULDER : X_INPUT_GAMEPAD_LEFT_SHOULDER;
    case kActionPageUp: return gamepad ? X_INPUT_GAMEPAD_LEFT_SHOULDER : 0;
    case kActionUp: return X_INPUT_GAMEPAD_DPAD_UP;
    case kActionDown: return X_INPUT_GAMEPAD_DPAD_DOWN;
    case kActionLeft: return X_INPUT_GAMEPAD_DPAD_LEFT;
    case kActionRight: return X_INPUT_GAMEPAD_DPAD_RIGHT;
    default: return 0;
  }
}


// Driven by d-pad presses so the game's scroll messages and scripts run;
// select-to-scroll sliders get A before and after.
struct SliderDrag {
  uint32_t slider = 0;
  bool entered = false;
  int expected = -1;     // step the queued presses should reach
  Clock::time_point last_press;
};
SliderDrag g_slider_drag;

bool SliderCanScroll(const uint8_t* base, uint32_t slider) {
  return !base[slider + kSliderSelectToScroll] || int32_t(Load32(base, slider + kSliderSelectedAux)) != -1;
}

int SliderStepAt(const uint8_t* base, uint32_t slider, Vec3 local) {
  const SliderTravel& travel = g_slider_travel[slider];
  int steps = std::max(int(Load32(base, slider + kSliderSteps)), 1);
  bool vertical = base[slider + kSliderVertical];
  float t = ((vertical ? local.z : local.x) - travel.from) / std::max(travel.to - travel.from, 1e-3f);
  return int(std::lround(std::clamp(t, 0.f, 1.f) * float(steps - 1)));
}

// At most four presses: the device's queue depth.
void StepSlider(const uint8_t* base, uint32_t slider, int target) {
  using namespace rex::input;
  if (mnk::PressesPending() || !SliderCanScroll(base, slider)) return;
  int current = int(Load32(base, slider + kSliderCurrent));
  auto now = Clock::now();
  if (g_slider_drag.expected >= 0 && current != g_slider_drag.expected &&
      now - g_slider_drag.last_press < std::chrono::milliseconds(300)) {
    return;
  }
  if (current == target) return;
  bool vertical = base[slider + kSliderVertical], up = target > current;
  int count = std::min(std::abs(target - current), 4);
  for (int n = 0; n < count; ++n) {
    Press(vertical ? (up ? X_INPUT_GAMEPAD_DPAD_UP : X_INPUT_GAMEPAD_DPAD_DOWN)
                   : (up ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT));
  }
  g_slider_drag.expected = current + (up ? count : -count);
  g_slider_drag.last_press = now;
}

void UpdatePointer(PPCContext& ctx, uint8_t* base) {
  static bool wheel_hooked = [] {
    rex::input::mnk::SetWheelHandler([](int32_t notches) {
      if (!g_pointer_on_list) return false;
      g_wheel_notches += notches;
      return true;
    });
    rex::input::mnk::SetClickHandler([](int mouse_button, uint16_t& buttons) {
      int click = g_click_buttons;
      if (mouse_button != 0 || click < 0) return false;
      if (int player = g_click_player; player > 0) {
        if (click) rb3pc::PressOnPlayer(player, uint16_t(click));
        buttons = 0;
        return true;
      }
      buttons = uint16_t(click);
      return true;
    });
    return true;
  }();
  (void)wheel_hooked;
  // Drop stale components: their memory may be freed.
  auto now = Clock::now();
  for (auto it = g_drawn.begin(); it != g_drawn.end();) {
    it = now - it->second.when > std::chrono::milliseconds(200) ? g_drawn.erase(it) : std::next(it);
  }
  for (auto* drawn : {&g_help_bars, &g_scrollbars, &g_reviews}) {
    for (auto it = drawn->begin(); it != drawn->end();) {
      it = now - it->second.when > std::chrono::milliseconds(200) ? drawn->erase(it) : std::next(it);
    }
  }
  for (auto it = g_slider_boxes.begin(); it != g_slider_boxes.end();) {
    it = g_drawn.count(it->first) ? std::next(it) : g_slider_boxes.erase(it);
  }
  for (auto it = g_slider_travel.begin(); it != g_slider_travel.end();) {
    it = g_drawn.count(it->first) ? std::next(it) : g_slider_travel.erase(it);
  }
  for (auto it = g_list_rows.begin(); it != g_list_rows.end();) {
    it = g_drawn.count(it->first) ? std::next(it) : g_list_rows.erase(it);
  }
  for (auto it = g_pinned_lists.begin(); it != g_pinned_lists.end();) {
    it = g_drawn.count(*it) ? std::next(it) : g_pinned_lists.erase(it);
  }
  // XAM dialogs (keyboard, message box) own the input.
  if (!REXCVAR_GET(mnk_mode) || rex::kernel::xam::xeXamIsUIActive()) {
    g_pointer_on_list = false;
    g_click_buttons = -1;
    g_slider_drag = {};
    return;
  }
  if (REXCVAR_GET(mouse_menu_debug)) {
    static std::unordered_map<uint32_t, int> logged;
    for (const auto& [c, b] : g_slider_boxes) {
      if (logged[c]++ % 300) continue;
      const SliderTravel& t = g_slider_travel[c];
      REXLOG_INFO("Pointer: slider {:08X} box {:.1f},{:.1f}-{:.1f},{:.1f} knob {:.1f}-{:.1f} step {}/{}{}", c, b.left,
                  b.top, b.right, b.bottom, t.from, t.to, int(Load32(base, c + kSliderCurrent)),
                  int(Load32(base, c + kSliderSteps)), SliderCanScroll(base, c) ? "" : " (select to scroll)");
    }
  }
  float px, py;
  if (!PointerPosition(px, py)) {
    g_review_preview_comp = 0;
    g_review_preview_score = -1;
    g_pointer_on_list = false;
    g_click_buttons = -1;
    g_drag = {};
    g_slider_drag = {};
    std::lock_guard lock(g_highlight_mutex);
    g_highlights.clear();
    return;
  }
  bool moved = std::fabs(px - g_last_x) > 0.002f || std::fabs(py - g_last_y) > 0.002f;
  g_last_x = px;
  g_last_y = py;
  bool debug = moved && REXCVAR_GET(mouse_menu_debug);
  g_log_hover = debug;
  if (debug) REXLOG_INFO("Pointer: moved to {:.3f},{:.3f}, {} drawn", px, py, g_drawn.size());
  // Every frame: the screen changes under a still pointer too.
  Vec3 local{};
  uint32_t comp = Hover(base, px, py, local);
  bool on_list = comp && IsList(Load32(base, comp));
  int item = on_list ? ListItemAt(base, comp, local) : -1;
  g_pointer_on_list = on_list;
  int click = -1;
  int player = 0;
  std::vector<Highlight> highlights;
  uint32_t label = 0;
  LocalBox line{};
  Scrollbar bar;
  float bar_z = 0;
  bool left = LeftButtonDown(), pressed = left && !g_left_was_down;
  g_review_preview_comp = 0;
  g_review_preview_score = -1;
  if (debug) {
    uint32_t review = 0;
    int score = 0;
    Vec3 local{};
    bool on = ReviewPickAt(base, px, py, review, score, &local);
    if (review || !g_reviews.empty()) {
      REXLOG_INFO("Pointer: review display local {:.1f},{:.1f} pick {}", local.x, local.z, on ? score : 0);
    }
  }
  g_left_was_down = left;
  // A held drag outlives frames the bar isn't drawn in (a jump to a song whose
  // art loads slowly stalls the menu past the stale cutoff); it acts again once
  // the bar is drawn.
  if (g_drag.bar && !left) g_drag = {};
  if (g_slider_drag.slider && (!left || !g_drawn.count(g_slider_drag.slider))) {
    if (g_slider_drag.entered && g_drawn.count(g_slider_drag.slider)) Press(rex::input::X_INPUT_GAMEPAD_A);
    g_slider_drag = {};
  }
  if (g_slider_drag.slider) {
    uint32_t slider = g_slider_drag.slider;
    Vec3 hit;
    if (PointerOnPlane(base, slider, g_drawn[slider].cam, px, py, hit)) StepSlider(base, slider, SliderStepAt(base, slider, hit));
    click = 0;
    comp = 0;
  } else if (g_drag.bar) {
    Vec3 hit;
    Scrollbar sb;
    auto drawn = g_scrollbars.find(g_drag.bar);
    if (drawn != g_scrollbars.end() && ReadScrollbar(base, g_drag.bar, drawn->second.cam, sb)) {
      if (PointerOnPlane(base, sb.bar, sb.cam, px, py, hit)) DragScrollbar(ctx, base, sb, hit.z + g_drag.grab);
      Highlight h;
      if (ScreenRect(base, sb.bar, sb.cam,
                     {sb.x + kBarCentre - kBarHalfWidth, sb.thumb_top, sb.x + kBarCentre + kBarHalfWidth,
                      sb.thumb_top - sb.thumb_height - kBarBottomExtra},
                     HighlightStyle::kThumbHeld, h)) {
        highlights.push_back(h);
      }
    }
    click = 0;
    comp = 0;
  } else if (ScrollbarAt(base, px, py, bar, bar_z)) {
    // A press on the track centres the thumb there first.
    bool on_thumb = bar_z <= bar.thumb_top && bar_z >= bar.thumb_top - bar.thumb_height;
    if (pressed) {
      g_drag = {bar.bar, on_thumb ? bar.thumb_top - bar_z : bar.thumb_height / 2};
      if (!on_thumb) DragScrollbar(ctx, base, bar, bar_z + g_drag.grab);
      if (REXCVAR_GET(mouse_menu_debug)) REXLOG_INFO("Pointer: scrollbar {:08X} grabbed at {:.1f}", bar.bar, bar_z);
    }
    Highlight h;
    if (ScreenRect(base, bar.bar, bar.cam,
                   {bar.x + kBarCentre - kBarHalfWidth, bar.thumb_top, bar.x + kBarCentre + kBarHalfWidth,
                    bar.thumb_top - bar.thumb_height - kBarBottomExtra},
                   on_thumb || pressed ? HighlightStyle::kThumbHeld : HighlightStyle::kThumb, h)) {
      highlights.push_back(h);
    }
    click = 0;
    g_pointer_on_list = true;
    comp = 0;
  } else if (uint32_t review = 0; [&] {
               int score = 0;
               if (!ReviewPickAt(base, px, py, review, score)) return false;
               int current = int(Load32(base, review + kReviewScore));
               g_review_preview_comp = review;
               g_review_preview_score = score;
               if (pressed && score != current) {
                 for (int n = (score - current + 6) % 6; n > 0; --n) Press(rex::input::X_INPUT_GAMEPAD_X);
                 if (REXCVAR_GET(mouse_menu_debug)) REXLOG_INFO("Pointer: review {} -> {}", current, score);
               }
               return true;
             }()) {
    click = 0;
    comp = 0;
  } else if (Highlight tab; OvershellTabAt(ctx, base, px, py, tab, player)) {
    click = rex::input::X_INPUT_GAMEPAD_START;
    if (debug) {
      REXLOG_INFO("Pointer: over the overshell tab of pad {}, highlight {:.3f},{:.3f}-{:.3f},{:.3f}", player, tab.x0,
                  tab.y0, tab.x1, tab.y1);
      for (const auto& run : tab.runs) REXLOG_INFO("Pointer:   glyphs of {}: {}", run.font, run.quads.size());
    }
    if (tab.x1 > tab.x0) highlights.push_back(std::move(tab));
    comp = 0;
  } else if (uint32_t action = OvershellOpen(base) ? 0 : HelpActionAt(base, px, py, label, line);
             action && !(comp && g_drawn.count(comp) && g_drawn[comp].order > g_drawn[label].order)) {
    // An open overshell covers the help bar and takes the input.
    click = ActionButton(action, PadIsGamepad(ctx, base));
    comp = 0;
    Highlight h;
    LocalBox glyphs;
    bool icons_only = false;
    bool have_glyphs = GlyphBox(base, Load32(base, label + 324), glyphs, &icons_only);
    if (debug) {
      REXLOG_INFO("Pointer: label {:08X} line {:.1f},{:.1f}-{:.1f},{:.1f} glyphs {} {:.1f},{:.1f}-{:.1f},{:.1f}", label,
                  line.left, line.top, line.right, line.bottom, have_glyphs, glyphs.left, glyphs.top, glyphs.right,
                  glyphs.bottom);
    }
    if (!have_glyphs) glyphs = line;
    uint32_t cam = g_drawn[label].cam;
    if (ScreenRect(base, label, cam, glyphs,
                   have_glyphs && icons_only ? HighlightStyle::kIcon : HighlightStyle::kButton, h)) {
      CollectGlyphRuns(base, label, Load32(base, label + 324), cam, h.runs);
      if (debug) {
        for (const auto& run : h.runs) REXLOG_INFO("Pointer:   glyphs of {} ({}): {}", run.font, run.icon, run.quads.size());
      }
      highlights.push_back(std::move(h));
    }
    if (debug) REXLOG_INFO("Pointer: over help action {}: button {:04X}", action, click);
  } else if (comp && Load32(base, comp) == kUISlider) {
    // Sliders take drags and the wheel, not A.
    click = 0;
    if (base[comp + kCompState] == kFocused) {
      g_pointer_on_list = true;
      if (pressed) {
        g_slider_drag = {comp};
        if (!SliderCanScroll(base, comp)) {
          Press(rex::input::X_INPUT_GAMEPAD_A);
          g_slider_drag.entered = true;
        }
        if (REXCVAR_GET(mouse_menu_debug)) {
          REXLOG_INFO("Pointer: slider {:08X} grabbed at step {} (now {}){}", comp, SliderStepAt(base, comp, local),
                      int(Load32(base, comp + kSliderCurrent)), g_slider_drag.entered ? ", selected first" : "");
        }
      }
    }
  } else if (on_list && !IsSongList(base, comp) && IsPinnedList(base, comp)) {
    // Click selects the row, then A; on the highlighted row the click's own A does it.
    bool focused = base[comp + kCompState] == kFocused;
    int selected = int(Load32(base, comp + kListState + kStateSelectedDisplay));
    if (focused && item >= 0 && item != selected) {
      click = 0;
      int count = NumShowing(ctx, base, comp);
      if (pressed && count > 0) {
        int first = int(Load32(base, comp + kListState + kStateFirstShowing));
        int pos = base[comp + kListState] ? (first + item) % count : first + item;
        if (pos < count) {
          SelectListItem(ctx, base, comp, pos, first);
          Press(rex::input::X_INPUT_GAMEPAD_A);
        }
      }
    }
    item = -1;
  } else if (on_list && IsSongList(base, comp)) {
    bool selected = base[comp + kCompState] == kFocused &&
                    item == int(Load32(base, comp + kListState + kStateSelectedDisplay));
    click = selected ? -1 : 0;
    item = -1;
  }
  g_click_player = player;
  g_click_buttons = click;
  {
    std::lock_guard lock(g_highlight_mutex);
    g_highlights = std::move(highlights);
    g_highlights_time = Clock::now();
  }
  if (moved && comp && (comp != g_target.comp || item != g_target.item)) {
    g_target = {comp, item, 0};
    if (debug) {
      REXLOG_INFO("Pointer: over {:08X} (vtable {:08X}) item {} at local {:.2f} {:.2f} {:.2f}{}", comp,
                  Load32(base, comp), item, local.x, local.y, local.z,
                  g_list_rows.count(comp) ? fmt::format(", rows centred {:.2f} along, middle {:.1f} across",
                                                        g_list_rows[comp].centre, g_list_rows[comp].across_mid)
                                          : "");
    }
  }
  if (int notches = g_wheel_notches.exchange(0)) {
    uint32_t focused = FocusedComponent(base);
    if (focused && Load32(base, focused) == kUISlider && !g_slider_drag.slider) {
      int current = int(Load32(base, focused + kSliderCurrent));
      int steps = int(Load32(base, focused + kSliderSteps));
      for (int n = 0; n < std::min(std::abs(notches), 4); ++n) {
        int target = std::clamp(current + (notches > 0 ? 1 : -1) * (n + 1), 0, steps - 1);
        if (target == current) break;
        using namespace rex::input;
        bool vertical = base[focused + kSliderVertical], up = notches > 0;
        if (SliderCanScroll(base, focused)) {
          Press(vertical ? (up ? X_INPUT_GAMEPAD_DPAD_UP : X_INPUT_GAMEPAD_DPAD_DOWN)
                         : (up ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT));
        }
      }
    } else if (focused && IsList(Load32(base, focused))) {
      if (IsSongList(base, focused)) {
        ScrollSongList(ctx, base, focused, notches);
      } else {
        ScrollList(ctx, base, focused, notches);
      }
    }
  }
  if (!g_target.comp) return;
  if (rex::input::mnk::PressesPending()) {
    // The device only sends presses while the window has focus.
    if (g_target.presses && Clock::now() - g_target.pressed > std::chrono::seconds(1)) g_target = {};
    return;
  }
  if (g_target.presses) {
    uint32_t focused_now = FocusedComponent(base);
    int selected_now = focused_now && IsList(Load32(base, focused_now))
                           ? int(Load32(base, focused_now + kListState + kStateSelectedDisplay))
                           : -1;
    bool changed = focused_now != g_target.waiting_focus || selected_now != g_target.waiting_selected;
    if (!changed && Clock::now() - g_target.pressed < std::chrono::milliseconds(400)) return;
  }
  if (g_target.presses >= kMaxPresses || !g_drawn.count(g_target.comp)) {
    g_target = {};
    return;
  }
  using namespace rex::input;
  // List of lists: the parent holds focus; pick the row there first.
  uint32_t row_parent = ListParent(base, g_target.comp);
  uint32_t target = row_parent ? row_parent : g_target.comp;
  if (base[target + kCompState] != kFocused) {
    uint32_t focused = FocusedComponent(base);
    // Nested screens (song list's VIEW MORE INFO) are their own PanelDir, but
    // input goes to the outer panel's focus, so set it in both.
    uint32_t dir = PanelDirOf(base, target);
    if (dir && focused && PanelDirOf(base, focused) == dir && SetFocusEverywhere(ctx, base, focused, target, dir)) {
      if (REXCVAR_GET(mouse_menu_debug)) REXLOG_INFO("Pointer: focus set on {:08X}", target);
      if (g_target.item < 0) g_target = {};
      return;
    }
    float fx, fy, tx, ty;
    if (!focused || !ScreenPos(base, focused, fx, fy) || !ScreenPos(base, target, tx, ty)) {
      g_target = {};
      return;
    }
    float dx = tx - fx, dy = ty - fy;
    if (std::fabs(dy) >= std::fabs(dx)) {
      Press(dy > 0 ? X_INPUT_GAMEPAD_DPAD_DOWN : X_INPUT_GAMEPAD_DPAD_UP);
    } else {
      Press(dx > 0 ? X_INPUT_GAMEPAD_DPAD_RIGHT : X_INPUT_GAMEPAD_DPAD_LEFT);
    }
    NotePress(base);
    return;
  }
  if (g_target.item < 0) {
    g_target = {};
    return;
  }
  if (row_parent) {
    // Empty cell in a short last row.
    int child_first = int(Load32(base, g_target.comp + kListState + kStateFirstShowing));
    if (child_first + g_target.item > NumShowing(ctx, base, g_target.comp) - 1) {
      g_target = {};
      return;
    }
    // Row = offset from the topmost row list, in parent spacings.
    uint32_t dir = Load32(base, row_parent + kListDir);
    bool vertical = Load32(base, dir + kDirOrientation) == 0;
    float spacing = LoadF(base, dir + kDirSpacing);
    auto along = [&](uint32_t c) {
      Vec3 l = ToLocal(base, row_parent, LoadVec(base, c + kCompXfm + 48));
      return vertical ? -l.z : l.x;
    };
    float top = 1e30f;
    for (const auto& [c, drawn] : g_drawn) {
      if (ListParent(base, c) == row_parent) top = std::min(top, along(c));
    }
    int row = spacing > 0 ? int(std::lround((along(g_target.comp) - top) / spacing)) : -1;
    int first = int(Load32(base, row_parent + kListState + kStateFirstShowing));
    int selected = int(Load32(base, row_parent + kListState + kStateSelectedDisplay));
    if (row >= 0 && row != selected && first + row <= NumShowing(ctx, base, row_parent) - 1) {
      SelectListItem(ctx, base, row_parent, first + row, first);
    }
    target = g_target.comp;
  }
  int first = int(Load32(base, target + kListState + kStateFirstShowing));
  int selected = int(Load32(base, target + kListState + kStateSelectedDisplay));
  // Skip empty rows and headings (DX FILTERS page): selecting one makes the
  // game move on with the scroll sound, on every move.
  int last = NumShowing(ctx, base, target) - 1;
  if (selected != g_target.item && first + g_target.item <= last &&
      IsActive(ctx, base, target, first + g_target.item)) {
    SelectListItem(ctx, base, target, first + g_target.item, first);
    if (int(Load32(base, target + kListState + kStateFirstShowing)) != first) g_pinned_lists.insert(target);
  }
  g_target = {};
}

}  // namespace

// World-x offset from the 16:9 edge to the screen edge on wider views.
// top_right_only: 0 unless in the 16:9 top-right corner.
float EdgeShift(const uint8_t* base, Vec3 pos, bool right, bool top_right_only = false) {
  double aspect = rb3pc::CurrentViewAspect();
  uint32_t cam = Load32(base, kCurrentCam);
  float sx, sy, depth;
  if (aspect <= 16.0 / 9.0 + 1e-3 || !cam || !Project(base, cam, pos, sx, sy, depth)) return 0;
  float width_16x9 = float((16.0 / 9.0) / aspect);
  float edge_16x9 = 0.5f + (right ? 0.5f : -0.5f) * width_16x9;
  // Right-aligned text has its origin on the edge.
  if (top_right_only &&
      !(sy < 0.3f && sx > edge_16x9 - 0.4f * width_16x9 && sx < edge_16x9 + 0.02f * width_16x9)) {
    return 0;
  }
  return Unproject(base, cam, right ? 1.f : 0.f, sy, depth).x - Unproject(base, cam, edge_16x9, sy, depth).x;
}

// Component local xfm +0x40, world +0x80. Both are shifted so the game's
// recompute and the pointer agree; reapplied when the game writes a new
// local position.
constexpr uint32_t kCompLocalXfm = 0x40;
float ShiftWithEdge(uint8_t* base, uint32_t comp, bool right, bool top_right_only = false) {
  struct Shifted {
    float local, written_local, world_shift;
  };
  static std::unordered_map<uint32_t, Shifted> shifted;
  uint32_t local_x = comp + kCompLocalXfm + 48, world_x = comp + kCompXfm + 48;
  float local = LoadF(base, local_x);
  auto it = shifted.find(comp);
  bool known = it != shifted.end() && it->second.written_local == local;
  float original_local = known ? it->second.local : local;
  float world = LoadF(base, world_x) - (known ? it->second.world_shift : 0);
  Vec3 pos = LoadVec(base, world_x);
  pos.x = world;
  float shift = EdgeShift(base, pos, right, top_right_only);
  // Parent x scale from the first rows (menus aren't rotated).
  float local_scale = LoadF(base, comp + kCompLocalXfm), world_scale = LoadF(base, comp + kCompXfm);
  float local_shift = std::fabs(world_scale) > 1e-6f ? shift * local_scale / world_scale : 0;
  StoreF(base, local_x, original_local + local_shift);
  StoreF(base, world_x, world + shift);
  shifted[comp] = {original_local, original_local + local_shift, shift};
  return shift;
}

// Main menu panel (holds mb_playnow.btn and expand_message_area.ihp) follows
// the left edge.
uint32_t g_main_menu_dir = 0;
bool ShiftMainMenuItem(uint8_t* base, uint32_t comp) {
  uint32_t object = Load32(base, comp + 0x30);
  if (object <= comp || object >= comp + 0x400) return false;
  uint32_t dir = Load32(base, object + 0x1C);
  if (dir < 0x40000000 || dir >= 0x90000000) return false;
  if (dir != g_main_menu_dir) {
    if (ObjectName(base, object) != "mb_playnow.btn") return false;
    g_main_menu_dir = dir;
  }
  ShiftWithEdge(base, comp, false);
  return true;
}

// A label's RndText (+0x144) and glyph meshes only recompute world xfms on
// relayout, not when the label moves, so redo it while drawing a shifted
// label. RndTransformable (component/mesh + 0x24): parent +0x10, local xfm
// +0x1C, world +0x5C.
void UpdateWorldPos(uint8_t* base, uint32_t trans) {
  uint32_t parent = Load32(base, trans + 0x10);
  if (parent < 0x40000000 || parent >= 0x90000000) return;
  Vec3 local = LoadVec(base, trans + 0x1C + 48);
  uint32_t m = parent + 0x5C;
  for (int j = 0; j < 3; ++j) {
    float v = local.x * LoadF(base, m + 4 * j) + local.y * LoadF(base, m + 16 + 4 * j) +
              local.z * LoadF(base, m + 32 + 4 * j) + LoadF(base, m + 48 + 4 * j);
    StoreF(base, trans + 0x5C + 48 + 4 * j, v);
  }
}
uint32_t g_label_text_following = 0;
void FollowLabelText(uint8_t* base, uint32_t label, bool own) {
  uint32_t text = Load32(base, label + 0x144);
  if (text < 0x40000000 || text >= 0x50000000) return;
  if (own) UpdateWorldPos(base, label + 0x24);
  UpdateWorldPos(base, text + 0x24);
  g_label_text_following = text + 0x24;
}

// DX's now-playing lines under the band info (line0-3.lbl, no transform
// parent) follow the right edge. Their text keeps its own world xfm, so it's
// drawn offset by g_label_text_shift.
float g_label_text_shift = 0;
bool ShiftNowPlayingLine(uint8_t* base, uint32_t label, uint32_t object) {
  if constexpr (!rb3pc::exe::kDeluxe) return false;
  static std::unordered_map<uint32_t, bool> is_line;
  auto [it, added] = is_line.try_emplace(object, false);
  if (added) {
    std::string name = ObjectName(base, object);
    it->second = name.size() == 9 && name.starts_with("line") && name[4] >= '0' && name[4] <= '3' &&
                 name.ends_with(".lbl");
  }
  if (!it->second || Load32(base, label + 0x34) != 0) return false;
  g_label_text_shift = ShiftWithEdge(base, label, true, true);
  return true;
}

// Band info (panel holding gamertag.lbl) follows the right edge: shift the
// panel's transform (labels recompute from it each frame); its meshes, its
// sliders' meshes and the parentless fan meter (meter_display) are drawn
// offset instead.
uint32_t g_band_info_trans = 0, g_band_info_dir = 0;
float g_band_info_shift = 0;
bool g_band_info_slider_drawing = false;
int g_label_drawing = 0;
void ShiftBandInfo(uint8_t* base, uint32_t trans) {
  struct Shifted {
    float local, written_local, world_shift;
  };
  static Shifted shifted{};
  static uint32_t shifted_trans = 0;
  uint32_t local_x = trans + 0x1C + 48, world_x = trans + 0x5C + 48;
  float local = LoadF(base, local_x);
  bool known = shifted_trans == trans && shifted.written_local == local;
  float original_local = known ? shifted.local : local;
  float world = LoadF(base, world_x) - (known ? shifted.world_shift : 0);
  double aspect = rb3pc::CurrentViewAspect();
  uint32_t cam = Load32(base, kCurrentCam);
  float sx, sy, depth;
  Vec3 pos = LoadVec(base, world_x);
  pos.x = world;
  float shift = 0;
  if (aspect > 16.0 / 9.0 + 1e-3 && cam && Project(base, cam, pos, sx, sy, depth)) {
    float right_16x9 = float(0.5 + 0.5 * (16.0 / 9.0) / aspect);
    shift = Unproject(base, cam, 1, sy, depth).x - Unproject(base, cam, right_16x9, sy, depth).x;
  }
  float local_scale = LoadF(base, trans + 0x1C), world_scale = LoadF(base, trans + 0x5C);
  float local_shift = std::fabs(world_scale) > 1e-6f ? shift * local_scale / world_scale : 0;
  StoreF(base, local_x, original_local + local_shift);
  StoreF(base, world_x, world + shift);
  shifted = {original_local, original_local + local_shift, shift};
  shifted_trans = trans;
  g_band_info_shift = shift;
}
bool InBandInfo(const uint8_t* base, uint32_t drawable) {
  if (g_band_info_slider_drawing) return true;
  if (g_label_drawing) return false;
  if (!g_band_info_trans) return false;
  uint32_t trans = Load32(base, drawable + 0x34), root = 0;
  for (int depth = 0; depth < 12 && trans >= 0x40000000 && trans < 0x90000000; ++depth) {
    if (trans == g_band_info_trans) return true;
    root = trans;
    trans = Load32(base, trans + 0x10);
  }
  if (!root) return false;
  // Fan meter: root RndTransformable is 0x24 into its object.
  static std::unordered_map<uint32_t, bool> is_meter;
  auto [it, added] = is_meter.try_emplace(root, false);
  if (added) {
    uint32_t comp = root - 0x24, object = Load32(base, comp + 0x30);
    it->second = object > comp && object < comp + 0x400 && ObjectName(base, object) == "meter_display";
  }
  if (!it->second) return false;
  uint32_t cam = Load32(base, kCurrentCam);
  float sx, sy, depth;
  if (!cam || !Project(base, cam, LoadVec(base, drawable + kCompXfm + 48), sx, sy, depth)) return false;
  double aspect = rb3pc::CurrentViewAspect();
  float right_16x9 = float(0.5 + 0.5 * (16.0 / 9.0) / aspect);
  float width_16x9 = float((16.0 / 9.0) / aspect);
  return sy < 0.25f && sx > right_16x9 - 0.4f * width_16x9 && sx < right_16x9;
}
void NoteBandInfoLabel(uint8_t* base, uint32_t label, uint32_t object) {
  static std::unordered_map<uint32_t, bool> is_gamertag;
  auto [it, added] = is_gamertag.try_emplace(object, false);
  if (added) it->second = ObjectName(base, object) == "gamertag.lbl";
  uint32_t trans = Load32(base, label + 0x34);
  if (it->second && trans >= 0x40000000 && trans < 0x90000000) {
    g_band_info_trans = trans;
    g_band_info_dir = Load32(base, object + 0x1C);
    ShiftBandInfo(base, trans);
  }
}
bool IsBandInfoComponent(const uint8_t* base, uint32_t comp) {
  uint32_t object = Load32(base, comp + 0x30);
  return g_band_info_dir && object > comp && object < comp + 0x400 &&
         Load32(base, object + 0x1C) == g_band_info_dir;
}

// Song list spread on wide views: the left half of the 16:9 picture follows
// the left edge, the right half the right edge, and meshes spanning the
// middle stretch (text column and art column edges mapped to their new
// spots). Screen-wide backgrounds are left alone.
//
// Parent +0x34, local xfm +0x40, world +0x80. Lists set rows' world xfms;
// others derive from the parent at draw time, so both are patched for the
// draw and restored.
Clock::time_point g_song_list_time;
uint32_t g_song_list_cam = 0;
constexpr float kSpreadSplit = 0.53f;      // in the 16:9 picture: the counts are right of it, titles left
constexpr float kSpreadArtColumn = 0.70f;   // the art column's left edge, in the 16:9 picture
constexpr float kSpreadTextColumn = 0.07f;  // where the list's text starts
// Unspread positions computed at draw time, for the next frame (the stored
// world position is stale or unset before the draw).
std::unordered_map<uint32_t, Vec3> g_spread_drawn_at;
void NoteSongListDrawn(const uint8_t* base, uint32_t list) {
  if (!IsSongList(base, list)) return;
  auto now = Clock::now();
  if (now - g_song_list_time > std::chrono::seconds(1)) g_spread_drawn_at.clear();
  g_song_list_time = now;
  g_song_list_cam = Load32(base, kCurrentCam);
}
struct SpreadSaved {
  uint32_t object = 0, parent = 0;
  float k = 1, c = 0, written_x = 0;
  uint8_t local[64], world[64], parent_world[64];
};

// Undo with EndSpread. Only meshes stretch.
bool BeginSpread(uint8_t* base, uint32_t object, bool mesh, SpreadSaved& saved) {
  double aspect = rb3pc::CurrentViewAspect();
  if (aspect <= 16.0 / 9.0 + 1e-3 || !g_song_list_cam) return false;
  uint32_t cam = Load32(base, kCurrentCam);
  if (cam != g_song_list_cam || Clock::now() - g_song_list_time > std::chrono::milliseconds(200)) return false;
  float width_16x9 = float((16.0 / 9.0) / aspect), edge = 0.5f - 0.5f * width_16x9;
  float split = edge + kSpreadSplit * width_16x9, art = edge + kSpreadArtColumn * width_16x9;
  float text_column = edge + kSpreadTextColumn * width_16x9;
  uint32_t local = object + 0x40, world = object + kCompXfm, parent = Load32(base, object + 0x34);
  bool has_parent = parent >= 0x40000000 && parent < 0x90000000;
  Vec3 row[4];
  for (int i = 0; i < 4; ++i) row[i] = LoadVec(base, world + 16 * i);
  auto to_world = [&](Vec3 v) {
    return Vec3{v.x * row[0].x + v.y * row[1].x + v.z * row[2].x + row[3].x,
                v.x * row[0].y + v.y * row[1].y + v.z * row[2].y + row[3].y,
                v.x * row[0].z + v.y * row[1].z + v.z * row[2].z + row[3].z};
  };
  // An unset world position (at the origin, or a mesh with only x set) is
  // rebuilt from the local chain up to the nearest parent that has one.
  Vec3 at_pos = row[3];
  uint32_t placed_by = 0;
  if (has_parent) {
    Vec3 pos = LoadVec(base, local + 48);
    uint32_t trans = parent;
    for (int depth = 0; depth < 8 && trans >= 0x40000000 && trans < 0x90000000; ++depth) {
      Vec3 x = LoadVec(base, trans + 0x5C);
      if (x.x * x.x + x.y * x.y + x.z * x.z > 1e-6f) break;
      pos = Transform(base, trans + 0x1C, pos);
      trans = Load32(base, trans + 0x10);
    }
    if (trans >= 0x40000000 && trans < 0x90000000) {
      Vec3 fresh = Transform(base, trans + 0x5C, pos);
      float wx, wy, fx, fy, d;
      bool at_origin = Project(base, cam, at_pos, wx, wy, d) && Project(base, cam, fresh, fx, fy, d) &&
                       std::fabs(wx - 0.5f) + std::fabs(wy - 0.5f) < 0.01f &&
                       std::fabs(fx - wx) + std::fabs(fy - wy) > 0.05f;
      if (at_origin || (mesh && at_pos.y == 0 && at_pos.z == 0)) {
        at_pos = fresh;
        placed_by = trans + 0x5C;
      }
    }
  }
  if (auto drawn = g_spread_drawn_at.find(object); drawn != g_spread_drawn_at.end()) at_pos = drawn->second;
  Vec3 center = mesh ? to_world(LoadVec(base, object + kMeshSphere)) : row[3];
  center = {center.x - row[3].x + at_pos.x, center.y - row[3].y + at_pos.y, center.z - row[3].z + at_pos.z};
  float radius = mesh ? LoadF(base, object + kMeshRadius) *
                            std::sqrt(row[0].x * row[0].x + row[0].y * row[0].y + row[0].z * row[0].z)
                      : 0;
  float sx, sy, depth, left, right, unused;
  if (!Project(base, cam, center, sx, sy, depth)) return false;
  auto at = [&](float screen_x) { return Unproject(base, cam, screen_x, sy, depth).x; };
  float k = 1, c;  // world x -> k x + c
  if (radius > 0 && Project(base, cam, {center.x - radius, center.y, center.z}, left, unused, unused) &&
      Project(base, cam, {center.x + radius, center.y, center.z}, right, unused, unused) && left < split &&
      right > split) {
    if (right - left > 1.2f) return false;
    float u1 = at(text_column), u2 = at(art);
    k = (at(art + edge) - at(text_column - edge)) / (u2 - u1);
    c = at(text_column - edge) - k * u1;
  } else {
    // By origin: a mesh's bounds can be well off it.
    if (!Project(base, cam, at_pos, sx, sy, depth)) return false;
    c = at(sx < split ? sx - edge : sx + edge) - at(sx);
  }
  saved.object = object;
  std::memcpy(saved.local, base + local, 64);
  std::memcpy(saved.world, base + world, 64);
  for (int i = 0; i < 3; ++i) {
    StoreVec(base, world + 16 * i, {row[i].x * k, row[i].y, row[i].z});
    StoreF(base, local + 16 * i, LoadF(base, local + 16 * i) * k);  // the parents aren't rotated
  }
  StoreVec(base, world + 48, {row[3].x * k + c, row[3].y, row[3].z});
  saved.k = k, saved.c = c, saved.written_x = row[3].x * k + c;
  if (placed_by && mesh) {
    // Drawn from that parent's transform: move it instead.
    saved.parent = placed_by;
    std::memcpy(saved.parent_world, base + saved.parent, 64);
    Vec3 p = LoadVec(base, saved.parent + 48);
    StoreF(base, saved.parent + 48, p.x * k + c);
  }
  if (has_parent) {
    Vec3 fresh = Transform(base, parent + 0x5C, LoadVec(base, local + 48));
    Vec3 a = ToLocal(base, parent - 0x24, fresh), b = ToLocal(base, parent - 0x24, {fresh.x * k + c, fresh.y, fresh.z});
    Vec3 l = LoadVec(base, local + 48);
    StoreVec(base, local + 48, {l.x + b.x - a.x, l.y + b.y - a.y, l.z + b.z - a.z});
  }
  return true;
}
void EndSpread(uint8_t* base, const SpreadSaved& saved) {
  if (!saved.object) return;
  Vec3 drawn = LoadVec(base, saved.object + kCompXfm + 48);
  if (saved.k == 1 && drawn.x != saved.written_x) {
    g_spread_drawn_at[saved.object] = {drawn.x - saved.c, drawn.y, drawn.z};
  }
  std::memcpy(base + saved.object + 0x40, saved.local, 64);
  std::memcpy(base + saved.object + kCompXfm, saved.world, 64);
  if (saved.parent) std::memcpy(base + saved.parent, saved.parent_world, 64);
}

// Game-thread tick, throttled to ~once per frame.
REX_HOOK_RAW(MM_COMPONENT_POLL) {
  MM_COMPONENT_POLL_ORIG(ctx, base);
  static Clock::time_point last;
  auto now = Clock::now();
  if (now - last < std::chrono::milliseconds(15)) return;
  last = now;
  UpdatePointer(ctx, base);
}

// Captures gNullStr.
REX_HOOK_RAW(MM_SET_FOCUS_COMPONENT) {
  uint32_t nav_type = ctx.r5.u32;
  if (!g_null_symbol && nav_type >= 0x40000000 && nav_type < 0x90000000 && base[nav_type] == 0) {
    g_null_symbol = nav_type;
  }
  MM_SET_FOCUS_COMPONENT_ORIG(ctx, base);
}

REX_HOOK_RAW(MM_BANDBUTTON_DRAW) {
  ShiftMainMenuItem(base, ctx.r3.u32);
  NoteDrawn(ctx.r3.u32, base);
  MM_BANDBUTTON_DRAW_ORIG(ctx, base);
}
REX_HOOK_RAW(MM_BANDLIST_DRAW) {
  uint32_t list = ctx.r3.u32, outer = g_list_drawing;  // a list of lists draws its rows' lists
  NoteDrawn(list, base);
  g_list_drawing = list;
  MM_BANDLIST_DRAW_ORIG(ctx, base);
  g_list_drawing = outer;
  FinishListRows(list);
}
REX_HOOK_RAW(MM_LABEL_DRAW) {
  uint32_t label = ctx.r3.u32;
  // Hide the overshell's ring-of-light quadrant (pad_num.lbl).
  uint32_t object = Load32(base, label + 0x30);
  bool has_object = object > label && object < label + 0x400;
  if (has_object && ObjectName(base, object) == "pad_num.lbl") return;
  if (has_object) NoteBandInfoLabel(base, label, object);
  if (!has_object || !ShiftNowPlayingLine(base, label, object)) {
    if (ShiftMainMenuItem(base, label)) {
      FollowLabelText(base, label, false);
    } else if (IsBandInfoComponent(base, label)) {
      FollowLabelText(base, label, true);
    }
  }
  NoteDrawn(label, base);
  ++g_label_drawing;
  uint32_t text = Load32(base, label + 0x144), text_x = text + kCompXfm + 48;
  if (SpreadSaved saved; BeginSpread(base, label, false, saved)) {
    bool has_text = text >= 0x40000000 && text < 0x50000000;
    float tx = has_text ? LoadF(base, text_x) : 0;
    if (has_text) UpdateWorldPos(base, text + 0x24);
    MM_LABEL_DRAW_ORIG(ctx, base);
    if (has_text) StoreF(base, text_x, tx);
    EndSpread(base, saved);
  } else if (g_label_text_shift != 0 && text >= 0x40000000 && text < 0x50000000) {
    float x = LoadF(base, text_x);
    StoreF(base, text_x, x + g_label_text_shift);
    g_label_text_shift = 0;
    g_label_text_following = text + 0x24;  // a line with a button in it draws its glyphs' meshes
    MM_LABEL_DRAW_ORIG(ctx, base);
    StoreF(base, text_x, x);
  } else {
    g_label_text_shift = 0;
    MM_LABEL_DRAW_ORIG(ctx, base);
  }
  g_label_text_following = 0;
  --g_label_drawing;
}
REX_HOOK_RAW(MM_UILIST_DRAW) {
  uint32_t list = ctx.r3.u32, outer = g_list_drawing;
  NoteSongListDrawn(base, list);
  NoteDrawn(list, base);
  g_list_drawing = list;
  MM_UILIST_DRAW_ORIG(ctx, base);
  g_list_drawing = outer;
  FinishListRows(list);
}
REX_HOOK_RAW(MM_SLIDER_DRAW) {
  uint32_t slider = ctx.r3.u32;
  NoteDrawn(slider, base);
  g_slider_meshes.clear();
  g_slider_drawing = slider;
  g_band_info_slider_drawing = IsBandInfoComponent(base, slider);
  MM_SLIDER_DRAW_ORIG(ctx, base);
  g_band_info_slider_drawing = false;
  g_slider_drawing = 0;
  const SliderMesh* knob = nullptr;
  for (const auto& m : g_slider_meshes) {
    if (m.radius > 0 && (!knob || m.radius < knob->radius)) knob = &m;
  }
  if (!knob) return;
  bool vertical = base[slider + kSliderVertical];
  auto along = [&](Vec3 v) { return vertical ? v.z : v.x; };
  auto across = [&](Vec3 v) { return vertical ? v.x : v.z; };
  const SliderMesh* track = nullptr;
  for (const auto& m : g_slider_meshes) {
    if (&m == knob || std::fabs(along(m.centre) - along(knob->centre)) > m.radius) continue;
    if (!track || m.radius > track->radius) track = &m;
  }
  if (!track) return;
  float lo = along(track->centre) - track->radius, hi = along(track->centre) + track->radius;
  float mid = across(knob->centre), half = knob->radius;
  g_slider_boxes[slider] = vertical ? LocalBox{mid - half, hi, mid + half, lo} : LocalBox{lo, mid + half, hi, mid - half};
  g_slider_travel[slider] = {lo + knob->radius, hi - knob->radius};
}
// Preview picks to dim: scores in (from, to].
uint32_t g_review_drawing = 0;
int g_review_dim_from = 0, g_review_dim_to = 0;

// Overshell bottom bar (overshell.milo connect_controller meshes) stretched
// about screen centre on wide views. Mesh name's const char* is at -0x20.
constexpr float kBarWidthIn16x9 = 1.13f;  // of the 16:9 picture's width
bool IsOvershellBar(const uint8_t* base, uint32_t mesh) {
  static std::unordered_map<uint32_t, bool> known;
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  auto [it, added] = known.try_emplace(mesh, false);
  if (added) {
    uint32_t name = Load32(base, mesh - 0x20);
    if ((name >= 0x40000000 && name < 0x50000000) || (name >= 0x82000000 && name < 0x83000000)) {
      std::string_view s(reinterpret_cast<const char*>(base + name), strnlen(reinterpret_cast<const char*>(base + name), 64));
      it->second = s == "connect_controller.mesh" || s == "connect_controller02.mesh" ||
                   s == "connect_controller_refract.mesh";
    }
  }
  return it->second;
}
void DrawOvershellBarStretched(PPCContext& ctx, uint8_t* base, uint32_t mesh) {
  float stretch = float(rb3pc::CurrentViewAspect() / (16.0 / 9.0)) / kBarWidthIn16x9 * 1.04f;
  uint32_t cam = Load32(base, kCurrentCam);
  float sx, sy, depth;
  Vec3 origin = LoadVec(base, mesh + kCompXfm + 48);
  if (stretch <= 1 || !cam || !Project(base, cam, origin, sx, sy, depth)) {
    return MM_DXMESH_DRAW_ORIG(ctx, base);
  }
  float centre = Unproject(base, cam, 0.5f, sy, depth).x;
  uint32_t xfm = mesh + kCompXfm;
  uint8_t saved[64];
  std::memcpy(saved, base + xfm, sizeof(saved));
  for (uint32_t row = 0; row < 3; ++row) StoreF(base, xfm + row * 16, LoadF(base, xfm + row * 16) * stretch);
  StoreF(base, xfm + 48, centre + (origin.x - centre) * stretch);
  MM_DXMESH_DRAW_ORIG(ctx, base);
  std::memcpy(base + xfm, saved, sizeof(saved));
}

// Review picks are one DxMesh each; dim them with RndMat colour (+0x2C, RGB floats).
constexpr uint32_t kMeshMat = 0xF8, kMatColor = 0x2C;
constexpr float kReviewDimmed = 0.65f;

void DrawMesh(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(MM_DXMESH_DRAW) {
  uint32_t mesh = ctx.r3.u32;
  SpreadSaved saved;
  if (g_label_drawing || IsOvershellBar(base, mesh) || !BeginSpread(base, mesh, true, saved)) {
    return DrawMesh(ctx, base);
  }
  DrawMesh(ctx, base);
  EndSpread(base, saved);
}
void DrawMesh(PPCContext& ctx, uint8_t* base) {
  uint32_t mesh = ctx.r3.u32;
  if (IsOvershellBar(base, mesh)) return DrawOvershellBarStretched(ctx, base, mesh);
  if (g_label_text_following && Load32(base, mesh + 0x34) == g_label_text_following) {
    UpdateWorldPos(base, mesh + 0x24);
  }
  if (g_band_info_shift != 0 && InBandInfo(base, mesh)) {
    uint32_t pos_x = mesh + kCompXfm + 48;
    float x = LoadF(base, pos_x);
    StoreF(base, pos_x, x + g_band_info_shift);
    MM_DXMESH_DRAW_ORIG(ctx, base);
    StoreF(base, pos_x, x);
    return;
  }
  if (g_list_drawing && !g_slider_drawing) {
    auto scale = [&](uint32_t xfm) {
      Vec3 a = LoadVec(base, xfm);
      return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    };
    float list_scale = scale(g_list_drawing + kCompXfm);
    float radius = list_scale > 1e-6f ? LoadF(base, mesh + kMeshRadius) * scale(mesh + kCompXfm) / list_scale : 0;
    NoteListMesh(base, g_list_drawing, mesh, Transform(base, mesh + kCompXfm, LoadVec(base, mesh + kMeshSphere)),
                 std::clamp(radius, 0.f, 1000.f));
  }
  if (g_slider_drawing && g_slider_meshes.size() < 16) {
    auto scale = [&](uint32_t xfm) {
      Vec3 a = LoadVec(base, xfm);
      return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    };
    float slider_scale = scale(g_slider_drawing + kCompXfm);
    Vec3 world = Transform(base, mesh + kCompXfm, LoadVec(base, mesh + kMeshSphere));
    if (slider_scale > 1e-6f) {
      g_slider_meshes.push_back({ToLocal(base, g_slider_drawing, world),
                                 LoadF(base, mesh + kMeshRadius) * scale(mesh + kCompXfm) / slider_scale});
    }
  }
  uint32_t mat = g_review_drawing ? Load32(base, mesh + kMeshMat) : 0;
  if (mat < 0x40000000 || mat >= 0x50000000) return MM_DXMESH_DRAW_ORIG(ctx, base);
  Vec3 local = ToLocal(base, g_review_drawing, LoadVec(base, mesh + kCompXfm + 48));
  int pick = int(std::lround((local.x - kReviewFirstX) / kReviewSpacing)) + 1;
  if (pick <= g_review_dim_from || pick > g_review_dim_to) return MM_DXMESH_DRAW_ORIG(ctx, base);
  uint8_t saved[12];
  std::memcpy(saved, base + mat + kMatColor, 12);
  for (int c = 0; c < 3; ++c) StoreF(base, mat + kMatColor + 4 * c, LoadF(base, mat + kMatColor + 4 * c) * kReviewDimmed);
  MM_DXMESH_DRAW_ORIG(ctx, base);
  std::memcpy(base + mat + kMatColor, saved, 12);
}
REX_HOOK_RAW(MM_REVIEW_DRAW) {
  uint32_t review = ctx.r3.u32;
  if (!review) return MM_REVIEW_DRAW_ORIG(ctx, base);
  g_reviews[review] = {Load32(base, kCurrentCam), Clock::now(), ++g_draw_count};
  int preview = g_review_preview_comp == review ? g_review_preview_score.load() : -1;
  uint32_t score = Load32(base, review + kReviewScore);
  if (preview < 0 || uint32_t(preview) == score) return MM_REVIEW_DRAW_ORIG(ctx, base);
  uint32_t shown = __builtin_bswap32(uint32_t(preview));
  std::memcpy(base + review + kReviewScore, &shown, 4);
  g_review_drawing = review;
  g_review_dim_from = int(score);
  g_review_dim_to = preview;
  MM_REVIEW_DRAW_ORIG(ctx, base);
  g_review_drawing = 0;
  uint32_t real = __builtin_bswap32(score);
  std::memcpy(base + review + kReviewScore, &real, 4);
}
REX_HOOK_RAW(MM_SCROLLBAR_DRAW) {
  if (ctx.r3.u32) g_scrollbars[ctx.r3.u32] = {Load32(base, kCurrentCam), Clock::now(), ++g_draw_count};
  MM_SCROLLBAR_DRAW_ORIG(ctx, base);
}
REX_HOOK_RAW(MM_HELP_DRAW) {
  ShiftMainMenuItem(base, ctx.r3.u32);
  if (ctx.r3.u32) g_help_bars[ctx.r3.u32] = {Load32(base, kCurrentCam), Clock::now(), ++g_draw_count};
  MM_HELP_DRAW_ORIG(ctx, base);
}

namespace rb3pc {
namespace {

// Font milos searched for "x.tex" atlases, loaded on first use.
constexpr const char* kFontMilos[] = {
    "pentatonic", "pentatonic_bold", "pentatonic_boldsmall", "pentatonic_display",
    "pentatonic_regularcond", "pentatonic_regularsmall", "pentatonic_mononumerals",
    "pentatonic_outline", "buttons", "button",
};

class PointerOverlay final : public rex::ui::ImGuiDialog {
 public:
  PointerOverlay(rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                 std::filesystem::path game_root)
      : ImGuiDialog(drawer),
        immediate_drawer_(immediate_drawer),
        game_root_(std::move(game_root)),
        ui_(immediate_drawer, game_root_) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ui_.Update();
    std::vector<Highlight> highlights;
    {
      std::lock_guard lock(g_highlight_mutex);
      // Stale during loading screens.
      if (Clock::now() - g_highlights_time > std::chrono::milliseconds(150)) return;
      highlights = g_highlights;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    GameArea area = GameAreaIn(io.DisplaySize.x, io.DisplaySize.y);
    float w = area.width, h = area.height;
    auto to_screen = [&](float x, float y) { return ImVec2(area.x + x * w, area.y + y * h); };
    for (const auto& hl : highlights) {
      ImVec2 a = to_screen(hl.x0, hl.y0), b = to_screen(hl.x1, hl.y1);
      switch (hl.style) {
        case HighlightStyle::kButton:
        case HighlightStyle::kIcon:
          DrawLabelHighlight(draw, hl, a, b, h, to_screen);
          break;
        case HighlightStyle::kThumb:
          draw->AddRectFilled(a, b, IM_COL32(240, 205, 40, 150));
          break;
        case HighlightStyle::kThumbHeld:
          draw->AddRectFilled(a, b, IM_COL32(240, 205, 40, 230));
          break;
      }
    }
  }

 private:
  struct FontAtlas {
    std::future<std::optional<Rgba8Image>> loading;
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    bool failed = false;
  };

  rex::ui::ImmediateTexture* Atlas(const std::string& name) {
    FontAtlas& atlas = atlases_[name];
    if (atlas.texture) return atlas.texture.get();
    if (atlas.failed) return nullptr;
    if (!atlas.loading.valid()) {
      atlas.loading = std::async(std::launch::async, [root = game_root_, name] {
        std::string stem = name.size() > 5 && name.ends_with(".font") ? name.substr(0, name.size() - 5) : name;
        // Longest milo-name prefix first ("Pentatonic_Bold_(3_00)4x" -> pentatonic_bold).
        std::string lower = stem;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        std::vector<const char*> milos(std::begin(kFontMilos), std::end(kFontMilos));
        std::stable_sort(milos.begin(), milos.end(), [&](const char* a, const char* b) {
          auto score = [&](const char* m) { return lower.starts_with(m) ? std::strlen(m) : 0; };
          return score(a) > score(b);
        });
        for (const char* milo : milos) {
          std::string path = std::string("ui/resource/fonts/gen/") + milo + ".milo_xbox";
          Rb3UiAssets assets = LoadRb3UiAssets(root, {{"atlas", path, stem + ".tex"}}, {});
          auto it = assets.images.find("atlas");
          if (it != assets.images.end()) return std::optional<Rgba8Image>(std::move(it->second));
        }
        return std::optional<Rgba8Image>();
      });
      return nullptr;
    }
    if (atlas.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    std::optional<Rgba8Image> image = atlas.loading.get();
    rex::ui::ImmediateDrawer* drawer = immediate_drawer_ ? immediate_drawer_() : nullptr;
    if (image && drawer) {
      atlas.texture = drawer->CreateTexture(image->width, image->height, rex::ui::ImmediateTextureFilter::kLinear,
                                            false, image->pixels.data());
    }
    if (!atlas.texture) {
      atlas.failed = true;
      REXLOG_WARN("Pointer: no atlas for font {}", name);
    }
    return atlas.texture.get();
  }

  // Main-menu-style yellow bar with the glyphs redrawn in black on top. Plain
  // outline until the atlases load (the bar would hide the text).
  template <typename ToScreen>
  void DrawLabelHighlight(ImDrawList* draw, const Highlight& hl, ImVec2 a, ImVec2 b, float h,
                          ToScreen to_screen) {
    std::vector<rex::ui::ImmediateTexture*> textures;
    bool ready = !hl.runs.empty();
    for (const auto& run : hl.runs) {
      textures.push_back(Atlas(run.font));
      ready = ready && textures.back();
    }
    float pad_x = h * (hl.style == HighlightStyle::kIcon ? 0.004f : 0.007f);
    float pad_y = hl.style == HighlightStyle::kIcon ? h * 0.004f : 0.0f;
    a = ImVec2(a.x - pad_x, a.y - pad_y);
    b = ImVec2(b.x + pad_x, b.y + pad_y);
    if (!ready) {
      draw->AddRect(a, b, kUiAccent, 0, 0, std::max(1.5f, h * 0.0025f));
      return;
    }
    float scale = h / 720.0f;
    ui_.DrawAccentBar(draw, a, b, scale);
    // The main menu's bar has a thin dark edge.
    draw->AddRect(ImVec2(a.x - 0.5f, a.y - 0.5f), ImVec2(b.x + 0.5f, b.y + 0.5f), IM_COL32(16, 16, 6, 190),
                  std::min(5.0f * scale, (b.y - a.y) * 0.5f), 0, std::max(1.0f, scale));
    for (size_t r = 0; r < hl.runs.size(); ++r) {
      const GlyphRun& run = hl.runs[r];
      auto id = reinterpret_cast<ImTextureID>(textures[r]);
      ImU32 color = run.icon ? run.color : IM_COL32(0, 0, 0, 255);
      for (const GlyphQuad& q : run.quads) {
        draw->AddImageQuad(id, to_screen(q.x[0], q.y[0]), to_screen(q.x[1], q.y[1]), to_screen(q.x[2], q.y[2]),
                           to_screen(q.x[3], q.y[3]), ImVec2(q.u[0], q.v[0]), ImVec2(q.u[1], q.v[1]),
                           ImVec2(q.u[2], q.v[2]), ImVec2(q.u[3], q.v[3]), color);
      }
    }
  }

  std::function<rex::ui::ImmediateDrawer*()> immediate_drawer_;
  std::filesystem::path game_root_;
  Rb3UiRenderer ui_;
  std::unordered_map<std::string, FontAtlas> atlases_;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreatePointerOverlay(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    const std::filesystem::path& game_root) {
  LoadIconInk(game_root);
  return std::make_unique<PointerOverlay>(drawer, std::move(immediate_drawer), game_root);
}

}  // namespace rb3pc
