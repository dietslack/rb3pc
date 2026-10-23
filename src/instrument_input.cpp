#include "instrument_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

#include "midi_input.h"
#include "mouse_cursor.h"


// By SDL connection order. "auto" uses SDL's joystick type, else gamepad.
// "none" hides the device; the next one takes its player slot.
#define RB3PC_CONTROLLER_TYPE_CVAR(n)                                                     \
  REXCVAR_DEFINE_STRING(controller_type_##n, "auto", "Instruments",                      \
                        "Instrument type for connected controller " #n                    \
                        ": auto, gamepad, guitar, drums or none")                         \
      .allowed({"auto", "gamepad", "guitar", "drums", "none"})
RB3PC_CONTROLLER_TYPE_CVAR(1);
RB3PC_CONTROLLER_TYPE_CVAR(2);
RB3PC_CONTROLLER_TYPE_CVAR(3);
RB3PC_CONTROLLER_TYPE_CVAR(4);

REXCVAR_DEFINE_BOOL(mouse_keyboard, true, "Instruments",
                    "Use the mouse and keyboard in the game's menus (as player 1's controller)");

REXCVAR_DEFINE_BOOL(instrument_input_log, false, "Instruments",
                    "Log controller state changes after remapping (for tuning)");

REXCVAR_DEFINE_STRING(debug_input_script, "", "Instruments",
                      "Testing aid: presses buttons on controller 1 at set times, e.g. "
                      "\"35:start,40:a,42.5:down\" (seconds after launch; buttons a b x y "
                      "start back up down left right lb rb)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_STATE;

// XInput subtypes RB3's controller classifier (0x8251EB20) recognizes.
constexpr uint8_t kSubTypeGuitar = 0x06;    // -> strat_xbox (flags 0)
constexpr uint8_t kSubTypeDrumKit = 0x08;   // -> hx_drums_xbox_rb2 (flags bit 0)

// MIDI device id = base + index in MidiInput::Devices().
constexpr uint64_t kMidiDeviceIdBase = 0x4D49444900000000ull;  // "MIDI"
bool IsMidiDevice(DeviceId id) { return (uint64_t(id) & ~0xFFFFull) == kMidiDeviceIdBase; }

std::mutex g_player_mutex;
std::array<std::vector<DeviceId>, rex::input::kMaxGuestUsers> g_player_devices;
struct PlayerPress {
  int player = -1;
  uint16_t buttons = 0;
  std::chrono::steady_clock::time_point at{};
  uint32_t serial = 0;  // presses sent, for the packet number
};
PlayerPress g_player_press;

// Games ignore a state whose packet number didn't change, so each press bumps
// it twice: on press and on release.
uint16_t PressedOnDevice(DeviceId id, uint32_t& packets) {
  constexpr auto kHold = std::chrono::milliseconds(60);
  std::lock_guard lock(g_player_mutex);
  packets = 0;
  const PlayerPress& press = g_player_press;
  if (press.player < 0) return 0;
  const auto& ids = g_player_devices[size_t(press.player)];
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) return 0;
  bool held = std::chrono::steady_clock::now() - press.at < kHold;
  packets = press.serial * 2 - (held ? 1 : 0);
  return held ? press.buttons : 0;
}

std::string ControllerTypeSetting(size_t index) {
  switch (index) {
    case 0: return REXCVAR_GET(controller_type_1);
    case 1: return REXCVAR_GET(controller_type_2);
    case 2: return REXCVAR_GET(controller_type_3);
    case 3: return REXCVAR_GET(controller_type_4);
    default: return "auto";
  }
}

// Two of the same controller share a GUID, so `nth` tells them apart.
SDL_JoystickID SdlIdForGuid(const std::string& guid, size_t nth) {
  SDL_JoystickID found = 0;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  for (int i = 0; ids && i < count; ++i) {
    char text[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), text, sizeof(text));
    if (guid == text && nth-- == 0) {
      found = ids[i];
      break;
    }
  }
  SDL_free(ids);
  return found;
}

class InstrumentInputDriver;
InstrumentInputDriver* g_driver = nullptr;

struct ScriptedPress {
  double at;
  uint16_t button;
};
constexpr double kScriptPress = 0.2;

