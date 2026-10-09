#include "input_bindings.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string_view>

#include <toml++/toml.hpp>

#include <rex/input/input.h>
#include <rex/logging.h>

namespace rb3pc {

namespace {

struct OutputInfo {
  const char* key;
  uint16_t button;  // X_INPUT_GAMEPAD_* bit, 0 for analog outputs
  const char* gamepad_label;
  const char* guitar_label;
  const char* drums_label;
};

using namespace rex::input;

// Guitar and drum meanings follow the Xbox 360 Rock Band layouts RB3 reads.
constexpr OutputInfo kOutputs[kOutputCount] = {
    {"a", X_INPUT_GAMEPAD_A, "A", "Green fret", "Green pad"},
    {"b", X_INPUT_GAMEPAD_B, "B", "Red fret", "Red pad"},
    {"y", X_INPUT_GAMEPAD_Y, "Y", "Yellow fret", "Yellow pad / cymbal"},
    {"x", X_INPUT_GAMEPAD_X, "X", "Blue fret", "Blue pad / cymbal"},
    {"left_shoulder", X_INPUT_GAMEPAD_LEFT_SHOULDER, "Left bumper", "Orange fret", "Kick pedal"},
    {"right_shoulder", X_INPUT_GAMEPAD_RIGHT_SHOULDER, "Right bumper", "Right bumper",
     "Cymbal flag"},
    {"dpad_up", X_INPUT_GAMEPAD_DPAD_UP, "D-pad up", "Strum up", "D-pad up (yellow cymbal)"},
    {"dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN, "D-pad down", "Strum down",
     "D-pad down (blue cymbal)"},
    {"dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT, "D-pad left", "D-pad left", "D-pad left"},
    {"dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT, "D-pad right", "D-pad right", "D-pad right"},
    {"start", X_INPUT_GAMEPAD_START, "Start", "Start", "Start"},
    {"back", X_INPUT_GAMEPAD_BACK, "Back", "Select / overdrive", "Select / overdrive"},
    {"guide", X_INPUT_GAMEPAD_GUIDE, "Guide", "Guide", "Guide"},
    {"left_thumb", X_INPUT_GAMEPAD_LEFT_THUMB, "Left stick click", "Solo fret modifier",
     "Second kick pedal"},
    {"right_thumb", X_INPUT_GAMEPAD_RIGHT_THUMB, "Right stick click", "Right stick click",
     "Pad flag"},
    {"left_x", 0, "Left stick X", "Left stick X", "Left stick X"},
    {"left_y", 0, "Left stick Y", "Left stick Y", "Left stick Y"},
    {"right_x", 0, "Right stick X", "Whammy", "Right stick X"},
    {"right_y", 0, "Right stick Y", "Tilt (overdrive)", "Right stick Y"},
    {"left_trigger", 0, "Left trigger", "Pickup switch", "Left trigger"},
    {"right_trigger", 0, "Right trigger", "Right trigger", "Right trigger"},
};

const OutputInfo& Info(Output output) { return kOutputs[size_t(output)]; }

constexpr int16_t kAxisPressThreshold = 16384;  // half deflection
constexpr int kCaptureMoveThreshold = 16384;

struct HatName {
  uint8_t bit;
  const char* name;
};
constexpr HatName kHatNames[] = {
    {SDL_HAT_UP, "up"}, {SDL_HAT_DOWN, "down"}, {SDL_HAT_LEFT, "left"}, {SDL_HAT_RIGHT, "right"}};

int ParseInt(std::string_view text) {
  int value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

bool Pressed(const Source& source, SDL_Joystick* joystick) {
  switch (source.kind) {
    case Source::Kind::kButton:
      return SDL_GetJoystickButton(joystick, source.index);
    case Source::Kind::kHat:
      return (SDL_GetJoystickHat(joystick, source.index) & source.direction) != 0;
    case Source::Kind::kAxis: {
      int value = SDL_GetJoystickAxis(joystick, source.index);
      if (source.direction > 0) return value > kAxisPressThreshold;
      if (source.direction < 0) return value < -kAxisPressThreshold;
      return (source.invert ? -value : value) > kAxisPressThreshold;
    }
    default:
      return false;
  }
}

// 0..65535; 0 = released / fully negative.
int Magnitude(const Source& source, SDL_Joystick* joystick) {
  switch (source.kind) {
    case Source::Kind::kButton:
    case Source::Kind::kHat:
      return Pressed(source, joystick) ? 65535 : 0;
    case Source::Kind::kAxis: {
      int value = SDL_GetJoystickAxis(joystick, source.index);
      if (source.direction != 0) {
        int deflection = source.direction > 0 ? value : -value - 1;
        return std::clamp(deflection, 0, 32767) * 2 + 1;
      }
      if (source.invert) value = -value - 1;
      return value + 32768;
    }
    default:
      return 0;
  }
}

// Buttons and half axes map to -32768 (released) .. 32767, like a whammy.
int16_t StickValue(const Source& source, SDL_Joystick* joystick) {
  if (source.kind == Source::Kind::kNone) return 0;
  return int16_t(Magnitude(source, joystick) - 32768);
}

uint8_t TriggerValue(const Source& source, SDL_Joystick* joystick) {
  return uint8_t(Magnitude(source, joystick) >> 8);
}

}  // namespace

const char* InstrumentName(Instrument instrument) {
  switch (instrument) {
    case Instrument::kGuitar: return "guitar";
    case Instrument::kDrums: return "drums";
    default: return "gamepad";
  }
}

bool IsAnalog(Output output) { return Info(output).button == 0; }

const char* OutputKey(Output output) { return Info(output).key; }

uint16_t OutputButton(Output output) { return Info(output).button; }

std::string OutputLabel(Output output, Instrument instrument) {
  switch (instrument) {
    case Instrument::kGuitar: return Info(output).guitar_label;
    case Instrument::kDrums: return Info(output).drums_label;
    default: return Info(output).gamepad_label;
  }
}

std::string Source::ToString() const {
  switch (kind) {
    case Kind::kButton:
      return "button:" + std::to_string(index);
    case Kind::kAxis:
      if (direction > 0) return "axis:" + std::to_string(index) + ":+";
      if (direction < 0) return "axis:" + std::to_string(index) + ":-";
      return "axis:" + std::to_string(index) + (invert ? ":inv" : "");
    case Kind::kHat:
      for (const auto& hat : kHatNames) {
        if (hat.bit == uint8_t(direction)) {
          return "hat:" + std::to_string(index) + ":" + hat.name;
        }
      }
      return "";
    default:
      return "";
  }
}

Source Source::Parse(std::string_view text) {
  Source source;
  auto colon = text.find(':');
  if (colon == std::string_view::npos) return source;
  auto kind = text.substr(0, colon);
  auto rest = text.substr(colon + 1);
  auto colon2 = rest.find(':');
  auto index = rest.substr(0, colon2);
  auto modifier = colon2 == std::string_view::npos ? std::string_view{} : rest.substr(colon2 + 1);
  source.index = uint8_t(ParseInt(index));
  if (kind == "button") {
    source.kind = Kind::kButton;
  } else if (kind == "axis") {
    source.kind = Kind::kAxis;
    if (modifier == "+") source.direction = 1;
    if (modifier == "-") source.direction = -1;
    if (modifier == "inv") source.invert = true;
  } else if (kind == "hat") {
    for (const auto& hat : kHatNames) {
      if (modifier == hat.name) {
        source.kind = Kind::kHat;
        source.direction = int8_t(hat.bit);
      }
    }
  }
  return source;
}

std::string Source::Describe() const {
  switch (kind) {
    case Kind::kButton:
      return "Button " + std::to_string(index);
    case Kind::kAxis:
      if (direction > 0) return "Axis " + std::to_string(index) + " +";
      if (direction < 0) return "Axis " + std::to_string(index) + " -";
      return "Axis " + std::to_string(index) + (invert ? " (inverted)" : "");
    case Kind::kHat:
      for (const auto& hat : kHatNames) {
        if (hat.bit == uint8_t(direction)) {
          return "Hat " + std::to_string(index) + " " + hat.name;
        }
      }
      return "Hat " + std::to_string(index);
    default:
      return "(unbound)";
  }
}

PadState Evaluate(const Profile& profile, SDL_Joystick* joystick) {
  PadState state;
  if (!joystick) return state;
  for (size_t i = 0; i < kOutputCount; ++i) {
    const Source& source = profile.sources[i];
    if (source.kind == Source::Kind::kNone) continue;
    auto output = Output(i);
    switch (output) {
      case Output::kLeftX: state.thumb_lx = StickValue(source, joystick); break;
      case Output::kLeftY: state.thumb_ly = StickValue(source, joystick); break;
      case Output::kRightX: state.thumb_rx = StickValue(source, joystick); break;
      case Output::kRightY: state.thumb_ry = StickValue(source, joystick); break;
      case Output::kLeftTrigger: state.left_trigger = TriggerValue(source, joystick); break;
      case Output::kRightTrigger: state.right_trigger = TriggerValue(source, joystick); break;
      default:
        if (Pressed(source, joystick)) state.buttons |= Info(output).button;
        break;
    }
  }
  if (profile.disable_tilt) {
    state.thumb_ry = 0;
  }
  return state;
}

Profile DefaultProfile(SDL_JoystickID id) {
  Profile profile;
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
  profile.guid = guid;
  const char* name = SDL_GetJoystickNameForID(id);
  profile.name = name ? name : "";

  // Y axes flipped: SDL's point down, XInput's up.
  struct Element {
    std::string_view name;
    Output output;
    bool invert;
  };
  static constexpr Element kElements[] = {
      {"a", Output::kA, false}, {"b", Output::kB, false}, {"x", Output::kX, false},
      {"y", Output::kY, false}, {"leftshoulder", Output::kLeftShoulder, false},
      {"rightshoulder", Output::kRightShoulder, false}, {"dpup", Output::kDpadUp, false},
      {"dpdown", Output::kDpadDown, false}, {"dpleft", Output::kDpadLeft, false},
      {"dpright", Output::kDpadRight, false}, {"start", Output::kStart, false},
      {"back", Output::kBack, false}, {"guide", Output::kGuide, false},
      {"leftstick", Output::kLeftThumb, false}, {"rightstick", Output::kRightThumb, false},
      {"leftx", Output::kLeftX, false}, {"lefty", Output::kLeftY, true},
      {"rightx", Output::kRightX, false}, {"righty", Output::kRightY, true},
      {"lefttrigger", Output::kLeftTrigger, false},
      {"righttrigger", Output::kRightTrigger, false},
  };

  char* mapping = SDL_GetGamepadMappingForID(id);
  if (!mapping) return profile;
  std::string_view rest(mapping);
  while (!rest.empty()) {
    auto comma = rest.find(',');
    auto entry = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    auto colon = entry.find(':');
    if (colon == std::string_view::npos) continue;
    auto key = entry.substr(0, colon);
    auto value = entry.substr(colon + 1);
    for (const auto& element : kElements) {
      if (key != element.name || value.size() < 2) continue;
      Source source;
      if (value[0] == 'b') {
        source.kind = Source::Kind::kButton;
        source.index = uint8_t(ParseInt(value.substr(1)));
      } else if (value[0] == 'a') {
        source.kind = Source::Kind::kAxis;
        source.index = uint8_t(ParseInt(value.substr(1)));
        source.invert = element.invert;
      } else if (value[0] == 'h') {  // h<hat>.<mask>
        auto dot = value.find('.');
        if (dot == std::string_view::npos) continue;
        source.kind = Source::Kind::kHat;
        source.index = uint8_t(ParseInt(value.substr(1, dot - 1)));
        source.direction = int8_t(ParseInt(value.substr(dot + 1)));
      }
      profile.sources[size_t(element.output)] = source;
    }
  }
  SDL_free(mapping);
  return profile;
}

// Whammy (right stick X, -32768 at rest per joypad.dta) and pickup switch
// (left trigger). A mid-resting axis like a PS3 whammy must bind as half axis.
bool IsOneWayGuitarControl(Output output, Instrument instrument) {
  return instrument == Instrument::kGuitar &&
         (output == Output::kRightX || output == Output::kLeftTrigger);
}

Capture::Capture(SDL_Joystick* joystick, Output output, Instrument instrument)
    : output_(output), one_way_(IsOneWayGuitarControl(output, instrument)) {
  if (!joystick) return;
  for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); ++i) {
    buttons_.push_back(SDL_GetJoystickButton(joystick, i));
  }
  for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i) {
    axes_.push_back(SDL_GetJoystickAxis(joystick, i));
  }
  for (int i = 0; i < SDL_GetNumJoystickHats(joystick); ++i) {
    hats_.push_back(SDL_GetJoystickHat(joystick, i));
  }
}

std::optional<Source> Capture::Poll(SDL_Joystick* joystick) {
  if (!joystick) return std::nullopt;
  Source source;
  for (size_t i = 0; i < buttons_.size(); ++i) {
    bool down = SDL_GetJoystickButton(joystick, int(i));
    if (down && !buttons_[i]) {
      source.kind = Source::Kind::kButton;
      source.index = uint8_t(i);
      return source;
    }
    buttons_[i] = down;
  }
  for (size_t i = 0; i < hats_.size(); ++i) {
    uint8_t hat = SDL_GetJoystickHat(joystick, int(i));
    uint8_t pressed = hat & ~hats_[i];
    for (const auto& name : kHatNames) {
      if (pressed & name.bit) {
        source.kind = Source::Kind::kHat;
        source.index = uint8_t(i);
        source.direction = int8_t(name.bit);
        return source;
      }
    }
    hats_[i] = hat;
  }
  for (size_t i = 0; i < axes_.size(); ++i) {
    int value = SDL_GetJoystickAxis(joystick, int(i));
    int delta = value - axes_[i];
    if (std::abs(delta) < kCaptureMoveThreshold) continue;
    source.kind = Source::Kind::kAxis;
    source.index = uint8_t(i);
    bool rests_mid_range = std::abs(int(axes_[i])) < kCaptureMoveThreshold;
    if (IsAnalog(output_) && !(one_way_ && rests_mid_range)) {
      source.invert = delta < 0;
    } else {
      source.direction = delta > 0 ? 1 : -1;
    }
    return source;
  }
  return std::nullopt;
}

BindingStore& BindingStore::Get() {
  static BindingStore store;
  return store;
}

void BindingStore::SetDirectory(std::filesystem::path dir) {
  std::lock_guard lock(mutex_);
  dir_ = std::move(dir);
  cache_.clear();
}

std::optional<Profile> BindingStore::Find(const std::string& guid) {
  std::lock_guard lock(mutex_);
  auto it = cache_.find(guid);
  if (it == cache_.end()) {
    it = cache_.emplace(guid, Load(guid)).first;
  }
  return it->second;
}

std::optional<Profile> BindingStore::Load(const std::string& guid) {
  auto path = dir_ / (guid + ".toml");
  std::error_code ec;
  if (dir_.empty() || !std::filesystem::exists(path, ec)) {
    return std::nullopt;
  }
  try {
    auto table = toml::parse_file(path.string());
    Profile profile;
    profile.guid = guid;
    profile.name = table["name"].value_or(std::string{});
    profile.disable_tilt = table["options"]["disable_tilt"].value_or(false);
    if (auto* bindings = table["bindings"].as_table()) {
      for (size_t i = 0; i < kOutputCount; ++i) {
        if (auto text = (*bindings)[OutputKey(Output(i))].value<std::string>()) {
          profile.sources[i] = Source::Parse(*text);
        }
      }
    }
    REXLOG_INFO("Bindings: loaded {} ({})", path.string(), profile.name);
    return profile;
  } catch (const toml::parse_error& e) {
    REXLOG_ERROR("Bindings: failed to parse {}: {}", path.string(), e.what());
    return std::nullopt;
  }
}

bool BindingStore::Save(const Profile& profile) {
  std::lock_guard lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  toml::table bindings;
  for (size_t i = 0; i < kOutputCount; ++i) {
    auto text = profile.sources[i].ToString();
    if (!text.empty()) {
      bindings.insert_or_assign(OutputKey(Output(i)), text);
    }
  }
  toml::table options;
  options.insert_or_assign("disable_tilt", profile.disable_tilt);
  toml::table table;
  table.insert_or_assign("name", profile.name);
  table.insert_or_assign("options", std::move(options));
  table.insert_or_assign("bindings", std::move(bindings));

  auto path = dir_ / (profile.guid + ".toml");
  std::ofstream out(path);
  out << "# rb3pc controller bindings for " << profile.name << "\n"
      << "# Values: button:N, axis:N, axis:N:inv, axis:N:+, axis:N:-, hat:N:up|down|left|right\n\n"
      << table << "\n";
  if (!out) {
    REXLOG_ERROR("Bindings: failed to write {}", path.string());
    return false;
  }
  cache_[profile.guid] = profile;
  REXLOG_INFO("Bindings: saved {}", path.string());
  return true;
}

void BindingStore::Remove(const std::string& guid) {
  std::lock_guard lock(mutex_);
  std::error_code ec;
  std::filesystem::remove(dir_ / (guid + ".toml"), ec);
  cache_[guid] = std::nullopt;
}

}  // namespace rb3pc
