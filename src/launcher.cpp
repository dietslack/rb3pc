#include "launcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/input/input.h>
#include <rex/ui/imgui_dialog.h>

#include "audio_options.h"
#include "custom_songs.h"
#include "deluxe.h"
#include "display_options.h"
#include "display_size.h"
#include "input_bindings.h"
#include "mic_input.h"
#include "instrument_input.h"
#include "menu_pads.h"
#include "midi_input.h"
#include "mouse_cursor.h"
#include "profile.h"
#include "rb3_ui.h"
#include "text_entry.h"

REXCVAR_DEFINE_BOOL(show_launcher, true, "rb3pc",
                    "Show the settings screen (profile, display, controllers) before the game "
                    "starts");

REXCVAR_DECLARE(std::string, debug_input_script);
REXCVAR_DEFINE_STRING(debug_launcher_script, "", "rb3pc",
                      "Testing aid: menu actions on this screen at set times, e.g. "
                      "\"3:down,4:confirm\" (seconds after it opens; up down left right confirm "
                      "back start)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

bool g_launcher_active = false;

using Clock = std::chrono::steady_clock;

constexpr const char* kControllerTypes[] = {"auto", "guitar", "drums", "gamepad", "none"};
constexpr const char* kMouseCursorLabels[] = {"HIDE WHEN IDLE", "ALWAYS SHOW", "ALWAYS HIDE"};
static_assert(std::size(kMouseCursorLabels) == std::size(rb3pc::kMouseCursorModes));

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
  kDeluxeRb3e,
  kDeluxeInstall,
  kSongsDefault,
  kSongsExtra1,
  kSongsExtra2,
  kSongsExtra3,
  kSongsExtra4,
  kSongsExtra5,
  kSongsAdd,
  kDisplayMode,
  kResolution,
  kAspectRatio,
  kFrameLimit,
  kFpsCounter,
  kAudioBuffer,
  kAudioDevice,
  kQuickStart,
  kController1,
  kController2,
  kController3,
  kController4,
  kBindControllers,
  kMouseKeyboard,
  kMouseCursor,
  kShowLauncher,
  kQuit,
  kCount,
};

enum class Section { kDeluxe, kSongs, kVideo, kAudio, kControllers, kGame, kCount };

static_assert(int(Row::kSongsAdd) - int(Row::kSongsExtra1) == int(rb3pc::kMaxExtraSongsFolders));

std::vector<Row> SectionRows(Section section, size_t extra_songs_folders) {
  switch (section) {
    case Section::kDeluxe: return {Row::kDeluxe, Row::kDeluxeRb3e, Row::kDeluxeInstall};
    case Section::kSongs: {
      std::vector<Row> rows = {Row::kSongsDefault};
      for (size_t i = 0; i < extra_songs_folders; ++i) {
        rows.push_back(Row(int(Row::kSongsExtra1) + int(i)));
      }
      if (extra_songs_folders < rb3pc::kMaxExtraSongsFolders) rows.push_back(Row::kSongsAdd);
      return rows;
    }
    case Section::kVideo:
      return {Row::kDisplayMode, Row::kResolution, Row::kAspectRatio, Row::kFrameLimit,
              Row::kFpsCounter};
    case Section::kAudio: return {Row::kAudioBuffer, Row::kAudioDevice};
    case Section::kControllers:
      return {Row::kController1, Row::kController2, Row::kController3, Row::kController4,
              Row::kBindControllers, Row::kMouseKeyboard, Row::kMouseCursor};
    case Section::kGame: return {Row::kProfile, Row::kQuickStart, Row::kShowLauncher};
    default: return {};
  }
}

const char* SectionLabel(Section section) {
  switch (section) {
    case Section::kDeluxe: return "ROCK BAND 3 DELUXE";
    case Section::kSongs: return "CUSTOM SONGS";
    case Section::kVideo: return "VIDEO";
    case Section::kAudio: return "AUDIO";
    case Section::kControllers: return "CONTROLLERS";
    case Section::kGame: return "GAME";
    default: return "";
  }
}

struct Item {
  // kDevice and kBind*: the BIND CONTROLLERS screens.
  enum Kind { kPlay, kSection, kSetting, kBack, kQuit, kDevice, kBindType, kBindMic, kBindInput, kBindReset } kind;
  Section section = Section::kCount;
  Row row = Row::kCount;
  int index = 0;  // kDevice: which device; kBindMic: which microphone; kBindInput: which input
};

enum class BindScreen { kNone, kDevices, kDevice };

struct BindDevice {
  bool midi = false;
  size_t slot = 0;          // a controller's place in SDL's order (controller_type_N)
  std::string key;          // "sdl:<SDL id>" or "midi:<MidiDevice::key>"
  std::string name;
};

// Strips the prefix every PS3 instrument's name has.
std::string ShortPadName(std::string name) {
  constexpr std::string_view kLicensed = "Licensed by Sony Computer Entertainment ";
  if (name.starts_with(kLicensed)) name.erase(0, kLicensed.size());
  if (name.starts_with("America ")) name.erase(0, 8);
  return name;
}

