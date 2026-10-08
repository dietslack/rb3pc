// Per-controller bindings: raw SDL joystick controls -> Xbox 360 pad.
// Profiles in <exe dir>/bindings/<SDL GUID>.toml; none = SDL gamepad mapping.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

namespace rb3pc {

enum class Instrument { kGamepad, kGuitar, kDrums };

const char* InstrumentName(Instrument instrument);

// In display order.
enum class Output : uint8_t {
  kA, kB, kY, kX, kLeftShoulder, kRightShoulder,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kStart, kBack, kGuide, kLeftThumb, kRightThumb,
  kLeftX, kLeftY, kRightX, kRightY, kLeftTrigger, kRightTrigger,
  kCount,
};
constexpr size_t kOutputCount = size_t(Output::kCount);

bool IsAnalog(Output output);
const char* OutputKey(Output output);  // stable name used in profile files
uint16_t OutputButton(Output output);  // X_INPUT_GAMEPAD_* bit, 0 if analog
// e.g. "Green fret".
std::string OutputLabel(Output output, Instrument instrument);

struct Source {
  enum class Kind : uint8_t { kNone, kButton, kAxis, kHat };
  Kind kind = Kind::kNone;
  uint8_t index = 0;
  // kAxis: 0 = whole axis, +1/-1 = only that half. kHat: SDL_HAT_* bit.
  int8_t direction = 0;
  bool invert = false;  // kAxis, whole axis only

  bool operator==(const Source&) const = default;
  std::string ToString() const;  // "button:3", "axis:2:-", "hat:0:up", "axis:5:inv"
  static Source Parse(std::string_view text);
  std::string Describe() const;  // human-readable, for the UI
};

struct Profile {
  std::string guid;
  std::string name;
  std::array<Source, kOutputCount> sources{};
  // Holds right stick Y (tilt) centred.
  bool disable_tilt = false;
};

// Host-endian equivalent of X_INPUT_GAMEPAD.
struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};

PadState Evaluate(const Profile& profile, SDL_Joystick* joystick);

// Equivalent to SDL's gamepad mapping.
Profile DefaultProfile(SDL_JoystickID id);

// "Press to bind": first control that moves.
class Capture {
 public:
  Capture(SDL_Joystick* joystick, Output output, Instrument instrument);
  std::optional<Source> Poll(SDL_Joystick* joystick);

 private:
  Output output_;
  bool one_way_;  // analog output that only moves one way, e.g. whammy
  std::vector<bool> buttons_;
  std::vector<int16_t> axes_;
  std::vector<uint8_t> hats_;
};

// Thread-safe profile cache backed by <dir>/<guid>.toml.
class BindingStore {
 public:
  static BindingStore& Get();

  void SetDirectory(std::filesystem::path dir);
  std::optional<Profile> Find(const std::string& guid);
  bool Save(const Profile& profile);
  void Remove(const std::string& guid);

 private:
  std::optional<Profile> Load(const std::string& guid);

  std::mutex mutex_;
  std::filesystem::path dir_;
  std::unordered_map<std::string, std::optional<Profile>> cache_;
};

}  // namespace rb3pc