std::vector<ScriptedPress> ParseInputScript(const std::string& script) {
  using namespace rex::input;
  static const std::pair<const char*, uint16_t> kButtons[] = {
      {"a", X_INPUT_GAMEPAD_A},
      {"b", X_INPUT_GAMEPAD_B},
      {"x", X_INPUT_GAMEPAD_X},
      {"y", X_INPUT_GAMEPAD_Y},
      {"start", X_INPUT_GAMEPAD_START},
      {"back", X_INPUT_GAMEPAD_BACK},
      {"up", X_INPUT_GAMEPAD_DPAD_UP},
      {"down", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"left", X_INPUT_GAMEPAD_DPAD_LEFT},
      {"right", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
  };
  std::vector<ScriptedPress> presses;
  size_t pos = 0;
  while (pos < script.size()) {
    size_t comma = script.find(',', pos);
    std::string item = script.substr(pos, comma == std::string::npos ? std::string::npos
                                                                     : comma - pos);
    pos = comma == std::string::npos ? script.size() : comma + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    double at = std::atof(item.substr(0, colon).c_str());
    std::string name = item.substr(colon + 1);
    for (const auto& [button_name, bit] : kButtons) {
      if (name == button_name) presses.push_back({at, bit});
    }
  }
  return presses;
}

uint16_t ScriptedButtons() {
  static const auto start = std::chrono::steady_clock::now();
  static const auto presses = ParseInputScript(REXCVAR_GET(debug_input_script));
  if (presses.empty()) return 0;
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  uint16_t buttons = 0;
  for (const auto& press : presses) {
    if (t >= press.at && t < press.at + kScriptPress) buttons |= press.button;
  }
  return buttons;
}

// Wraps the SDK's SDL driver: instruments report a Rock Band subtype, and a
// controller with a bindings profile is read raw through it (input_bindings.cpp).
class InstrumentInputDriver final : public rex::input::InputDriver {
 public:
  InstrumentInputDriver(std::unique_ptr<rex::input::sdl::SDLInputDriver> sdl)
      : InputDriver(nullptr, 0), sdl_(std::move(sdl)) {
    sdl_->set_is_active_callback([this] { return is_active(); });
    g_driver = this;
  }
  ~InstrumentInputDriver() override { g_driver = nullptr; }

  std::vector<ControllerInfo> Controllers() {
    std::lock_guard lock(mutex_);
    std::vector<ControllerInfo> out;
    for (const auto& device : devices_) {
      out.push_back({device.index, device.name, device.guid, device.sdl_id, device.instrument,
                     device.setting});
    }
    return out;
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void OnWindowAvailable(rex::ui::Window* window) override { sdl_->OnWindowAvailable(window); }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    size_t first = out.size();
    sdl_->EnumerateDevices(out);
    // Called constantly; only resolve new or moved devices.
    std::lock_guard lock(mutex_);
    std::vector<Device> devices;
    for (size_t i = first; i < out.size(); ++i) {
      size_t index = i - first;
      auto known = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) {
        return d.id == out[i].id && d.index == index;
      });
      if (known != devices_.end()) {
        devices.push_back(std::move(*known));
        continue;
      }
      size_t same_guid = std::count_if(devices.begin(), devices.end(),
                                       [&](const Device& d) { return d.guid == out[i].guid; });
      Device device{out[i].id, index, out[i].guid, out[i].name};
      device.sdl_id = SdlIdForGuid(device.guid, same_guid);
      device.instrument = Resolve(device);
      devices.push_back(std::move(device));
    }
    devices_ = std::move(devices);
    // Hide controllers set to "none"; they stay in devices_ for the launcher.
    for (size_t i = devices_.size(); i-- > 0;) {
      if (ControllerTypeSetting(devices_[i].index) == "none" && first + i < out.size()) {
        out.erase(out.begin() + std::ptrdiff_t(first + i));
      }
    }
    auto midi = MidiInput::Get().Devices();
    midi_keys_.resize(midi.size());
    for (size_t i = 0; i < midi.size(); ++i) {
      midi_keys_[i] = midi[i].key;
      if (!midi[i].connected || MidiInput::Get().Profile(midi[i].key).type == MidiType::kNone) continue;
      DeviceInfo info;
      info.id = DeviceId(kMidiDeviceIdBase + i);
      info.name = "MIDI: " + midi[i].name;
      info.guid = "midi:" + midi[i].key;
      out.push_back(std::move(info));
    }
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    if (IsMidiDevice(id)) {
      *out_caps = {};
      out_caps->type = 0x01;  // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = kSubTypeDrumKit;
      out_caps->flags = 1;
      out_caps->gamepad.buttons = 0xF3FF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
      return X_ERROR_SUCCESS;
    }
    X_RESULT result = sdl_->GetDeviceCapabilities(id, flags, out_caps);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
    switch (InstrumentFor(id)) {
      case Instrument::kGuitar:
        out_caps->sub_type = kSubTypeGuitar;
        out_caps->flags = 0;
        break;
      case Instrument::kDrums:
        out_caps->sub_type = kSubTypeDrumKit;
        out_caps->flags = 1;
        break;
      case Instrument::kGamepad:
        break;
    }
    return result;
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (IsMidiDevice(id)) return GetMidiState(id, out_state);
    X_RESULT result = sdl_->GetDeviceState(id, out_state);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
    Instrument instrument = InstrumentFor(id);
    std::string guid;
    SDL_JoystickID sdl_id = 0;
    {
      std::lock_guard lock(mutex_);
      for (const auto& device : devices_) {
        if (device.id == id) {
          guid = device.guid;
          sdl_id = device.sdl_id;
        }
      }
    }
    if (auto profile = BindingStore::Get().Find(guid)) {
      if (SDL_Joystick* joystick = sdl_id ? SDL_GetJoystickFromID(sdl_id) : nullptr) {
        PadState pad = Evaluate(*profile, joystick);
        auto& out = out_state->gamepad;
        out.buttons = pad.buttons;
        out.left_trigger = pad.left_trigger;
        out.right_trigger = pad.right_trigger;
        out.thumb_lx = pad.thumb_lx;
        out.thumb_ly = pad.thumb_ly;
        out.thumb_rx = pad.thumb_rx;
        out.thumb_ry = pad.thumb_ry;
      }
    }
    {
      uint32_t packets = 0;
      out_state->gamepad.buttons = uint16_t(out_state->gamepad.buttons) | PressedOnDevice(id, packets);
      out_state->packet_number = uint32_t(out_state->packet_number) + packets;
    }
    if (IsFirstController(id)) {
      // SDK doesn't advance packet_number while unfocused.
      static uint16_t last_scripted = 0;
      static uint32_t script_packets = 0;
      uint16_t scripted = ScriptedButtons();
      if (scripted != last_scripted) {
        last_scripted = scripted;
        ++script_packets;
      }
      out_state->gamepad.buttons = uint16_t(out_state->gamepad.buttons) | scripted;
      out_state->packet_number = uint32_t(out_state->packet_number) + script_packets;
    }
    {
      std::lock_guard lock(mutex_);
      for (auto& device : devices_) {
        if (device.id != id) continue;
        uint16_t buttons = out_state->gamepad.buttons;
        if (buttons & ~device.last_buttons) NoteControllerPress();
        device.last_buttons = buttons;
      }
    }
    if (REXCVAR_GET(instrument_input_log)) {
      LogState(id, instrument, *out_state);
    }
    return result;
  }

  bool IsFirstController(DeviceId id) {
    std::lock_guard lock(mutex_);
    for (const auto& device : devices_) {
      if (ControllerTypeSetting(device.index) != "none") return device.id == id;
    }
    return false;
  }

  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
    if (IsMidiDevice(id)) return X_ERROR_SUCCESS;
    return sdl_->SetDeviceVibration(id, vibration);
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
    if (IsMidiDevice(id)) return X_ERROR_EMPTY;
    return sdl_->GetDeviceKeystroke(id, flags, out_keystroke);
  }

 private:
  X_RESULT GetMidiState(DeviceId id, X_INPUT_STATE* out_state) {
    size_t index = size_t(uint64_t(id) - kMidiDeviceIdBase);
    std::string key;
    {
      std::lock_guard lock(mutex_);
      if (index >= midi_keys_.size()) return X_ERROR_DEVICE_NOT_CONNECTED;
      key = midi_keys_[index];
    }
    bool changed = false;
    PadState pad = MidiInput::Get().PollDrums(key, changed);
    *out_state = {};
    {
      std::lock_guard lock(mutex_);
      if (changed) ++midi_packets_[index];
      out_state->packet_number = midi_packets_[index];
    }
    uint32_t packets = 0;
    auto& out = out_state->gamepad;
    out.buttons = pad.buttons | PressedOnDevice(id, packets);
    out.thumb_lx = pad.thumb_lx;
    out.thumb_ly = pad.thumb_ly;
    out.thumb_rx = pad.thumb_rx;
    out.thumb_ry = pad.thumb_ry;
    out_state->packet_number = uint32_t(out_state->packet_number) + packets;
    if (changed && pad.buttons) NoteControllerPress();
    if (REXCVAR_GET(instrument_input_log) && changed) {
      REXLOG_INFO("Instruments: MIDI {} buttons={:04X} lx={} ly={} rx={} ry={}", key, pad.buttons,
                  pad.thumb_lx, pad.thumb_ly, pad.thumb_rx, pad.thumb_ry);
    }
    return X_ERROR_SUCCESS;
  }

  struct Device {
    DeviceId id;
    size_t index;
    std::string guid;
    std::string name;
    SDL_JoystickID sdl_id = 0;
    Instrument instrument = Instrument::kGamepad;
    std::string setting;  // controller_type_N value the instrument came from
    X_INPUT_STATE last_logged{};
    uint16_t last_buttons = 0;  // for NoteControllerPress
  };

  Instrument Resolve(Device& device) {
    device.setting = ControllerTypeSetting(device.index);
    Instrument instrument = Instrument::kGamepad;
    if (device.setting == "guitar") {
      instrument = Instrument::kGuitar;
    } else if (device.setting == "drums") {
      instrument = Instrument::kDrums;
    } else if (device.setting == "auto") {
      switch (device.sdl_id ? SDL_GetJoystickTypeForID(device.sdl_id)
                            : SDL_JOYSTICK_TYPE_UNKNOWN) {
        case SDL_JOYSTICK_TYPE_GUITAR: instrument = Instrument::kGuitar; break;
        case SDL_JOYSTICK_TYPE_DRUM_KIT: instrument = Instrument::kDrums; break;
        default: break;
      }
    }
    REXLOG_INFO("Instruments: controller {} \"{}\" -> {} (controller_type_{} = {})",
                device.index + 1, device.name,
                device.setting == "none" ? "ignored" : InstrumentName(instrument),
                device.index + 1, device.setting);
    return instrument;
  }

  Instrument InstrumentFor(DeviceId id) {
    std::lock_guard lock(mutex_);
    for (auto& device : devices_) {
      if (device.id != id) {
        continue;
      }
      if (device.setting != ControllerTypeSetting(device.index)) {
        device.instrument = Resolve(device);
      }
      return device.instrument;
    }
    return Instrument::kGamepad;
  }

  void LogState(DeviceId id, Instrument instrument, const X_INPUT_STATE& state) {
    std::lock_guard lock(mutex_);
    for (auto& device : devices_) {
      if (device.id != id) {
        continue;
      }
      const auto& pad = state.gamepad;
      const auto& last = device.last_logged.gamepad;
      if (pad.buttons == last.buttons && pad.left_trigger == last.left_trigger &&
          pad.right_trigger == last.right_trigger && pad.thumb_lx == last.thumb_lx &&
          pad.thumb_ly == last.thumb_ly && pad.thumb_rx == last.thumb_rx &&
          pad.thumb_ry == last.thumb_ry) {
        return;
      }
      device.last_logged = state;
      REXLOG_INFO(
          "Instruments: controller {} ({}) buttons={:04X} lt={} rt={} lx={} ly={} rx={} ry={}",
          device.index + 1, InstrumentName(instrument), uint16_t(pad.buttons),
          pad.left_trigger, pad.right_trigger, int16_t(pad.thumb_lx), int16_t(pad.thumb_ly),
          int16_t(pad.thumb_rx), int16_t(pad.thumb_ry));
      return;
    }
  }

  std::unique_ptr<rex::input::sdl::SDLInputDriver> sdl_;
  std::mutex mutex_;
  std::vector<Device> devices_;
  std::vector<std::string> midi_keys_;  // by MIDI device index
  std::unordered_map<size_t, uint32_t> midi_packets_;
};

}  // namespace

