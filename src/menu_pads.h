// Menu input from SDL controllers using their bindings profiles, for the
// launcher and the name box (text_entry.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/input/input.h>

#include "input_bindings.h"

namespace rb3pc {

enum Action : uint32_t {
  kUp = 1 << 0,
  kDown = 1 << 1,
  kLeft = 1 << 2,
  kRight = 1 << 3,
  kConfirm = 1 << 4,  // green fret / A / Enter
  kBack = 1 << 5,     // red fret / B / Backspace
  kStart = 1 << 6,    // Start / Escape-to-finish
  kSelect = 1 << 7,   // Back / Select
};

// All controllers OR'd. Uses SDL directly: the launcher runs before the
// runtime's input system, and the name box while the game gets no input.
class MenuPads {
 public:
  MenuPads() { initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD); }
  // Full shutdown: the SDK's driver only sees pads from a fresh init's
  // SDL_EVENT_GAMEPAD_ADDED.
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
      if (b & X_INPUT_GAMEPAD_BACK) actions |= kSelect;
      // Not the right stick: guitars put tilt and whammy there.
      if (pad.instrument == Instrument::kGamepad) {
        if (state.thumb_ly > 20000) actions |= kUp;
        if (state.thumb_ly < -20000) actions |= kDown;
        if (state.thumb_lx < -20000) actions |= kLeft;
        if (state.thumb_lx > 20000) actions |= kRight;
      }
    }
    return actions;
  }

  // SDL order = controller_type_N order.
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
    auto now = std::chrono::steady_clock::now();
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
  uint32_t held_ = ~0u;  // ignore buttons held on open
  std::array<std::chrono::steady_clock::time_point, 32> next_repeat_{};
};


}  // namespace rb3pc
