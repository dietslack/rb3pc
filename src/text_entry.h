// XamShowKeyboardUI replacement in RB3's style, guitar-playable: strum
// changes the letter, green next, red erase, Start confirm, Back cancel.
// The keyboard types directly.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include <rex/kernel/xam/keyboard_ui.h>

namespace rex::ui {
class ImmediateDrawer;
}  // namespace rex::ui

namespace rb3pc {

class Rb3UiRenderer;

// Also for the app's screens; draw each frame until Draw returns true.
std::unique_ptr<rex::kernel::xam::KeyboardUi> CreateTextEntry(
    std::shared_ptr<Rb3UiRenderer> ui, const rex::kernel::xam::KeyboardUiRequest& request);

// Call once the UI exists.
void InstallTextEntry(std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                      const std::filesystem::path& game_root);

}  // namespace rb3pc