std::vector<ControllerInfo> ConnectedControllers() {
  return g_driver ? g_driver->Controllers() : std::vector<ControllerInfo>{};
}

void SetControllerType(size_t index, const std::string& type) {
  if (index < 4) {
    rex::cvar::SetFlagByName("controller_type_" + std::to_string(index + 1), type);
  }
}

bool MouseKeyboardEnabled() { return REXCVAR_GET(mouse_keyboard); }

void SetMouseKeyboardEnabled(bool on) {
  rex::cvar::SetFlagByName("mouse_keyboard", on ? "true" : "false");
  rex::cvar::SetFlagByName("mnk_mode", on ? "true" : "false");
}

void ApplyMouseKeyboardDefaults() {
  // MnK merges into player 1. SDK defaults are a shooter's; replace with menu
  // binds. Enablement is our own cvar since mnk_mode's default is off, so a
  // saved off there is indistinguishable from unset.
  SetMouseKeyboardEnabled(MouseKeyboardEnabled());
  static const std::pair<const char*, const char*> kDefaults[] = {
      {"mnk_mouse", "false"},       // no mouse look
      {"mnk_wheel_dpad", "true"},   // the wheel moves through lists (SDK patch 0024)
      {"keybind_dpad_up", "Up"},
      {"keybind_dpad_down", "Down"},
      {"keybind_dpad_left", "Left"},
      {"keybind_dpad_right", "Right"},
      {"keybind_a", "Return,NumpadEnter,Space,1,LMB"},  // green: select
      {"keybind_b", "Escape,Backspace,2,RMB"},          // red: back
      {"keybind_y", "3"},                               // yellow
      {"keybind_x", "4"},                               // blue
      {"keybind_left_shoulder", "5"},                   // orange
      {"keybind_start", "Tab,P,MMB"},                   // overshell menu / pause
      {"keybind_back", ""},
      {"keybind_right_shoulder", ""},
      {"keybind_left_trigger", ""},
      {"keybind_right_trigger", ""},
      {"keybind_lstick_up", ""},
      {"keybind_lstick_down", ""},
      {"keybind_lstick_left", ""},
      {"keybind_lstick_right", ""},
      {"keybind_lstick_press", ""},
      {"keybind_rstick_up", ""},
      {"keybind_rstick_down", ""},
      {"keybind_rstick_left", ""},
      {"keybind_rstick_right", ""},
      {"keybind_rstick_press", ""},
  };
  for (const auto& [name, value] : kDefaults) {
    if (!rex::cvar::HasNonDefaultValue(name)) rex::cvar::SetFlagByName(name, value);
  }
}

