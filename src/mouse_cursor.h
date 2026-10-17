// mouse_cursor: auto (hidden after idle or a pad/key press), show, hide.

#pragma once

#include <memory>
#include <string>

namespace rex::ui {
class ImGuiDialog;
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rb3pc {

// mouse_cursor's values, in the launcher's order.
constexpr const char* kMouseCursorModes[] = {"auto", "show", "hide"};

std::string MouseCursorMode();
// Saved as mouse_cursor; applies straight away once the window has one.
void SetMouseCursorMode(const std::string& mode);

// A controller button went down (any thread).
void NoteControllerPress();

// Invisible per-frame dialog. UI thread.
std::unique_ptr<rex::ui::ImGuiDialog> CreateMouseCursorController(rex::ui::ImGuiDrawer* drawer,
                                                                  rex::ui::Window* window);

}  // namespace rb3pc
