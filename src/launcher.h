// Pre-boot settings screen. Runs before the runtime exists, so startup-only
// settings apply without a restart. Controllers are read from SDL through
// their bindings profiles.

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

struct LauncherHooks {
  std::function<bool()> is_fullscreen;
  std::function<void(bool)> set_fullscreen;
  std::function<void()> play;  // called once, from the UI thread
  std::function<void()> quit;
  // Nothing else repaints before the game runs; pads are polled while drawing.
  std::function<void()> request_paint;
  std::filesystem::path config_path;
};

// Not when an input script is driving or the other executable handed over.
bool LauncherAllowed();
bool ShowLauncherSetting();  // show_launcher

// With show_launcher off, holding Shift while the game opens brings the
// launcher up anyway. The held keys only reach the window with its keyboard
// focus, so this draws nothing until then (or a timeout) and calls done once,
// from the UI thread, with whether Shift was held.
std::unique_ptr<rex::ui::ImGuiDialog> CreateShiftCheck(rex::ui::ImGuiDrawer* drawer,
                                                       std::function<void()> request_paint,
                                                       std::function<void(bool)> done);

bool LauncherActive();

std::unique_ptr<rex::ui::ImGuiDialog> CreateLauncher(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    std::filesystem::path game_root, LauncherHooks hooks);

}  // namespace rb3pc