// Between the logo and the help bar.
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
    MidiInput::Get().Start();  // MIDI instruments show up here too
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
    DrawNameEntry(io);
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
    } else if (bind_ == BindScreen::kDevices) {
      auto devices = BindDevices();
      for (int i = 0; i < int(devices.size()); ++i) items.push_back({Item::kDevice, section_, Row::kCount, i});
      items.push_back({Item::kBack});
    } else if (bind_ == BindScreen::kDevice) {
      items.push_back({Item::kBindType, section_});
      if (auto device = CurrentDevice(); device && !BindTypeIsNone(*device)) {
        if (HasMics(*device)) {
          for (int i = 0; i < kMicCount; ++i) items.push_back({Item::kBindMic, section_, Row::kCount, i});
        }
        int inputs = device->midi ? int(kDrumInputCount) : int(kOutputCount);
        for (int i = 0; i < inputs; ++i) items.push_back({Item::kBindInput, section_, Row::kCount, i});
        items.push_back({Item::kBindReset, section_});
      }
      items.push_back({Item::kBack});
    } else {
      for (Row row : SectionRows(section_, songs_folders_.size())) items.push_back({Item::kSetting, section_, row});
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
    bind_ = BindScreen::kNone;
    selected_ = 0;
    scroll_ = 0;
  }

  void LeaveSection() {
    if (!InSection()) return;
    StopCapture();
    scroll_ = 0;
    if (bind_ == BindScreen::kDevice) {
      bind_ = BindScreen::kDevices;
      auto devices = BindDevices();
      selected_ = 0;
      for (int i = 0; i < int(devices.size()); ++i) {
        if (devices[i].key == bind_key_) selected_ = i;
      }
      return;
    }
    if (bind_ == BindScreen::kDevices) {
      bind_ = BindScreen::kNone;
      auto rows = SectionRows(section_, songs_folders_.size());
      selected_ = int(std::find(rows.begin(), rows.end(), Row::kBindControllers) - rows.begin());
      return;
    }
    selected_ = 1 + int(section_);  // back on the category's own line
    section_ = Section::kCount;
  }

  std::string SectionSummary(Section section) const {
    switch (section) {
      case Section::kDeluxe: return Value(Row::kDeluxe);
      case Section::kSongs:
        return songs_count_ == 1 ? "1 PACKAGE" : std::to_string(songs_count_) + " PACKAGES";
      case Section::kVideo: {
        int limit = kLimits[LimitIndex()];
        return Value(Row::kDisplayMode) + ", " +
               (hooks_.is_fullscreen() ? ScaleShortLabel(WantedResolutionScale(true)) + ", " +
                                             AspectLabel(ViewShape(true)) + ", "
                                       : std::string()) +
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
        size_t n = BindDevices().size();
        return n == 1 ? "1 CONNECTED" : std::to_string(n) + " CONNECTED";
      }
      case Section::kGame: return Value(Row::kProfile);
      default: return "";
    }
  }

  void Save() {
    if (!hooks_.config_path.empty()) rex::cvar::SaveConfig(hooks_.config_path);
  }

  static int MouseCursorIndex() {
    std::string mode = MouseCursorMode();
    for (int i = 0; i < int(std::size(kMouseCursorModes)); ++i) {
      if (mode == kMouseCursorModes[i]) return i;
    }
    return 0;
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
      case Row::kDeluxeRb3e:
        return DeluxeInstalled() && DeluxeExecutableAvailable() ? 2 : 0;
      case Row::kQuickStart:
      case Row::kMouseKeyboard:
      case Row::kDisplayMode:
      case Row::kFpsCounter:
      case Row::kShowLauncher:
        return 2;
      case Row::kAspectRatio:
        // Windowed, it follows the window: nothing to pick.
        return hooks_.is_fullscreen() ? kAspectChoiceCount : 0;
      case Row::kResolution:
        return hooks_.is_fullscreen() ? kScaleCount + 1 : 0;
      case Row::kFrameLimit:
        return kLimitCount;
      case Row::kAudioBuffer:
        return kAudioQueueCount;
      case Row::kAudioDevice:
        return kAudioDeviceCount;
      case Row::kMouseCursor:
        return int(std::size(kMouseCursorModes));
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        return MidiInPort(row) ? 2 : int(std::size(kControllerTypes));
      default:
        return 0;
    }
  }

  int ChoiceIndex(Row row) const {
    switch (row) {
      case Row::kDeluxe:
        return DeluxeEnabled() ? 1 : 0;
      case Row::kDeluxeRb3e:
        return GetBoolFlag("deluxe_rb3e_functions") ? 1 : 0;
      case Row::kDisplayMode:
        return hooks_.is_fullscreen() ? 1 : 0;
      case Row::kAspectRatio:
        return AspectRatioSetting();
      case Row::kFpsCounter:
        return GetBoolFlag("show_fps") ? 1 : 0;
      case Row::kQuickStart:
        return GetBoolFlag("quick_start") ? 1 : 0;
      case Row::kMouseKeyboard:
        return MouseKeyboardEnabled() ? 1 : 0;
      case Row::kShowLauncher:
        return GetBoolFlag("show_launcher") ? 1 : 0;
      case Row::kResolution:
        return FullscreenResolutionSetting();  // 0 = match the display
      case Row::kFrameLimit:
        return LimitIndex();
      case Row::kAudioBuffer:
        return AudioQueueIndex();
      case Row::kAudioDevice:
        return AudioDeviceIndex();
      case Row::kMouseCursor:
        return MouseCursorIndex();
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        if (auto midi = MidiInPort(row)) return BindTypeIsNone(*midi) ? 1 : 0;
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
      case Row::kDeluxeRb3e:
        rex::cvar::SetFlagByName("deluxe_rb3e_functions", index ? "true" : "false");
        break;
      case Row::kDisplayMode:
        hooks_.set_fullscreen(index == 1);
        break;
      case Row::kAspectRatio:
        SetAspectRatioSetting(index);
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
        SetFullscreenResolutionSetting(index);
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
      case Row::kMouseCursor:
        SetMouseCursorMode(kMouseCursorModes[index]);
        break;
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        if (auto midi = MidiInPort(row)) {
          MidiProfile profile = MidiInput::Get().Profile(MidiKey(*midi));
          profile.type = index ? MidiType::kNone : MidiType::kProDrums;
          MidiInput::Get().SaveProfile(profile);
          return;
        }
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
      case Row::kDeluxeRb3e: return "RB3ENHANCED FEATURES";
      case Row::kDeluxeInstall: return DeluxeInstalled() ? "UPDATE DELUXE" : "INSTALL DELUXE";
      case Row::kSongsDefault: return "SONGS FOLDER";
      case Row::kSongsExtra1:
      case Row::kSongsExtra2:
      case Row::kSongsExtra3:
      case Row::kSongsExtra4:
      case Row::kSongsExtra5: return "FOLDER " + std::to_string(int(row) - int(Row::kSongsExtra1) + 2);
      case Row::kSongsAdd: return "ADD A FOLDER";
      case Row::kDisplayMode: return "DISPLAY";
      case Row::kResolution: return "INTERNAL RESOLUTION";
      case Row::kFrameLimit: return "FRAME RATE LIMIT";
      case Row::kAudioBuffer: return "AUDIO BUFFER";
      case Row::kAudioDevice: return "AUDIO DEVICE BUFFER";
      case Row::kAspectRatio: return "ASPECT RATIO";
      case Row::kFpsCounter: return "FPS COUNTER";
      case Row::kQuickStart: return "QUICK START (SKIP INTRO)";
      case Row::kController1: return "CONTROLLER 1";
      case Row::kController2: return "CONTROLLER 2";
      case Row::kController3: return "CONTROLLER 3";
      case Row::kController4: return "CONTROLLER 4";
      case Row::kBindControllers: return "BIND CONTROLLERS";
      case Row::kMouseKeyboard: return "MOUSE & KEYBOARD IN MENUS";
      case Row::kMouseCursor: return "MOUSE CURSOR";
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
      case Row::kDeluxeRb3e:
        if (!DeluxeExecutableAvailable()) return "NOT BUILT";
        return DeluxeInstalled() ? (index ? "ON" : "OFF") : "NOT INSTALLED";
      case Row::kDeluxeInstall: {
        std::lock_guard lock(install_->mutex);
        if (install_->running) {
          return "INSTALLING " + std::to_string(int(install_->progress * 100)) + "%";
        }
        return "CHOOSE ZIP...";
      }
      case Row::kSongsAdd: return songs_status_.empty() ? "CHOOSE FOLDER..." : songs_status_;
      case Row::kDisplayMode: return index ? "FULLSCREEN" : "WINDOWED";
      case Row::kAspectRatio:
        if (!hooks_.is_fullscreen()) return "";
        if (index == 0) return "MATCH DISPLAY (" + AspectLabel(ViewShape(true)) + ")";
        return Upper(kAspectChoices[index].label);
      case Row::kQuickStart:
      case Row::kFpsCounter:
      case Row::kMouseKeyboard:
      case Row::kShowLauncher: return index ? "ON" : "OFF";
      case Row::kResolution: {
        if (!hooks_.is_fullscreen()) return "";
        if (index == 0) {
          uint32_t w = 0, h = 0;
          OutputPixelSize(true, w, h);
          return "MATCH DISPLAY (" + ScaleShortLabel(ScaleForOutput(true, w, h)) + ")";
        }
        return Upper(ScaleLabel({kScales[index - 1], kScales[index - 1]}));
      }
      case Row::kFrameLimit: return Upper(kLimitLabels[index]);
      case Row::kAudioBuffer: return Upper(kAudioQueueLabels[index]);
      case Row::kAudioDevice: return Upper(kAudioDeviceLabels[index]);
      case Row::kMouseCursor: return kMouseCursorLabels[index];
      case Row::kController1:
      case Row::kController2:
      case Row::kController3:
      case Row::kController4:
        if (MidiInPort(row)) return index ? kControllerTypeLabels[std::size(kControllerTypes) - 1] : "PRO DRUMS";
        return kControllerTypeLabels[index];
      default: return "";
    }
  }

  static std::string Upper(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
  }

  void HandleInput(ImGuiIO& io) {
    PollDeluxeInstall();
    PollSongsFolder();
    if (SelectedRow() != songs_remove_armed_) songs_remove_armed_ = Row::kCount;
    if (bind_ == BindScreen::kDevice && !CurrentDevice()) LeaveSection();
    uint32_t held = pads_.Poll();
    uint32_t pad_pressed = repeater_.Update(held);
    // A controller's own bindings screen: its buttons only bind and light up
    // their rows there, so pressing them to test doesn't move or start
    // anything. The keyboard and mouse drive it.
    if (bind_ == BindScreen::kDevice) pad_pressed = 0;
    if (pad_pressed) NoteControllerPress();
    uint32_t pressed = pad_pressed | ScriptedActions();
    if (capturing_) {
      // Pads are being captured, so only keyboard/mouse can cancel.
      PollCapture();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
          ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        StopCapture();
      }
      return;
    }
    // Pads still polled above, so the button that closes it doesn't leak through.
    if (name_entry_) return;

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
    if (InSection() && io.MouseWheel != 0) {
      selected_ = std::clamp(selected_ + (io.MouseWheel < 0 ? 1 : -1), 0, count - 1);
    }
    KeepSelectionVisible();
    Item item = Selected();
    if (item.kind == Item::kSetting) {
      if (pressed & kLeft) Cycle(item.row, -1);
      if (pressed & kRight) Cycle(item.row, +1);
    }
    if (item.kind == Item::kBindType) {
      if (pressed & kLeft) CycleBindType(-1);
      if (pressed & kRight) CycleBindType(+1);
    }
    if (item.kind == Item::kBindMic) {
      if (pressed & kLeft) CycleMic(item.index, -1);
      if (pressed & kRight) CycleMic(item.index, +1);
    }
    if (item.kind == Item::kBindInput) {
      auto device = CurrentDevice();
      if (pressed & kLeft) ClearBinding(item.index);
      if ((pressed & kRight) && device && device->midi) StartCapture(item.index, true);
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
        case Item::kDevice: {
          auto devices = BindDevices();
          if (item.index < int(devices.size())) {
            bind_key_ = devices[size_t(item.index)].key;
            bind_ = BindScreen::kDevice;
            selected_ = 0;
            scroll_ = 0;
          }
          return;
        }
        case Item::kBindType: CycleBindType(+1); return;
        case Item::kBindMic: CycleMic(item.index, +1); return;
        case Item::kBindInput: StartCapture(item.index, false); return;
        case Item::kBindReset: ResetBindings(); return;
        case Item::kSetting:
          if (item.row == Row::kProfile) {
            // Pads get the game's text entry; keyboard/mouse edit in place.
            if (pad_pressed & kConfirm) {
              OpenNameEntry();
            } else {
              StartEditing();
            }
          } else if (item.row == Row::kDeluxeInstall) {
            PickDeluxeZip();
          } else if (item.row == Row::kSongsDefault) {
            OpenSongsFolder();
          } else if (IsExtraSongsFolder(item.row)) {
            RemoveSongsFolder(item.row);
          } else if (item.row == Row::kSongsAdd) {
            PickSongsFolder();
          } else if (item.row == Row::kBindControllers) {
            bind_ = BindScreen::kDevices;
            selected_ = 0;
            scroll_ = 0;
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

  // debug_launcher_script: the actions due since the last frame.
  uint32_t ScriptedActions() {
    static const auto start = Clock::now();
    static const std::vector<std::pair<double, uint32_t>> script = [] {
      static const std::pair<const char*, uint32_t> kActions[] = {
          {"up", kUp},       {"down", kDown}, {"left", kLeft},   {"right", kRight},
          {"confirm", kConfirm}, {"back", kBack}, {"start", kStart}};
      std::vector<std::pair<double, uint32_t>> out;
      std::string text = REXCVAR_GET(debug_launcher_script);
      size_t pos = 0;
      while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? text.size() : comma + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        for (const auto& [name, action] : kActions) {
          if (item.substr(colon + 1) == name) out.push_back({std::atof(item.substr(0, colon).c_str()), action});
        }
      }
      return out;
    }();
    double now = std::chrono::duration<double>(Clock::now() - start).count();
    uint32_t actions = 0;
    for (const auto& [at, action] : script) {
      if (at > script_done_ && at <= now) actions |= action;
    }
    script_done_ = now;
    return actions;
  }
  double script_done_ = 0;

  std::vector<BindDevice> BindDevices() const {
    std::vector<BindDevice> devices;
    const auto& pads = pads_.Pads();
    for (size_t i = 0; i < pads.size(); ++i) {
      devices.push_back({false, i, "sdl:" + std::to_string(pads[i].id), ShortPadName(pads[i].profile.name)});
    }
    for (const auto& midi : MidiInput::Get().Devices()) {
      if (midi.connected) devices.push_back({true, 0, "midi:" + midi.key, "MIDI: " + midi.name});
    }
    return devices;
  }

  // MIDI instruments follow the controllers, in player slot order.
  std::optional<BindDevice> MidiInPort(Row row) const {
    size_t slot = size_t(int(row) - int(Row::kController1));
    auto devices = BindDevices();
    if (slot >= devices.size() || !devices[slot].midi) return std::nullopt;
    return devices[slot];
  }

  std::optional<BindDevice> CurrentDevice() const {
    for (auto& device : BindDevices()) {
      if (device.key == bind_key_) return device;
    }
    return std::nullopt;
  }

  static std::string MidiKey(const BindDevice& device) { return device.key.substr(5); }

  const MenuPads::Pad* PadOf(const BindDevice& device) const {
    if (device.midi || device.slot >= pads_.Pads().size()) return nullptr;
    return &pads_.Pads()[device.slot];
  }

  Instrument PadInstrument(const BindDevice& device) const {
    std::string type = device.slot < 4 ? rex::cvar::GetFlagByName(ControllerCvar(int(device.slot)).c_str())
                                       : std::string("auto");
    if (type == "guitar") return Instrument::kGuitar;
    if (type == "drums") return Instrument::kDrums;
    if (type == "auto") {
      if (const MenuPads::Pad* pad = PadOf(device)) return pad->instrument;
    }
    return Instrument::kGamepad;
  }

  bool BindTypeIsNone(const BindDevice& device) const {
    if (device.midi) return MidiInput::Get().Profile(MidiKey(device)).type == MidiType::kNone;
    return device.slot < 4 && ControllerIndex(int(device.slot)) == int(std::size(kControllerTypes)) - 1;
  }

  std::string BindTypeLabel(const BindDevice& device) const {
    if (device.midi) return Upper(MidiTypeLabel(MidiInput::Get().Profile(MidiKey(device)).type));
    return kControllerTypeLabels[device.slot < 4 ? ControllerIndex(int(device.slot)) : 0];
  }

  void CycleBindType(int delta) {
    auto device = CurrentDevice();
    if (!device) return;
    if (device->midi) {
      MidiProfile profile = MidiInput::Get().Profile(MidiKey(*device));
      profile.type = profile.type == MidiType::kNone ? MidiType::kProDrums : MidiType::kNone;
      MidiInput::Get().SaveProfile(profile);
    } else if (device->slot < 4) {
      int count = int(std::size(kControllerTypes));
      int index = (ControllerIndex(int(device->slot)) + delta + count) % count;
      rex::cvar::SetFlagByName(ControllerCvar(int(device->slot)).c_str(), kControllerTypes[index]);
      Save();
    }
  }

  Profile PadProfile(const MenuPads::Pad& pad) const {
    Profile profile = DefaultProfile(pad.id);
    if (auto saved = BindingStore::Get().Find(profile.guid)) {
      std::string name = profile.name;
      profile = *saved;
      if (profile.name.empty()) profile.name = name;
    }
    return profile;
  }

  // `add` keeps a MIDI input's other notes.
  void StartCapture(int input, bool add) {
    auto device = CurrentDevice();
    if (!device) return;
    capture_input_ = input;
    capture_add_ = add;
    capture_start_ = Clock::now();
    if (device->midi) {
      auto last = MidiInput::Get().LastHit(MidiKey(*device));
      capture_serial_ = last ? last->serial : 0;
    } else if (const MenuPads::Pad* pad = PadOf(*device)) {
      sdl_capture_.emplace(pad->joystick, Output(input), PadInstrument(*device));
    } else {
      return;
    }
    capturing_ = true;
  }

  void StopCapture() {
    capturing_ = false;
    sdl_capture_.reset();
  }

  void PollCapture() {
    auto device = CurrentDevice();
    if (!device || Clock::now() - capture_start_ > std::chrono::seconds(10)) {
      StopCapture();
      return;
    }
    if (device->midi) {
      auto last = MidiInput::Get().LastHit(MidiKey(*device));
      if (!last || last->serial == capture_serial_) return;
      MidiProfile profile = MidiInput::Get().Profile(MidiKey(*device));
      auto& notes = profile.notes[size_t(capture_input_)];
      if (!capture_add_) notes.clear();
      if (std::find(notes.begin(), notes.end(), last->note) == notes.end()) notes.push_back(last->note);
      MidiInput::Get().SaveProfile(profile);
      StopCapture();
      return;
    }
    const MenuPads::Pad* pad = PadOf(*device);
    if (!pad || !sdl_capture_) {
      StopCapture();
      return;
    }
    if (auto source = sdl_capture_->Poll(pad->joystick)) {
      Profile profile = PadProfile(*pad);
      profile.sources[size_t(capture_input_)] = *source;
      BindingStore::Get().Save(profile);
      pads_.ReloadProfiles();
      StopCapture();
    }
  }

  void ClearBinding(int input) {
    auto device = CurrentDevice();
    if (!device) return;
    if (device->midi) {
      MidiProfile profile = MidiInput::Get().Profile(MidiKey(*device));
      profile.notes[size_t(input)].clear();
      MidiInput::Get().SaveProfile(profile);
    } else if (const MenuPads::Pad* pad = PadOf(*device)) {
      Profile profile = PadProfile(*pad);
      profile.sources[size_t(input)] = {};
      BindingStore::Get().Save(profile);
      pads_.ReloadProfiles();
    }
  }

  // Back to the General MIDI map (keeping the type), or SDL's own mapping.
  void ResetBindings() {
    auto device = CurrentDevice();
    if (!device) return;
    if (device->midi) {
      MidiProfile old = MidiInput::Get().Profile(MidiKey(*device));
      MidiProfile profile = DefaultMidiProfile(old.key, old.name);
      profile.type = old.type;
      MidiInput::Get().SaveProfile(profile);
    } else if (const MenuPads::Pad* pad = PadOf(*device)) {
      BindingStore::Get().Remove(pad->profile.guid);
      pads_.ReloadProfiles();
    }
  }

  // Mics are global (the game doesn't tie them to controllers); shown under
  // any gamepad / vocals controller.
  bool HasMics(const BindDevice& device) const {
    return !device.midi && PadInstrument(device) == Instrument::kGamepad;
  }

  // Skips devices another mic has.
  void CycleMic(int mic, int delta) {
    std::vector<std::string> choices = {""};
    for (const std::string& name : MicDeviceNames()) {
      bool taken = false;
      for (int other = 0; other < kMicCount; ++other) taken |= other != mic && MicSetting(other) == name;
      if (!taken) choices.push_back(name);
    }
    int count = int(choices.size());
    int at = int(std::find(choices.begin(), choices.end(), MicSetting(mic)) - choices.begin());
    at = at >= count ? 0 : (at + delta + count) % count;
    SetMicSetting(mic, choices[size_t(at)]);
    Save();
  }

  static std::string MicValue(int mic) {
    std::string device = MicSetting(mic);
    if (device.empty()) return "NONE";
    return Upper(device) + (MicOpen(mic) ? "" : " (UNPLUGGED)");
  }

  std::string BindInputLabel(const BindDevice& device, int input) const {
    if (device.midi) return Upper(DrumInputLabel(DrumInput(input)));
    return Upper(OutputLabel(Output(input), PadInstrument(device)));
  }

  std::string BindInputValue(const BindDevice& device, int input) const {
    if (capturing_ && capture_input_ == input) return device.midi ? "HIT A PAD..." : "PRESS A CONTROL...";
    if (device.midi) {
      MidiProfile profile = MidiInput::Get().Profile(MidiKey(device));
      const auto& notes = profile.notes[size_t(input)];
      if (notes.empty()) return "-";
      std::string text = DescribeNotes({notes[0]});
      for (size_t i = 1; i < notes.size(); ++i) text += ", " + std::to_string(notes[i]);
      return Upper(text);
    }
    if (const MenuPads::Pad* pad = PadOf(device)) return Upper(pad->profile.sources[size_t(input)].Describe());
    return "";
  }

  // Being played now: a MIDI input hit a moment ago, a controller's button held.
  bool BindInputLive(const BindDevice& device, int input) const {
    if (device.midi) return MidiInput::Get().RecentlyHit(MidiKey(device))[size_t(input)];
    const MenuPads::Pad* pad = PadOf(device);
    if (!pad || IsAnalog(Output(input))) return false;
    return Evaluate(pad->profile, pad->joystick).buttons & OutputButton(Output(input));
  }

  int VisibleItems() const { return kMaxVisibleRows - 1; }
  void KeepSelectionVisible() {
    int count = int(Items().size());
    selected_ = std::clamp(selected_, 0, std::max(count - 1, 0));
    if (!InSection()) {
      scroll_ = 0;
      return;
    }
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + VisibleItems()) scroll_ = selected_ - VisibleItems() + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(count - VisibleItems(), 0));
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

  static bool IsExtraSongsFolder(Row row) {
    return row >= Row::kSongsExtra1 && row <= Row::kSongsExtra5;
  }

  void PickSongsFolder() {
    auto* picked = new std::shared_ptr<PickedFolder>(songs_picked_);
    std::string start = rex::path_to_utf8(
        songs_folders_.empty() ? rex::filesystem::GetUserFolder() : songs_folders_.back());
    SDL_ShowOpenFolderDialog(
        [](void* userdata, const char* const* folders, int) {
          std::unique_ptr<std::shared_ptr<PickedFolder>> picked(
              static_cast<std::shared_ptr<PickedFolder>*>(userdata));
          if (!folders || !folders[0]) return;  // cancelled, or an error
          std::lock_guard lock((*picked)->mutex);
          (*picked)->path = std::filesystem::path(folders[0]);
        },
        picked, nullptr, start.c_str(), false);
  }

  void PollSongsFolder() {
    std::optional<std::filesystem::path> path;
    {
      std::lock_guard lock(songs_picked_->mutex);
      path.swap(songs_picked_->path);
    }
    if (!path) return;
    songs_status_.clear();
    if (rex::path_to_utf8(*path).find(';') != std::string::npos) {
      songs_status_ = "NO ; IN FOLDER NAMES";
      return;
    }
    if (songs_folders_.size() >= kMaxExtraSongsFolders ||
        std::find(songs_folders_.begin(), songs_folders_.end(), *path) != songs_folders_.end()) {
      return;
    }
    songs_folders_.push_back(*path);
    SaveSongsFolders();
    if (section_ == Section::kSongs) selected_ = int(songs_folders_.size());
  }

  // Needs a second press. Files on disk stay.
  void RemoveSongsFolder(Row row) {
    if (songs_remove_armed_ != row) {
      songs_remove_armed_ = row;
      return;
    }
    songs_remove_armed_ = Row::kCount;
    size_t index = size_t(int(row) - int(Row::kSongsExtra1));
    if (index >= songs_folders_.size()) return;
    songs_folders_.erase(songs_folders_.begin() + index);
    SaveSongsFolders();
  }

  void SaveSongsFolders() {
    SetExtraSongsFolders(songs_folders_);
    Save();
    songs_count_ = CountSongPackages(SongsFolders());
  }

  static void OpenSongsFolder() {
    std::filesystem::path folder = DefaultSongsFolder();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::string url = "file://";
    for (unsigned char c : rex::path_to_utf8(folder)) {
      if (std::isalnum(c) || std::strchr("/-._~", c)) {
        url += char(c);
      } else {
        url += '%';
        url += "0123456789ABCDEF"[c >> 4];
        url += "0123456789ABCDEF"[c & 15];
      }
    }
    SDL_OpenURL(url.c_str());
  }

  std::string SongsFolderNote(Row row) const {
    if (row == songs_remove_armed_) return "PRESS AGAIN TO REMOVE";
    std::filesystem::path folder = DefaultSongsFolder();
    if (IsExtraSongsFolder(row)) folder = songs_folders_[size_t(int(row) - int(Row::kSongsExtra1))];
    std::string text = rex::path_to_utf8(folder);
    if (text.size() > 44) text = "..." + text.substr(text.size() - 41);
    return text;
  }

  std::string DeluxeNote() const {
    if (!install_status_.empty()) return install_status_;
    return DeluxeInstalled() ? "" : "THE XBOX 360 RELEASE ZIP";
  }

  // For controllers; the keyboard edits the row in place (StartEditing).
  void OpenNameEntry() {
    std::string name = SanitizeProfileName(rex::cvar::GetFlagByName("profile_name"));
    if (name.empty()) name = ProfileName();
    // The entry draws with this screen's renderer, which outlives it.
    std::shared_ptr<Rb3UiRenderer> ui(std::shared_ptr<void>(), &ui_);
    name_entry_ = CreateTextEntry(
        ui, {"Profile name", "The name the game shows for player 1", name, kMaxProfileNameLength});
  }

  void DrawNameEntry(ImGuiIO& io) {
    if (!name_entry_ || !name_entry_->Draw(io)) return;
    if (!name_entry_->cancelled()) {
      std::string name = SanitizeProfileName(name_entry_->text());
      // The login name is the default, so don't pin it in the config.
      rex::cvar::SetFlagByName("profile_name", "");
      if (!name.empty() && name != ProfileName()) rex::cvar::SetFlagByName("profile_name", name);
      Save();
    }
    name_entry_.reset();
  }

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

  // Modelled on RB3's dialog backdrop, main menu bar and overshell checkboxes.

  struct Layout {
    float scale, cx, menu_x, value_x, menu_w, top, bottom, row_h, text;
  };

  Layout ComputeLayout(ImVec2 display) const {
    Layout l;
    l.scale = std::max(display.y / 720.0f, 0.5f);
    l.cx = display.x * 0.5f;
    l.menu_w = std::min(760.0f * l.scale, display.x * 0.92f);
    l.menu_x = l.cx - l.menu_w * 0.5f;
    l.value_x = l.menu_x + l.menu_w * 0.53f;
    l.top = 148.0f * l.scale;
    // Help bar starts 58 units from the bottom.
    float rows_bottom = 720.0f - 58.0f - 6.0f;
    l.bottom = rows_bottom * l.scale;
    float fit = (rows_bottom - 148.0f - 32.0f) / float(kMaxVisibleRows);
    l.row_h = std::min(34.0f, fit) * l.scale;
    l.text = std::min(25.0f * l.scale, l.row_h * 0.8f);
    return l;
  }

  float MenuTop(const Layout& l) const {
    int items = int(Items().size());
    int count = InSection() ? std::min(items, VisibleItems()) + 1 : items;  // + the header
    float height = count * l.row_h + 2 * 16.0f * l.scale;
    return l.top + std::max(0.0f, (l.bottom - l.top - height) * 0.5f);
  }

  float RowY(const Layout& l, int index) const {
    float gap = 16.0f * l.scale;
    float top = MenuTop(l);
    if (InSection()) {
      // Below the category's header, BACK set apart; scrolled in long lists.
      float y = top + (index - scroll_ + 1) * l.row_h + gap;
      if (index == int(Items().size()) - 1) y += gap;
      return y;
    }
    // A gap after PLAY and before QUIT.
    float y = top + index * l.row_h;
    if (index >= 1) y += gap;
    if (index == int(Items().size()) - 1) y += gap;
    return y;
  }

  int HoveredRow(ImVec2 mouse) const {
    Layout l = ComputeLayout(ImGui::GetIO().DisplaySize);
    if (mouse.x < l.menu_x || mouse.x > l.menu_x + l.menu_w) return -1;
    for (int i = 0; i < int(Items().size()); ++i) {
      if (!RowVisible(i)) continue;
      float y = RowY(l, i);
      if (mouse.y >= y && mouse.y < y + l.row_h) return i;
    }
    return -1;
  }

  bool RowVisible(int index) const {
    return !InSection() || (index >= scroll_ && index < scroll_ + VisibleItems());
  }

  bool IsCheckbox(Row row) const {
    return (row == Row::kFpsCounter || row == Row::kQuickStart || row == Row::kShowLauncher ||
            row == Row::kDeluxe || row == Row::kDeluxeRb3e || row == Row::kMouseKeyboard) &&
           ChoiceCount(row);
  }

  void DrawHighlight(ImDrawList* draw, const Layout& l, float x0, float x1, float y) {
    ui_.DrawAccentBar(draw, ImVec2(x0, y + 1 * l.scale), ImVec2(x1, y + l.row_h - 1 * l.scale),
                      l.scale);
  }

  void Draw(ImDrawList* draw, ImVec2 display) {
    Layout l = ComputeLayout(display);
    float logo_w = std::min(460.0f * l.scale, display.x * 0.7f);
    float logo_h = logo_w / ui_.logo_aspect();
    float logo_y = 26.0f * l.scale;
    ui_.DrawBackdrop(draw, display, l.scale, ImVec2(l.cx, logo_y + logo_h * 0.5f));
    ui_.DrawLogo(draw, ImVec2(l.cx - logo_w * 0.5f, logo_y),
                 ImVec2(l.cx + logo_w * 0.5f, logo_y + logo_h));


    std::vector<std::string> pad_names = pads_.Names();
    float pad = 14.0f * l.scale;
    KeepSelectionVisible();
    std::optional<BindDevice> bind_device = bind_ == BindScreen::kDevice ? CurrentDevice() : std::nullopt;
    if (InSection()) {
      std::string title = bind_ == BindScreen::kDevices ? "BIND CONTROLLERS"
                          : bind_device                 ? Upper(bind_device->name)
                                                        : SectionLabel(section_);
      if (title.size() > 34) title = title.substr(0, 33) + "...";
      float size = l.text * 1.3f;
      float w = ui_.TextWidth(UiFont::kHub, title, size);
      ui_.DrawTextShadowed(draw, UiFont::kHub, title, size, l.cx - w * 0.5f,
                           MenuTop(l) + (l.row_h - size) * 0.5f, kUiAccent);
    }
    std::vector<Item> items = Items();
    for (int i = 0; i < int(items.size()); ++i) {
      if (!RowVisible(i)) continue;
      const Item& item = items[i];
      Row row = item.row;
      float y = RowY(l, i);
      bool selected = i == selected_;
      float text = l.text;
      float text_y = y + (l.row_h - text) * 0.5f;

      if (item.kind == Item::kPlay || item.kind == Item::kQuit || item.kind == Item::kBack) {
        // Centred like the game's own menus, the bar hugging the word.
        float size = item.kind == Item::kPlay ? text * 1.3f : text;
        std::string label = item.kind == Item::kPlay   ? "PLAY"
                            : item.kind == Item::kQuit ? "QUIT"
                                                       : "BACK";
        float w = ui_.TextWidth(UiFont::kHub, label, size);
        float ty = y + (l.row_h - size) * 0.5f;
        if (selected) {
          DrawHighlight(draw, l, l.cx - w * 0.5f - pad, l.cx + w * 0.5f + pad, y);
          ui_.DrawText(draw, UiFont::kHub, label, size, l.cx - w * 0.5f, ty, kUiDark);
        } else {
          ui_.DrawTextShadowed(draw, UiFont::kHub, label, size, l.cx - w * 0.5f, ty, kUiText);
        }
        continue;
      }

      if (selected) DrawHighlight(draw, l, l.menu_x, l.menu_x + l.menu_w, y);
      auto label = [&](const std::string& s, float size, float x, float ty, ImU32 color) {
        if (selected) {
          ui_.DrawText(draw, UiFont::kHub, s, size, x, ty, kUiDark);
        } else {
          ui_.DrawTextShadowed(draw, UiFont::kHub, s, size, x, ty, color);
        }
      };
      if (item.kind == Item::kSection) {
        label(SectionLabel(item.section), text, l.menu_x + pad, text_y, kUiText);
        label(SectionSummary(item.section), text, l.value_x, text_y, kUiAccent);
        // The texture has a lot of empty space around it, so draw it bigger.
        float size = l.row_h * 0.95f;
        float x1 = l.menu_x + l.menu_w - pad * 0.5f;
        float cy = y + l.row_h * 0.5f;
        if (ui_.HasImage("chevron")) {
          ui_.DrawImage(draw, "chevron", ImVec2(x1 - size, cy - size * 0.5f),
                        ImVec2(x1, cy + size * 0.5f), selected ? kUiDark : kUiText);
        } else {
          float cell = text * 2.6f;
          ui_.DrawText(draw, UiFont::kButtons, "S", cell, x1 - cell * 0.65f,
                       y + (l.row_h - cell) * 0.5f, IM_COL32_WHITE);
        }
        continue;
      }
      if (item.kind == Item::kDevice || item.kind == Item::kBindType || item.kind == Item::kBindMic ||
          item.kind == Item::kBindInput || item.kind == Item::kBindReset) {
        // Fitted between value_x and the right edge.
        auto fit = [&](std::string s, float max_w) {
          while (s.size() > 4 && ui_.TextWidth(UiFont::kHub, s, text) > max_w) s = s.substr(0, s.size() - 4) + "...";
          return s;
        };
        float value_w = l.menu_x + l.menu_w - pad - l.value_x;
        if (item.kind == Item::kDevice) {
          auto devices = BindDevices();
          if (item.index >= int(devices.size())) continue;
          const BindDevice& device = devices[size_t(item.index)];
          label(fit(Upper(device.name), l.value_x - l.menu_x - pad * 2), text, l.menu_x + pad, text_y, kUiText);
          label(BindTypeLabel(device), text, l.value_x, text_y, kUiAccent);
        } else if (item.kind == Item::kBindReset) {
          label(bind_device && bind_device->midi ? "RESET TO GENERAL MIDI" : "RESET TO DEFAULTS", text,
                l.menu_x + pad, text_y, kUiText);
        } else if (bind_device && item.kind == Item::kBindType) {
          label("INSTRUMENT TYPE", text, l.menu_x + pad, text_y, kUiText);
          label(BindTypeLabel(*bind_device), text, l.value_x, text_y, kUiAccent);
        } else if (item.kind == Item::kBindMic) {
          static constexpr const char* kMicLabels[] = {"MIC 1: LEAD / HARMONY 1", "MIC 2: HARMONY 2",
                                                       "MIC 3: HARMONY 3"};
          label(fit(kMicLabels[item.index], l.value_x - l.menu_x - pad * 2), text, l.menu_x + pad, text_y, kUiText);
          bool live = MicLevel(item.index) > 0.05f;
          label(fit(MicValue(item.index), value_w), text, l.value_x, text_y,
                live ? IM_COL32(110, 255, 120, 255) : kUiAccent);
        } else if (bind_device) {
          label(fit(BindInputLabel(*bind_device, item.index), l.value_x - l.menu_x - pad * 2), text,
                l.menu_x + pad, text_y, kUiText);
          bool live = BindInputLive(*bind_device, item.index);
          label(fit(BindInputValue(*bind_device, item.index), value_w), text, l.value_x, text_y,
                live ? IM_COL32(110, 255, 120, 255) : kUiAccent);
        }
        continue;
      }
      // Windowed: follows the window, grayed out.
      bool locked = (row == Row::kResolution || row == Row::kAspectRatio) && !ChoiceCount(row);
      label(Label(row), text, l.menu_x + pad, text_y, locked ? kUiMuted : kUiText);

      if (IsCheckbox(row)) {
        float box = 26.0f * l.scale;
        ImVec2 min(l.value_x, y + (l.row_h - box) * 0.5f), max(min.x + box, min.y + box);
        ImU32 tint = selected ? IM_COL32(60, 60, 50, 255) : IM_COL32_WHITE;
        if (ui_.HasImage("checkbox")) {
          ui_.DrawImage(draw, "checkbox", min, max, tint);
          if (ChoiceIndex(row)) {
            float o = box * 0.08f;
            ui_.DrawImage(draw, "check", ImVec2(min.x + o, min.y - o * 2),
                          ImVec2(max.x + o * 3, max.y - o), IM_COL32_WHITE);
          }
        } else {
          label(ChoiceIndex(row) ? "ON" : "OFF", text, l.value_x, text_y, kUiAccent);
        }
      } else if (row == Row::kProfile && editing_) {
        bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        float w = ui_.TextWidth(UiFont::kHub, edit_name_, text);
        label(edit_name_, text, l.value_x, text_y, kUiAccent);
        if (blink) {
          draw->AddRectFilled(ImVec2(l.value_x + w + 3 * l.scale, text_y + text * 0.1f),
                              ImVec2(l.value_x + w + 13 * l.scale, text_y + text * 0.9f),
                              selected ? kUiDark : kUiAccent);
        }
      } else {
        float x = l.value_x;
        bool arrows = selected && adjusting_;
        // Left chevron is the right one mirrored.
        float size = l.row_h * 0.95f, arrow = size * 0.3f;
        auto draw_arrow = [&](bool left, float center_x) {
          ImVec2 min(center_x - size * 0.5f, y + (l.row_h - size) * 0.5f);
          ImVec2 max(min.x + size, min.y + size);
          ui_.DrawImage(draw, "chevron", min, max, kUiDark,
                        left ? ImVec2(1, 0) : ImVec2(0, 0), left ? ImVec2(0, 1) : ImVec2(1, 1));
        };
        // Values stay put; the left arrow sits in the gap before them.
        std::string value = Value(row);
        label(value, text, x, text_y, kUiAccent);
        if (arrows) {
          draw_arrow(true, x - arrow - 6 * l.scale);
          draw_arrow(false, x + ui_.TextWidth(UiFont::kHub, value, text) + arrow + 6 * l.scale);
        }
      }

      if (row == Row::kBindControllers && ui_.HasImage("chevron")) {
        float size = l.row_h * 0.95f;
        float x1 = l.menu_x + l.menu_w - pad * 0.5f;
        float cy = y + l.row_h * 0.5f;
        ui_.DrawImage(draw, "chevron", ImVec2(x1 - size, cy - size * 0.5f), ImVec2(x1, cy + size * 0.5f),
                      selected ? kUiDark : kUiText);
      }
      if (row == Row::kDeluxeInstall) {
        std::string note = DeluxeNote();
        if (note.size() > 34) note = note.substr(0, 33) + "...";
        float small = 15.0f * l.scale;
        float w = ui_.TextWidth(UiFont::kDisplay, note, small);
        ui_.DrawText(draw, UiFont::kDisplay, note, small, l.menu_x + l.menu_w - pad - w,
                     y + (l.row_h - small) * 0.5f,
                     selected ? IM_COL32(40, 40, 30, 255) : kUiMuted);
      }
      if (row == Row::kSongsDefault || IsExtraSongsFolder(row)) {
        std::string note = SongsFolderNote(row);
        float small = 15.0f * l.scale;
        float w = ui_.TextWidth(UiFont::kDisplay, note, small);
        ImU32 color = selected ? IM_COL32(40, 40, 30, 255) : kUiMuted;
        ui_.DrawText(draw, UiFont::kDisplay, note, small, l.menu_x + l.menu_w - pad - w,
                     y + (l.row_h - small) * 0.5f, color);
      }

      if (row >= Row::kController1 && row <= Row::kController4) {
        size_t slot = size_t(int(row) - int(Row::kController1));
        std::string name = slot < pad_names.size() ? ShortPadName(pad_names[slot]) : "NOT CONNECTED";
        if (auto midi = MidiInPort(row)) name = Upper(midi->name);
        if (name.size() > 30) name = name.substr(0, 29) + "...";
        float small = 15.0f * l.scale;
        float w = ui_.TextWidth(UiFont::kDisplay, name, small);
        ui_.DrawText(draw, UiFont::kDisplay, name, small, l.menu_x + l.menu_w - pad - w,
                     y + (l.row_h - small) * 0.5f,
                     selected ? IM_COL32(40, 40, 30, 255) : kUiMuted);
      }
    }

    DrawHints(draw, display, l);
  }

  // The help bar, with the game's fret icons ("g" / "r" in its buttons font).
  void DrawHints(ImDrawList* draw, ImVec2 display, const Layout& l) {
    struct Hint {
      const char* icon;  // buttons-font glyph, or nullptr for a text-only hint
      std::string text;
    };
    std::vector<Hint> hints;
    Item hovered_item = Selected();
    bool bind_midi = false;
    if (auto device = bind_ == BindScreen::kDevice ? CurrentDevice() : std::nullopt) bind_midi = device->midi;
    if (capturing_) {
      hints = {{nullptr, bind_midi ? "HIT A PAD" : "PRESS OR MOVE A CONTROL"}, {nullptr, "ESC: CANCEL"}};
    } else if (bind_ == BindScreen::kDevice) {
      // Keyboard and mouse only here (HandleInput).
      hints = {{nullptr, "UP / DOWN: MOVE"}};
      switch (hovered_item.kind) {
        case Item::kBindInput:
          hints.push_back({nullptr, "ENTER: BIND"});
          if (bind_midi) hints.push_back({nullptr, "RIGHT: ADD A NOTE"});
          hints.push_back({nullptr, "LEFT: CLEAR"});
          break;
        case Item::kBindType: hints.push_back({nullptr, "ENTER: CHANGE"}); break;
        case Item::kBindMic: hints.push_back({nullptr, "LEFT / RIGHT: CHANGE"}); break;
        case Item::kBindReset: hints.push_back({nullptr, "ENTER: RESET"}); break;
        default: break;
      }
      hints.push_back({nullptr, "ESC: BACK"});
    } else if (hovered_item.kind == Item::kDevice) {
      hints = {{nullptr, "STRUM: MOVE"}, {"g", "BINDINGS"}, {"r", "BACK"}};
    } else if (editing_) {
      hints = {{nullptr, "TYPE A NAME"},
               {nullptr, "BACKSPACE: DELETE"},
               {nullptr, "ENTER: DONE"},
               {nullptr, "ESC: CANCEL"}};
    } else if (adjusting_) {
      hints = {{nullptr, "STRUM: CHANGE"}, {"g", "SAVE"}, {"r", "SAVE"}, {nullptr, "START: PLAY"}};
    } else if (SelectedRow() == Row::kProfile) {
      hints = {{nullptr, "STRUM: MOVE"},
               {"g", "RENAME"},
               {nullptr, "OR TYPE ON THE KEYBOARD"},
               {"r", "BACK"},
               {nullptr, "START: PLAY"}};
    } else if (InSection()) {
      Row row = SelectedRow();
      const char* confirm = IsListChoice(row)            ? "SELECT"
                            : row == Row::kSongsDefault  ? "OPEN"
                            : row == songs_remove_armed_ ? "REMOVE (FILES STAY)"
                            : IsExtraSongsFolder(row)    ? "REMOVE"
                            : row == Row::kSongsAdd      ? "ADD"
                            : row == Row::kBindControllers ? "OPEN"
                                                         : "CHANGE";
      hints = {{nullptr, "STRUM: MOVE"},
               {"g", confirm},
               {"r", "BACK"},
               {nullptr, "START: PLAY"}};
    } else {
      hints = {{nullptr, "STRUM: MOVE"}, {"g", "SELECT"}, {nullptr, "START: PLAY"}};
    }
    float size = 19.0f * l.scale;
    float icon = size * 1.5f;
    float gap = 30.0f * l.scale;
    float total = 0;
    for (const auto& hint : hints) {
      total += ui_.TextWidth(UiFont::kHub, hint.text, size) + (hint.icon ? icon : 0);
    }
    total += gap * float(hints.size() - 1);

    float bar_h = 44.0f * l.scale;
    float bar_y = display.y - bar_h - 14.0f * l.scale;
    draw->AddRectFilledMultiColor(ImVec2(0, bar_y), ImVec2(display.x, bar_y + bar_h),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 140));
    float x = l.cx - total * 0.5f;
    float y = bar_y + (bar_h - size) * 0.5f;
    for (const auto& hint : hints) {
      if (hint.icon) {
        ui_.DrawText(draw, UiFont::kButtons, hint.icon, icon, x, y + (size - icon) * 0.5f,
                     IM_COL32_WHITE);
        x += icon;
      }
      ui_.DrawTextShadowed(draw, UiFont::kHub, hint.text, size, x, y, kUiText);
      x += ui_.TextWidth(UiFont::kHub, hint.text, size) + gap;
    }
  }

  Rb3UiRenderer ui_;
  LauncherHooks hooks_;
  MenuPads pads_;
  Repeater repeater_;
  int selected_ = 0;  // index into Items()
  int scroll_ = 0;    // the first item shown, in lists longer than the screen
  Section section_ = Section::kCount;  // the open category, or kCount at the top
  BindScreen bind_ = BindScreen::kNone;
  std::string bind_key_;  // the device whose bindings are open
  bool capturing_ = false;  // waiting for a control to bind
  int capture_input_ = 0;
  bool capture_add_ = false;
  uint64_t capture_serial_ = 0;  // MIDI: the last hit before capturing
  std::optional<Capture> sdl_capture_;
  Clock::time_point capture_start_;
  bool editing_ = false;
  std::unique_ptr<rex::kernel::xam::KeyboardUi> name_entry_;  // open from a controller
  bool adjusting_ = false;  // a list setting is selected for changing
  int pending_ = 0;         // its choice until saved
  std::string edit_name_;
  bool finished_ = false;
  std::shared_ptr<DeluxeInstallJob> install_ = std::make_shared<DeluxeInstallJob>();
  std::string install_status_;  // the last install's error, if it failed
  struct PickedFolder {
    std::mutex mutex;
    std::optional<std::filesystem::path> path;
  };
  std::shared_ptr<PickedFolder> songs_picked_ = std::make_shared<PickedFolder>();
  std::vector<std::filesystem::path> songs_folders_ = ExtraSongsFolders();  // besides the default
  std::string songs_status_;  // why the last pick wasn't added, if it wasn't
  Row songs_remove_armed_ = Row::kCount;  // pressed once to remove
  size_t songs_count_ = CountSongPackages(SongsFolders());
};

