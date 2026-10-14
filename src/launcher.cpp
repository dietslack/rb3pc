#include "launcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>

#include "audio_options.h"
#include "deluxe.h"
#include "display_options.h"
#include "input_bindings.h"
#include "instrument_input.h"
#include "profile.h"
#include "rb3_ui.h"

REXCVAR_DEFINE_BOOL(show_launcher, true, "rb3pc",
                    "Show the settings screen (profile, display, controllers) before the game "
                    "starts");

REXCVAR_DECLARE(std::string, debug_input_script);

namespace rb3pc {

namespace {

bool g_launcher_active = false;

using Clock = std::chrono::steady_clock;

// Menu actions, from any controller, the keyboard or the mouse.
enum Action : uint32_t {
  kUp = 1 << 0,
  kDown = 1 << 1,
  kLeft = 1 << 2,
  kRight = 1 << 3,
  kConfirm = 1 << 4,  // green fret / A / Enter
  kBack = 1 << 5,     // red fret / B / Backspace
  kStart = 1 << 6,    // Start / Escape-to-finish
};

// Every connected controller, read through its bindings profile (or SDL's
// gamepad mapping), OR'd together. The runtime's input system doesn't exist
// yet, so this talks to SDL directly.
class MenuPads {
 public:
  MenuPads() { initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD); }
  // Shut down fully: the SDK's input driver only learns about controllers
  // from the SDL_EVENT_GAMEPAD_ADDED events a fresh init sends.
  ~MenuPads() {
    for (auto& pad : pads_) SDL_CloseJoystick(pad.joystick);
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }

  uint32_t Poll() {
    if (!initialized_) return 0;
    if (refresh_countdown_-- <= 0) {
      Refresh();
      refresh_countdown_ = 30;
    }
    SDL_UpdateJoysticks();
    using namespace rex::input;
    uint32_t actions = 0;
    for (auto& pad : pads_) {
      PadState state = Evaluate(pad.profile, pad.joystick);
      uint16_t b = state.buttons;
      if (b & X_INPUT_GAMEPAD_DPAD_UP) actions |= kUp;
      if (b & X_INPUT_GAMEPAD_DPAD_DOWN) actions |= kDown;
      if (b & X_INPUT_GAMEPAD_DPAD_LEFT) actions |= kLeft;
      if (b & X_INPUT_GAMEPAD_DPAD_RIGHT) actions |= kRight;
      if (b & X_INPUT_GAMEPAD_A) actions |= kConfirm;
      if (b & X_INPUT_GAMEPAD_B) actions |= kBack;
      if (b & X_INPUT_GAMEPAD_START) actions |= kStart;
      // A gamepad's left stick moves too. Guitars report tilt and whammy on
      // the right stick, so that one is left alone.
      if (pad.instrument == Instrument::kGamepad) {
        if (state.thumb_ly > 20000) actions |= kUp;
        if (state.thumb_ly < -20000) actions |= kDown;
        if (state.thumb_lx < -20000) actions |= kLeft;
        if (state.thumb_lx > 20000) actions |= kRight;
      }
    }
    return actions;
  }

  // Names in SDL's order, which is also the controller_type_N order.
  std::vector<std::string> Names() const {
    std::vector<std::string> names;
    for (const auto& pad : pads_) names.push_back(pad.profile.name);
    return names;
  }

 private:
  struct Pad {
    SDL_JoystickID id;
    SDL_Joystick* joystick;
    Profile profile;
    Instrument instrument;
  };

  void Refresh() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    std::vector<Pad> pads;
    for (int i = 0; i < count; ++i) {
      auto it = std::find_if(pads_.begin(), pads_.end(),
                             [&](const Pad& pad) { return pad.id == ids[i]; });
      if (it != pads_.end()) {
        pads.push_back(std::move(*it));
        pads_.erase(it);
        continue;
      }
      SDL_Joystick* joystick = SDL_OpenJoystick(ids[i]);
      if (!joystick) continue;
      Pad pad{ids[i], joystick, DefaultProfile(ids[i]), Instrument::kGamepad};
      std::string guid = pad.profile.guid;
      if (auto profile = BindingStore::Get().Find(guid)) {
        std::string name = pad.profile.name;
        pad.profile = *profile;
        if (pad.profile.name.empty()) pad.profile.name = name;
      }
      switch (SDL_GetJoystickTypeForID(ids[i])) {
        case SDL_JOYSTICK_TYPE_GUITAR:
          pad.instrument = Instrument::kGuitar;
          break;
        case SDL_JOYSTICK_TYPE_DRUM_KIT:
          pad.instrument = Instrument::kDrums;
          break;
        default:
          break;
      }
      pads.push_back(std::move(pad));
    }
    SDL_free(ids);
    for (auto& gone : pads_) SDL_CloseJoystick(gone.joystick);
    pads_ = std::move(pads);
  }