void PressOnPlayer(int player, uint16_t buttons) {
  if (player <= 0) {  // the keyboard and mouse are player 1
    rex::input::mnk::QueuePress(buttons);
    return;
  }
  if (player >= int(rex::input::kMaxGuestUsers)) return;
  std::lock_guard lock(g_player_mutex);
  g_player_press.player = player;
  g_player_press.buttons = buttons;
  g_player_press.at = std::chrono::steady_clock::now();
  ++g_player_press.serial;
}

// SDK's SlotAssignment, plus logging and a copy for PressOnPlayer.
class LoggedAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
    slots_.OnDevicesChanged(devices);
    for (const auto& device : devices) {
      REXLOG_INFO("Input: {} -> player {}", device.name,
                  device.synthetic ? 1 : int(device.ordinal) + 1);
    }
    std::lock_guard lock(g_player_mutex);
    for (uint32_t user = 0; user < g_player_devices.size(); ++user) {
      slots_.DevicesForUser(user, g_player_devices[user]);
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
    slots_.DevicesForUser(user_index, out);
  }

 private:
  rex::input::SlotAssignment slots_;
};

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);

  if (!tool_mode) {
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::make_unique<InstrumentInputDriver>(std::move(sdl_driver)));
        MidiInput::Get().Start();
      }
    }
    auto mnk_driver = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<LoggedAssignment>());
  return input;
}

}  // namespace rb3pc
