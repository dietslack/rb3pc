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
#include <optional>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/presenter.h>

#include "exe.h"
#include "rb3_assets.h"
#include "rb3_ui.h"

REXCVAR_DEFINE_BOOL(mouse_menu_debug, false, "rb3pc", "Log what the mouse pointer hovers in menus");
REXCVAR_DEFINE_STRING(debug_pointer_script, "", "rb3pc",
                      "Testing aid: pointer positions over time instead of the mouse, "
                      "\"58:0.15,0.5;64:0.15,0.37;66:a\" (seconds after launch: x,y in 0-1, or "
                      "a / b for a click, u / d for a wheel notch, p / r to press / release "
                      "the left button, U D L R A S for d-pad / A / Start presses)")
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

bool IsList(uint32_t vtable) { return vtable == kBandList || vtable == kUIList; }
bool IsPointable(uint32_t vtable) {
  return vtable == kBandButton || vtable == kUIButton || IsList(vtable);
}

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
          default:  // a left click, as the click handler takes it
            if (REXCVAR_GET(mouse_menu_debug)) REXLOG_INFO("Pointer: scripted click, buttons {}", click);
            if (click) rex::input::mnk::QueuePress(click < 0 ? rex::input::X_INPUT_GAMEPAD_A : uint16_t(click));
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

int ListItemAt(const uint8_t* base, uint32_t list, Vec3 local) {
  uint32_t dir = Load32(base, list + kListDir);
  if (!dir) return -1;
  bool vertical = Load32(base, dir + kDirOrientation) == 0;
  float spacing = LoadF(base, dir + kDirSpacing);
  int num_display = int(Load32(base, list + kListState + kStateNumDisplay));
  if (!(spacing > 0) || num_display <= 0) return -1;
  int item = int(std::floor((vertical ? -local.z : local.x) / spacing));
  return item >= 0 && item < num_display ? item : -1;
}

// Components in the same menu as the focused one: those its navigation links
// (UIComponent NavRight / NavDown, ObjPtrs whose target is at +0xEC / +0xF8)
// reach, either way. Menus drawn but not active (a hub's other pages, faded
// out) aren't linked to it.
constexpr uint32_t kNavRight = 0xEC, kNavDown = 0xF8;

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
    for (const auto& [other, drawn] : g_drawn) {
      if (Load32(base, other + kNavRight) == c || Load32(base, other + kNavDown) == c) add(other);
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

// The component of the active menu under the pointer, and the hit in its
// local space.
uint32_t Hover(const uint8_t* base, float px, float py, Vec3& local) {
  uint32_t focused = FocusedComponent(base);
  if (!focused) return 0;
  for (uint32_t comp : ActiveMenu(base, focused)) {
    uint32_t vtable = Load32(base, comp), cam = g_drawn[comp].cam;
    if (!cam || !IsPointable(vtable) || base[comp + kCompState] == kDisabled) continue;
    float sx, sy, depth;
    if (!Project(base, cam, LoadVec(base, comp + kCompXfm + 48), sx, sy, depth)) continue;
    Vec3 a = Unproject(base, cam, px, py, depth * 0.25f);
    Vec3 b = Unproject(base, cam, px, py, depth * 4.0f);
    Vec3 hit;
    if (!PlaneHit(base, comp, a, b, hit)) continue;
    bool inside = IsList(vtable) ? ListItemAt(base, comp, hit) >= 0 : InButton(base, comp, hit);
    if (g_log_hover) {
      REXLOG_INFO("Pointer: {:08X} vt {:08X} state {} local {:.1f},{:.1f}: {}", comp, vtable,
                  base[comp + kCompState], hit.x, hit.z, inside ? "inside" : "outside");
    }
    if (inside) {
      local = hit;
      return comp;
    }
  }
  return 0;
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

}  // namespace