  bool initialized_ = false;
  int refresh_countdown_ = 0;
  std::vector<Pad> pads_;
};

// Turns held actions into presses, with key-repeat for movement.
class Repeater {
 public:
  uint32_t Update(uint32_t held) {
    constexpr uint32_t kRepeating = kUp | kDown | kLeft | kRight;
    auto now = Clock::now();
    uint32_t pressed = held & ~held_;
    for (int bit = 0; bit < 32; ++bit) {
      uint32_t mask = 1u << bit;
      if (pressed & mask) next_repeat_[bit] = now + std::chrono::milliseconds(380);
      if ((held & held_ & kRepeating & mask) && now >= next_repeat_[bit]) {
        pressed |= mask;
        next_repeat_[bit] = now + std::chrono::milliseconds(90);
      }
    }
    held_ = held;
    return pressed;
  }

 private:
  uint32_t held_ = ~0u;  // ignore whatever is already held when the screen opens
  std::array<Clock::time_point, 32> next_repeat_{};
};

constexpr const char* kControllerTypes[] = {"auto", "guitar", "drums", "gamepad", "none"};
constexpr const char* kControllerTypeLabels[] = {"AUTO", "GUITAR", "DRUMS", "GAMEPAD / VOCALS",
                                               "NONE (IGNORED)"};

// Polled by the screen. Shared with the worker, which may outlive the screen.
struct DeluxeInstallJob {
  std::mutex mutex;
  std::optional<std::filesystem::path> picked;  // from the file picker
  bool running = false;
  std::atomic<float> progress{0};
  std::optional<std::string> result;  // "" on success, else the error
};

enum class Row {
  kPlay,
  kProfile,
  kDeluxe,
  kDeluxeInstall,
  kDisplayMode,
  kResolution,
  kFrameLimit,
  kFpsCounter,
  kAudioBuffer,
  kAudioDevice,
  kQuickStart,
  kController1,
  kController2,
  kController3,
  kController4,
  kMouseKeyboard,
  kShowLauncher,
  kQuit,
  kCount,
};

// The top level lists PLAY, these categories and QUIT; a category opens its
// settings (green / A / Enter), and red / B / Escape goes back.
enum class Section { kDeluxe, kVideo, kAudio, kControllers, kGame, kCount };

std::vector<Row> SectionRows(Section section) {
  switch (section) {
    case Section::kDeluxe: return {Row::kDeluxe, Row::kDeluxeInstall};
    case Section::kVideo:
      return {Row::kDisplayMode, Row::kResolution, Row::kFrameLimit, Row::kFpsCounter};
    case Section::kAudio: return {Row::kAudioBuffer, Row::kAudioDevice};
    case Section::kControllers:
      return {Row::kController1, Row::kController2, Row::kController3, Row::kController4,
              Row::kMouseKeyboard};
    case Section::kGame: return {Row::kProfile, Row::kQuickStart, Row::kShowLauncher};
    default: return {};
  }
}

const char* SectionLabel(Section section) {
  switch (section) {
    case Section::kDeluxe: return "ROCK BAND 3 DELUXE";
    case Section::kVideo: return "VIDEO";
    case Section::kAudio: return "AUDIO";
    case Section::kControllers: return "CONTROLLERS";
    case Section::kGame: return "GAME";
    default: return "";
  }
}

struct Item {
  enum Kind { kPlay, kSection, kSetting, kBack, kQuit } kind;
  Section section = Section::kCount;
  Row row = Row::kCount;
};

// Rows that fit between the logo and the help bar (a category's header, its
// settings and BACK at most).
constexpr int kMaxVisibleRows = 9;

class Launcher final : public rex::ui::ImGuiDialog {
 public:
  Launcher(rex::ui::ImGuiDrawer* drawer,
           std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
           std::filesystem::path game_root, LauncherHooks hooks)
      : ImGuiDialog(drawer),
        ui_(std::move(immediate_drawer), std::move(game_root)),
        hooks_(std::move(hooks)) {
    g_launcher_active = true;
  }
  ~Launcher() override { g_launcher_active = false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (finished_) return;
    ui_.Update();
    HandleInput(io);
    if (finished_) return;

    ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("##rb3pc_launcher", nullptr, flags)) {
      Draw(ImGui::GetWindowDrawList(), display);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (hooks_.request_paint) hooks_.request_paint();
  }

