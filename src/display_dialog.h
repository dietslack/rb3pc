// Display options: FPS counter overlay (F5), display settings window (F8).

#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace rb3pc {

struct DisplayHooks {
  std::function<bool()> is_fullscreen;
  std::function<void(bool)> set_fullscreen;
  std::filesystem::path config_path;  // where settings are saved
};

// Always-present overlay that shows the game's frame rate while show_fps is
// on. Not in rb3pc-dx: show_fps switches Deluxe's own counter there.
std::unique_ptr<rex::ui::ImGuiDialog> CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer);

std::unique_ptr<rex::ui::ImGuiDialog> CreateDisplayDialog(rex::ui::ImGuiDrawer* drawer,
                                                          DisplayHooks hooks);

// Flips show_fps and saves it (not in rb3pc-dx).
void ToggleFpsCounter(const std::filesystem::path& config_path);

}  // namespace rb3pc