class ShiftCheck final : public rex::ui::ImGuiDialog {
 public:
  ShiftCheck(rex::ui::ImGuiDrawer* drawer, std::function<void()> request_paint,
             std::function<void(bool)> done)
      : ImGuiDialog(drawer), request_paint_(std::move(request_paint)), done_(std::move(done)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!done_) return;
    auto now = Clock::now();
    // The compositor sends the held modifiers right after the focus; give
    // them a moment to be read.
    if (SDL_GetKeyboardFocus()) {
      if (!focused_at_) focused_at_ = now;
    }
    bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    bool timed_out = focused_at_ ? now - *focused_at_ >= std::chrono::milliseconds(100)
                                 : now - start_ >= std::chrono::seconds(1);
    if (shift || timed_out) {
      std::function<void(bool)> done = std::move(done_);
      done_ = nullptr;
      if (!shift && !focused_at_) REXLOG_INFO("Startup Shift check: the window got no keyboard focus");
      done(shift);
      return;
    }
    if (request_paint_) request_paint_();
  }

 private:
  std::function<void()> request_paint_;
  std::function<void(bool)> done_;
  Clock::time_point start_ = Clock::now();
  std::optional<Clock::time_point> focused_at_;
};

}  // namespace

bool LauncherAllowed() {
  // A boot handed over from the other executable has been through it already.
  return REXCVAR_GET(debug_input_script).empty() && !LaunchedByOtherExecutable();
}

bool ShowLauncherSetting() { return REXCVAR_GET(show_launcher); }

std::unique_ptr<rex::ui::ImGuiDialog> CreateShiftCheck(rex::ui::ImGuiDrawer* drawer,
                                                       std::function<void()> request_paint,
                                                       std::function<void(bool)> done) {
  return std::make_unique<ShiftCheck>(drawer, std::move(request_paint), std::move(done));
}

bool LauncherActive() { return g_launcher_active; }

std::unique_ptr<rex::ui::ImGuiDialog> CreateLauncher(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    std::filesystem::path game_root, LauncherHooks hooks) {
  return std::make_unique<Launcher>(drawer, std::move(immediate_drawer), std::move(game_root),
                                    std::move(hooks));
}

}  // namespace rb3pc
