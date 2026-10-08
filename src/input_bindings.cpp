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

}  // namespace rb3pc
