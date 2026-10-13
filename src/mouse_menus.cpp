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

// The help bar label under the pointer: its action (or kActionNone), the
// label and the text line hit.
uint32_t HelpActionAt(const uint8_t* base, float px, float py, uint32_t& label_hit, LocalBox& line) {
  uint64_t best_order = 0;
  uint32_t action = kActionNone;
  for (const auto& [help, drawn] : g_help_bars) {
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
// thumb +348, top bone (RndMesh, local pos +112) +352, thumb position 0-1
// +388, thumb share of track +392. Track runs -z from the top bone.
constexpr uint32_t kBarList = 328, kBarHeight = 332, kBarMinThumb = 348, kBarTopBone = 352;
constexpr uint32_t kBarPosition = 388, kBarScale = 392, kBoneLocalPos = 112;
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
  out.x = LoadF(base, bone + kBoneLocalPos);
  out.top = LoadF(base, bone + kBoneLocalPos + 8);
  out.height = LoadF(base, bar + kBarHeight);
  out.thumb_height = std::max(scale * out.height, LoadF(base, bar + kBarMinThumb));
  out.thumb_top = out.top - LoadF(base, bar + kBarPosition) * (out.height - out.thumb_height);
  return out.height > 0;
}

bool ScrollbarAt(const uint8_t* base, float px, float py, Scrollbar& bar, float& z) {
  for (const auto& [b, drawn] : g_scrollbars) {
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

// Player 1's overshell tab (bottom left: name, instrument, "MENU" with the
// Start icon): the labels of the leftmost OvershellDir (vtable 0x8202CFC4,
// one per player slot; the keyboard and mouse are player 1) drawn in the
// bottom strip, where the tab sits while the overshell is closed. A click on
// it presses Start, which opens the overshell. Its highlight goes on the
// MENU row (the labels below the name, right of the instrument icon).
constexpr uint32_t kOvershellDirVtable = rb3pc::exe::kDeluxe ? 0x8202DBFC : 0x8202CFC4;
constexpr float kOvershellStrip = 0.86f;  // the closed tab is below this

bool OvershellOpen(const uint8_t* base) {
  uint32_t focused = FocusedComponent(base);
  uint32_t object = focused ? Load32(base, focused + 0x30) : 0;
  if (object <= focused || object >= focused + 0x400) return false;
  uint32_t dir = Load32(base, object + 0x1C);
  return dir >= 0x40000000 && dir < 0x50000000 && Load32(base, dir) == kOvershellDirVtable;
}

bool OvershellTabAt(const uint8_t* base, float px, float py, Highlight& highlight) {
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
  // Player 1's slot: the leftmost.
  uint32_t dir = std::min_element(labels.begin(), labels.end(), [](const Label& a, const Label& b) {
                   return a.rect.x0 < b.rect.x0;
                 })->dir;
  float x0 = 2, y0 = 2, x1 = -1, y1 = -1;
  for (const auto& l : labels) {
    if (l.dir != dir) continue;
    x0 = std::min(x0, l.rect.x0), y0 = std::min(y0, l.rect.y0);
    x1 = std::max(x1, l.rect.x1), y1 = std::max(y1, l.rect.y1);
  }
  float pad = (y1 - y0) * 0.15f;
  if (px < x0 - pad || px > x1 + pad || py < y0 - pad || py > y1 + pad) return false;
  // Not while this overshell is open (its menu has the focus): its rows are
  // its own to click.
  uint32_t focused = FocusedComponent(base);
  uint32_t object = focused ? Load32(base, focused + 0x30) : 0;
  if (object > focused && object < focused + 0x400 && Load32(base, object + 0x1C) == dir) return false;
  // The MENU row: below the middle of the tab, right of its first quarter.
  highlight = {2, 2, -1, -1, HighlightStyle::kButton, {}};
  for (const auto& l : labels) {
    if (l.dir != dir || (l.rect.y0 + l.rect.y1) * 0.5f < (y0 + y1) * 0.5f ||
        (l.rect.x0 + l.rect.x1) * 0.5f < x0 + (x1 - x0) * 0.25f) {
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
  if (!REXCVAR_GET(mnk_mode)) return;
  float px, py;
  if (!PointerPosition(px, py)) {
    g_review_preview_comp = 0;
    g_review_preview_score = -1;
    g_pointer_on_list = false;
    g_click_buttons = -1;
    g_drag = {};
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
  if (g_drag.bar && (!left || !g_scrollbars.count(g_drag.bar))) g_drag = {};
  if (g_drag.bar) {
    // Dragging: the thumb follows the pointer, wherever it is.
    Vec3 hit;
    Scrollbar sb;
    if (ReadScrollbar(base, g_drag.bar, g_scrollbars[g_drag.bar].cam, sb)) {
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
  } else if (Highlight tab; OvershellTabAt(base, px, py, tab)) {
    click = rex::input::X_INPUT_GAMEPAD_START;
    if (debug) {
      REXLOG_INFO("Pointer: over the overshell tab, highlight {:.3f},{:.3f}-{:.3f},{:.3f}", tab.x0, tab.y0, tab.x1, tab.y1);
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
  } else if (on_list && IsSongList(base, comp)) {
    bool selected = base[comp + kCompState] == kFocused &&
                    item == int(Load32(base, comp + kListState + kStateSelectedDisplay));
    click = selected ? -1 : 0;
    item = -1;
  }
  g_click_buttons = click;
  {
    std::lock_guard lock(g_highlight_mutex);
    g_highlights = std::move(highlights);
    g_highlights_time = Clock::now();
  }
  if (moved && comp && (comp != g_target.comp || item != g_target.item)) {
    g_target = {comp, item, 0};
    if (debug) {
      REXLOG_INFO("Pointer: over {:08X} (vtable {:08X}) item {} at local {:.2f} {:.2f} {:.2f}",
                  comp, Load32(base, comp), item, local.x, local.y, local.z);
    }
  }
  if (int notches = g_wheel_notches.exchange(0)) {
    uint32_t focused = FocusedComponent(base);
    if (focused && IsList(Load32(base, focused))) {
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
  uint32_t target = g_target.comp;
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
  // Within the focused list: select the item under the pointer, with the
  // view where it is (scrolling is the wheel's job).
  int first = int(Load32(base, target + kListState + kStateFirstShowing));
  int selected = int(Load32(base, target + kListState + kStateSelectedDisplay));
  // Rows past the end of a short list are empty: nothing to select there.
  int last = NumShowing(ctx, base, target) - 1;
  if (selected != g_target.item && first + g_target.item <= last) {
    SelectListItem(ctx, base, target, first + g_target.item, first);
  }
  g_target = {};
}

}  // namespace

// Every component polls through here; the pointer is updated once per frame
// or so, on the game thread, where the game's functions can be called.
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
  NoteDrawn(ctx.r3.u32, base);
  MM_BANDBUTTON_DRAW_ORIG(ctx, base);
}
REX_HOOK_RAW(MM_BANDLIST_DRAW) {
  NoteDrawn(ctx.r3.u32, base);
  MM_BANDLIST_DRAW_ORIG(ctx, base);
}
REX_HOOK_RAW(MM_LABEL_DRAW) {
  NoteDrawn(ctx.r3.u32, base);
  MM_LABEL_DRAW_ORIG(ctx, base);
}
REX_HOOK_RAW(MM_UILIST_DRAW) {
  NoteDrawn(ctx.r3.u32, base);
  MM_UILIST_DRAW_ORIG(ctx, base);
}
// Preview picks to dim: scores in (from, to].
uint32_t g_review_drawing = 0;
int g_review_dim_from = 0, g_review_dim_to = 0;

// While a review preview draws, its picks are DxMeshes (one per pick, at
// x = -90 + 45 * i in the display's space, each with its own material):
// those to dim are drawn with their material's colour (RndMat +0x2C, RGB
// floats, multiplying the texture) darkened, then it's put back.
constexpr uint32_t kMeshMat = 0xF8, kMatColor = 0x2C;
constexpr float kReviewDimmed = 0.65f;
REX_HOOK_RAW(MM_DXMESH_DRAW) {
  uint32_t mesh = ctx.r3.u32;
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
