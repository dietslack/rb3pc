// Controller bindings window (F6 by default).

#pragma once

#include <memory>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
}  // namespace rex::ui

namespace rb3pc {

std::unique_ptr<rex::ui::ImGuiDialog> CreateBindingsDialog(rex::ui::ImGuiDrawer* drawer);

}  // namespace rb3pc
