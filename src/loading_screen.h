// RB3-styled loading screen shown while the shader/pipeline cache is built
// (first launch, new shaders, or after a graphics driver update).

#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace rb3pc {

// Reads the logo and the Pentatonic font from the game's ARK under
// `game_root` in the background.
std::unique_ptr<rex::ui::ImGuiDialog> CreateLoadingScreen(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    std::filesystem::path game_root);

}  // namespace rb3pc
