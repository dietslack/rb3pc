// Mouse pointer in RB3's menus (src/mouse_menus.cpp).

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

// Always-present overlay that highlights what the pointer is over (help bar
// labels, scrollbar thumbs), in the style of the main menu's: a yellow bar
// with the label's text redrawn black. Reads the game's fonts and art from
// `game_root`'s ARK in the background. Null in rb3pc-dx, which has no pointer
// support yet.
std::unique_ptr<rex::ui::ImGuiDialog> CreatePointerOverlay(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    const std::filesystem::path& game_root);

}  // namespace rb3pc