 private:

  bool InSection() const { return section_ != Section::kCount; }

  std::vector<Item> Items() const {
    std::vector<Item> items;
    if (!InSection()) {
      items.push_back({Item::kPlay});
      for (int s = 0; s < int(Section::kCount); ++s) items.push_back({Item::kSection, Section(s)});
      items.push_back({Item::kQuit});
    } else {
      for (Row row : SectionRows(section_)) items.push_back({Item::kSetting, section_, row});
      items.push_back({Item::kBack});
    }
    return items;
  }

  Item Selected() const {
    auto items = Items();
    return items[std::clamp(selected_, 0, int(items.size()) - 1)];
  }

  Row SelectedRow() const {
    Item item = Selected();
    return item.kind == Item::kSetting ? item.row : Row::kCount;
  }

  void EnterSection(Section section) {
    section_ = section;
    selected_ = 0;
  }

  void LeaveSection() {
    if (!InSection()) return;
    selected_ = 1 + int(section_);  // back on the category's own line
    section_ = Section::kCount;
  }

  std::string SectionSummary(Section section) const {
    switch (section) {
      case Section::kDeluxe: return Value(Row::kDeluxe);
      case Section::kVideo: {
        std::string scale = kScaleLabels[ScaleIndex()];  // "2x (2560x1440)"
        int limit = kLimits[LimitIndex()];
        return Value(Row::kDisplayMode) + ", " + Upper(scale.substr(0, scale.find(' '))) + ", " +
               (limit ? std::to_string(limit) + " FPS" : "UNCAPPED");
      }
      case Section::kAudio: {
        // At 48 kHz.
        int samples = kAudioQueueFrames[AudioQueueIndex()] * 256;
        int device = kAudioDeviceFrames[AudioDeviceIndex()];
        std::string queue_ms = std::to_string(int(samples / 48.0 + 0.5)) + " MS";
        if (!device) return queue_ms + " + SYSTEM BUFFER";
        return std::to_string(int((samples + device) / 48.0 + 0.5)) + " MS";
      }
      case Section::kControllers: {
        size_t n = pads_.Names().size();
        return n == 1 ? "1 CONNECTED" : std::to_string(n) + " CONNECTED";
      }
      case Section::kGame: return Value(Row::kProfile);
      default: return "";
    }
  }

  void Save() {
    if (!hooks_.config_path.empty()) rex::cvar::SaveConfig(hooks_.config_path);
  }

  static int ControllerIndex(int slot) {
    std::string value = rex::cvar::GetFlagByName(ControllerCvar(slot).c_str());
    for (int i = 0; i < int(std::size(kControllerTypes)); ++i) {
      if (value == kControllerTypes[i]) return i;
    }
    return 0;
  }
  static std::string ControllerCvar(int slot) {
    return "controller_type_" + std::to_string(slot + 1);
  }

  // 0 = not a choice row.
  int ChoiceCount(Row row) const {
    switch (row) {
      case Row::kDeluxe:
        return DeluxeInstalled() && DeluxeExecutableAvailable() ? 2 : 0;
      case Row::kQuickStart:
      case Row::kMouseKeyboard:
      case Row::kDisplayMode:
      case Row::kFpsCounter:
      case Row::kShowLauncher:
        return 2;
      case Row::kResolution:
        return kScaleCount;
      case Row::kFrameLimit:
        return kLimitCount;
      case Row::kAudioBuffer:
        return kAudioQueueCount;
      case Row::kAudioDevice:
        return kAudioDeviceCount;
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        return int(std::size(kControllerTypes));
      default:
        return 0;
    }
  }

  int ChoiceIndex(Row row) const {
    switch (row) {
      case Row::kDeluxe:
        return DeluxeEnabled() ? 1 : 0;
      case Row::kDisplayMode:
        return hooks_.is_fullscreen() ? 1 : 0;
      case Row::kFpsCounter:
        return GetBoolFlag("show_fps") ? 1 : 0;
      case Row::kQuickStart:
        return GetBoolFlag("quick_start") ? 1 : 0;
      case Row::kMouseKeyboard:
        return MouseKeyboardEnabled() ? 1 : 0;
      case Row::kShowLauncher:
        return GetBoolFlag("show_launcher") ? 1 : 0;
      case Row::kResolution:
        return ScaleIndex();
      case Row::kFrameLimit:
        return LimitIndex();
      case Row::kAudioBuffer:
        return AudioQueueIndex();
      case Row::kAudioDevice:
        return AudioDeviceIndex();
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        return ControllerIndex(int(row) - int(Row::kController1));
      default:
        return 0;
    }
  }

