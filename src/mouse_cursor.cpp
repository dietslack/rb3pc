#include "mouse_cursor.h"

#include <atomic>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

REXCVAR_DEFINE_STRING(mouse_cursor, "auto", "rb3pc",
                      "The mouse cursor over the window: auto (hidden a few seconds after the "
                      "mouse last moved, and on a controller or keyboard press), show, or hide")
    .allowed({"auto", "show", "hide"});

namespace rb3pc {

namespace {

std::atomic<bool> g_controller_pressed{false};

using Visibility = rex::ui::Window::CursorVisibility;

Visibility VisibilityFor(const std::string& mode) {
  if (mode == "show") return Visibility::kVisible;
  if (mode == "hide") return Visibility::kHidden;
  return Visibility::kAutoHidden;
}

class MouseCursorController final : public rex::ui::ImGuiDialog {
 public:
  MouseCursorController(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window)
      : ImGuiDialog(drawer), window_(window) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    bool pressed = g_controller_pressed.exchange(false);
    // Mouse capture (mouse look) sets its own visibility and restores the
    // earlier one after: leave it be meanwhile.
    if (window_->IsMouseCaptureRequested()) return;
    Visibility wanted = VisibilityFor(MouseCursorMode());
    if (window_->GetCursorVisibility() != wanted) {
      window_->SetCursorVisibility(wanted);
      return;
    }
    if (wanted != Visibility::kAutoHidden) return;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart && !pressed; ++key) {
      pressed = ImGui::IsKeyPressed(ImGuiKey(key), false);
    }
    if (pressed) {
      // Entering auto-hide hides the cursor at once (and stops the idle
      // timer); the next mouse motion shows it again.
      window_->SetCursorVisibility(Visibility::kHidden);
      window_->SetCursorVisibility(Visibility::kAutoHidden);
    }
  }

 private:
  rex::ui::Window* window_;
};

}  // namespace

std::string MouseCursorMode() { return REXCVAR_GET(mouse_cursor); }

void SetMouseCursorMode(const std::string& mode) {
  rex::cvar::SetFlagByName("mouse_cursor", mode);
}

void NoteControllerPress() { g_controller_pressed.store(true, std::memory_order_relaxed); }

std::unique_ptr<rex::ui::ImGuiDialog> CreateMouseCursorController(rex::ui::ImGuiDrawer* drawer,
                                                                  rex::ui::Window* window) {
  return std::make_unique<MouseCursorController>(drawer, window);
}

}  // namespace rb3pc
