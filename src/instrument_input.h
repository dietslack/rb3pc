// SDL controllers presented to RB3 as Rock Band guitars or drum kits, with
// per-controller bindings (input_bindings.h).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/system/interfaces/input.h>

#include "input_bindings.h"

namespace rb3pc {

// Enables the SDK's MnK driver with menu bindings, where rb3pc.toml doesn't
// set them. Call before the input system is created.
void ApplyMouseKeyboardDefaults();

bool MouseKeyboardEnabled();
void SetMouseKeyboardEnabled(bool on);

// Same drivers as rex::input::CreateDefaultInputSystem, with the SDL driver
// wrapped by an instrument-aware driver (controller_type_N cvars).
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

struct ControllerInfo {
  size_t index;  // 0-based connection order; controller_type_<index + 1>
  std::string name;
  std::string guid;
  SDL_JoystickID sdl_id;
  Instrument instrument;
  std::string type_setting;  // controller_type_N value
};

std::vector<ControllerInfo> ConnectedControllers();

// Sets controller_type_<index + 1> ("auto", "gamepad", "guitar", "drums").
void SetControllerType(size_t index, const std::string& type);

// Briefly presses `buttons` on player `player`'s controllers (0-3, the game's
// pad number). Player 1 also gets it from MnK, which shares that slot.
void PressOnPlayer(int player, uint16_t buttons);

// While on, the game sees every controller idle (MIDI included); SDL and the
// bindings screens still read them. For the bindings window.
void HoldControllersFromGame(bool hold);

}  // namespace rb3pc