  void SetChoice(Row row, int index) {
    switch (row) {
      case Row::kDeluxe:
        SetDeluxeEnabled(index == 1);
        break;
      case Row::kDisplayMode:
        hooks_.set_fullscreen(index == 1);
        break;
      case Row::kFpsCounter:
        rex::cvar::SetFlagByName("show_fps", index ? "true" : "false");
        break;
      case Row::kQuickStart:
        rex::cvar::SetFlagByName("quick_start", index ? "true" : "false");
        break;
      case Row::kMouseKeyboard:
        SetMouseKeyboardEnabled(index == 1);
        break;
      case Row::kShowLauncher:
        rex::cvar::SetFlagByName("show_launcher", index ? "true" : "false");
        break;
      case Row::kResolution:
        SetScaleIndex(index);
        break;
      case Row::kFrameLimit:
        SetLimitIndex(index);
        break;
      case Row::kAudioBuffer:
        SetAudioQueueIndex(index);
        break;
      case Row::kAudioDevice:
        SetAudioDeviceIndex(index);
        break;
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        rex::cvar::SetFlagByName(ControllerCvar(int(row) - int(Row::kController1)).c_str(),
                                 kControllerTypes[index]);
        break;
      default:
        return;
    }
    Save();
  }

  // A setting picked from a list, adjusted in place (not an on/off box).
  bool IsListChoice(Row row) const { return ChoiceCount(row) > 0 && !IsCheckbox(row); }

  // Adjusting a list setting: green selects it, strumming changes it, green
  // or red saves it. The choice is only applied when saved.
  void StartAdjusting(Row row) {
    adjusting_ = true;
    pending_ = ChoiceIndex(row);
  }

  void FinishAdjusting() {
    if (!adjusting_) return;
    adjusting_ = false;
    Row row = SelectedRow();
    if (row != Row::kCount && pending_ != ChoiceIndex(row)) SetChoice(row, pending_);
  }

  void HandleAdjusting(uint32_t pressed) {
    Row row = SelectedRow();
    int count = ChoiceCount(row);
    if (!count) {
      adjusting_ = false;
      return;
    }
    if (pressed & (kUp | kLeft)) pending_ = (pending_ + count - 1) % count;
    if (pressed & (kDown | kRight)) pending_ = (pending_ + 1) % count;
    bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                 ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    if (pressed & (kConfirm | kBack) || click) FinishAdjusting();
    if (pressed & kStart) {
      FinishAdjusting();
      Play();
    }
  }

  void Cycle(Row row, int delta) {
    int count = ChoiceCount(row);
    if (!count) return;
    SetChoice(row, (ChoiceIndex(row) + delta + count) % count);
  }

  std::string Label(Row row) const {
    switch (row) {
      case Row::kPlay: return "PLAY";
      case Row::kProfile: return "PROFILE NAME";
      case Row::kDeluxe: return "ROCK BAND 3 DELUXE";
      case Row::kDeluxeInstall: return DeluxeInstalled() ? "UPDATE DELUXE" : "INSTALL DELUXE";
      case Row::kDisplayMode: return "DISPLAY";
      case Row::kResolution: return "INTERNAL RESOLUTION";
      case Row::kFrameLimit: return "FRAME RATE LIMIT";
      case Row::kAudioBuffer: return "AUDIO BUFFER";
      case Row::kAudioDevice: return "AUDIO DEVICE BUFFER";
      case Row::kFpsCounter: return "FPS COUNTER";
      case Row::kQuickStart: return "QUICK START (SKIP INTRO)";
      case Row::kController1: return "CONTROLLER 1";
      case Row::kController2: return "CONTROLLER 2";
      case Row::kController3: return "CONTROLLER 3";
      case Row::kController4: return "CONTROLLER 4";
      case Row::kMouseKeyboard: return "MOUSE & KEYBOARD IN MENUS";
      case Row::kShowLauncher: return "SHOW THIS SCREEN AT STARTUP";
      case Row::kQuit: return "QUIT";
      default: return "";
    }
  }

  std::string Value(Row row) const {
    int index = adjusting_ && row == SelectedRow() ? pending_ : ChoiceIndex(row);
    switch (row) {
      case Row::kProfile: {
        if (editing_) return edit_name_;
        std::string name = rex::cvar::GetFlagByName("profile_name");
        return SanitizeProfileName(name).empty() ? ProfileName() : SanitizeProfileName(name);
      }
      case Row::kDeluxe:
        if (!DeluxeExecutableAvailable()) return "NOT BUILT";
        return DeluxeInstalled() ? (index ? "ON" : "OFF") : "NOT INSTALLED";
      case Row::kDeluxeInstall: {
        std::lock_guard lock(install_->mutex);
        if (install_->running) {
          return "INSTALLING " + std::to_string(int(install_->progress * 100)) + "%";
        }
        return "CHOOSE ZIP...";
      }
      case Row::kDisplayMode: return index ? "FULLSCREEN" : "WINDOWED";
      case Row::kQuickStart:
      case Row::kFpsCounter:
      case Row::kMouseKeyboard:
      case Row::kShowLauncher: return index ? "ON" : "OFF";
      case Row::kResolution: return Upper(kScaleLabels[index]);
      case Row::kFrameLimit: return Upper(kLimitLabels[index]);
      case Row::kAudioBuffer: return Upper(kAudioQueueLabels[index]);
      case Row::kAudioDevice: return Upper(kAudioDeviceLabels[index]);
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4: return kControllerTypeLabels[index];
      default: return "";
    }
  }

  static std::string Upper(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
  }

  void HandleInput(ImGuiIO& io) {
    PollDeluxeInstall();
    uint32_t held = pads_.Poll();
    uint32_t pad_pressed = repeater_.Update(held);
    uint32_t pressed = pad_pressed;

    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (key(ImGuiKey_UpArrow)) pressed |= kUp;
    if (key(ImGuiKey_DownArrow)) pressed |= kDown;
    if (key(ImGuiKey_LeftArrow)) pressed |= kLeft;
    if (key(ImGuiKey_RightArrow)) pressed |= kRight;
    if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) pressed |= editing_ ? kStart : kConfirm;
    if (!editing_ && key(ImGuiKey_Space)) pressed |= kConfirm;
    if (key(ImGuiKey_Backspace)) pressed |= kBack;
    if (editing_ && key(ImGuiKey_Escape)) {
      editing_ = false;  // cancel, keeping the old name
      return;
    }
    if (key(ImGuiKey_Escape)) pressed |= kBack;

    if (editing_) {
      HandleEditing(pressed & ~pad_pressed);
      return;
    }
    if (adjusting_) {
      HandleAdjusting(pressed);
      return;
    }

    if (SelectedRow() == Row::kProfile) {
      std::string typed = TypedText();
      // Space selects, as on every other row.
      if (typed.find_first_not_of(' ') != std::string::npos) {
        editing_ = true;
        edit_name_.clear();
        AppendTyped(typed);
        return;
      }
    }

    bool mouse_moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    int hovered = HoveredRow(io.MousePos);
    if (hovered >= 0 && mouse_moved) selected_ = hovered;
    if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      selected_ = hovered;
      pressed |= kConfirm;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) pressed |= kBack;

    int count = int(Items().size());
    if (pressed & kUp) selected_ = (selected_ + count - 1) % count;
    if (pressed & kDown) selected_ = (selected_ + 1) % count;
    Item item = Selected();
    if (item.kind == Item::kSetting) {
      if (pressed & kLeft) Cycle(item.row, -1);
      if (pressed & kRight) Cycle(item.row, +1);
    }
    if (pressed & kBack) {
      LeaveSection();
      return;
    }
    if (pressed & kConfirm) {
      switch (item.kind) {
        case Item::kPlay: Play(); return;
        case Item::kQuit: Finish(hooks_.quit); return;
        case Item::kSection: EnterSection(item.section); return;
        case Item::kBack: LeaveSection(); return;
        case Item::kSetting:
          if (item.row == Row::kProfile) {
            // Typed only: a controller's green doesn't start editing (Enter,
            // Space or a click does).
            if (!(pad_pressed & kConfirm)) StartEditing();
          } else if (item.row == Row::kDeluxeInstall) {
            PickDeluxeZip();
          } else if (IsListChoice(item.row)) {
            StartAdjusting(item.row);
          } else {
            Cycle(item.row, +1);
          }
          break;
      }
    }
    if (pressed & kStart) Play();
  }

  void PickDeluxeZip() {
    {
      std::lock_guard lock(install_->mutex);
      if (install_->running) return;
    }
    static const SDL_DialogFileFilter kFilters[] = {{"Rock Band 3 Deluxe (Xbox 360 zip)", "zip"}};
    auto* job = new std::shared_ptr<DeluxeInstallJob>(install_);
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
          std::unique_ptr<std::shared_ptr<DeluxeInstallJob>> job(
              static_cast<std::shared_ptr<DeluxeInstallJob>*>(userdata));
          if (!files || !files[0]) return;  // cancelled, or an error
          std::lock_guard lock((*job)->mutex);
          (*job)->picked = std::filesystem::path(files[0]);
        },
        job, nullptr, kFilters, 1, nullptr, false);
  }

  void PollDeluxeInstall() {
    std::lock_guard lock(install_->mutex);
    if (install_->picked && !install_->running) {
      std::filesystem::path source = *install_->picked;
      install_->picked.reset();
      install_->running = true;
      install_->progress = 0;
      install_status_ = "";
      std::thread([job = install_, source] {
        auto error = InstallDeluxe(source, &job->progress);
        std::lock_guard lock(job->mutex);
        job->running = false;
        job->result = error.value_or("");
      }).detach();
    }
    if (install_->result) {
      install_status_ = *install_->result;
      if (install_status_.empty()) {
        SetDeluxeEnabled(true);
        Save();
      }
      install_->result.reset();
    }
  }

  // The note on the right of the install row: the last install's error, or
  // what to pick until something is installed.
  std::string DeluxeNote() const {
    if (!install_status_.empty()) return install_status_;
    return DeluxeInstalled() ? "" : "THE XBOX 360 RELEASE ZIP";
  }

  // Profile name: typed on the keyboard only (typing on the row starts a new
  // one; Enter, Space or a click edits the current one; Enter saves, Escape
  // cancels).
  void StartEditing() {
    editing_ = true;
    edit_name_ = SanitizeProfileName(rex::cvar::GetFlagByName("profile_name"));
    if (edit_name_.empty()) edit_name_ = ProfileName();
  }

  // From key presses: SDL sends no text events until text input is enabled.
  static std::string TypedText() {
    std::string out;
    // The SDK forwards Shift as a key but not as ImGui's modifier state.
    bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    for (int i = 0; i < 26; ++i) {
      if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_A + i), true)) out += char((shift ? 'A' : 'a') + i);
    }
    for (int i = 0; i < 10; ++i) {
      if (ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_0 + i), true) ||
          ImGui::IsKeyPressed(ImGuiKey(ImGuiKey_Keypad0 + i), true)) {
        out += char('0' + i);
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) out += ' ';
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, true)) out += shift ? '_' : '-';
    if (ImGui::IsKeyPressed(ImGuiKey_Period, true)) out += '.';
    return out;
  }

  void AppendTyped(const std::string& typed) {
    for (char c : typed) {
      if (edit_name_.size() < kMaxProfileNameLength) edit_name_ += c;
    }
  }

  void HandleEditing(uint32_t pressed) {
    std::string typed = TypedText();
    AppendTyped(typed);
    if ((pressed & kBack) && !edit_name_.empty()) edit_name_.pop_back();
    if (pressed & kStart) {
      editing_ = false;
      std::string name = SanitizeProfileName(edit_name_);
      // The login name is the default, so don't pin it in the config.
      rex::cvar::SetFlagByName("profile_name", "");
      if (!name.empty() && name != ProfileName()) {
        rex::cvar::SetFlagByName("profile_name", name);
      }
      Save();
    }
  }

  // Blocked during a Deluxe install; the boot would miss it.
  void Play() {
    {
      std::lock_guard lock(install_->mutex);
      if (install_->running) return;
    }
    Finish(hooks_.play);
  }

  void Finish(const std::function<void()>& action) {
    finished_ = true;
    Save();
    if (action) action();
  }

  Rb3UiRenderer ui_;
  LauncherHooks hooks_;
  MenuPads pads_;
  Repeater repeater_;
  int selected_ = 0;  // index into Items()
  Section section_ = Section::kCount;  // the open category, or kCount at the top
  bool editing_ = false;
  bool adjusting_ = false;  // a list setting is selected for changing
  int pending_ = 0;         // its choice until saved
  std::string edit_name_;
  bool finished_ = false;
  std::shared_ptr<DeluxeInstallJob> install_ = std::make_shared<DeluxeInstallJob>();
  std::string install_status_;  // the last install's error, if it failed
};

}  // namespace

}  // namespace rb3pc
