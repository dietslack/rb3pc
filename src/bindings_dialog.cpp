#include "bindings_dialog.h"

#include <chrono>
#include <iterator>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

#include "input_bindings.h"
#include "instrument_input.h"

namespace rb3pc {

namespace {

constexpr const char* kTypes[] = {"auto", "gamepad", "guitar", "drums", "none"};
constexpr auto kCaptureTimeout = std::chrono::seconds(10);

class BindingsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit BindingsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowSize(ImVec2(640, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Controller bindings##rb3pc", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    auto controllers = ConnectedControllers();
    if (controllers.empty()) {
      ImGui::TextUnformatted("No controllers connected.");
      ImGui::End();
      return;
    }
    if (selected_ >= controllers.size()) selected_ = 0;

    // Controller picker.
    auto label = [](const ControllerInfo& c) {
      return std::to_string(c.index + 1) + ": " + c.name + " (" + InstrumentName(c.instrument) +
             ")";
    };
    if (ImGui::BeginCombo("Controller", label(controllers[selected_]).c_str())) {
      for (size_t i = 0; i < controllers.size(); ++i) {
        if (ImGui::Selectable(label(controllers[i]).c_str(), i == selected_)) {
          selected_ = i;
          capture_.reset();
        }
      }
      ImGui::EndCombo();
    }
    const ControllerInfo& controller = controllers[selected_];

    // Instrument type (controller_type_N).
    int type_index = 0;
    for (int i = 0; i < int(std::size(kTypes)); ++i) {
      if (controller.type_setting == kTypes[i]) type_index = i;
    }
    if (ImGui::Combo("Instrument type", &type_index, kTypes, int(std::size(kTypes)))) {
      SetControllerType(controller.index, kTypes[type_index]);
    }
    ImGui::TextDisabled("RB3 sees: %s. A standard gamepad can only sing and navigate menus.",
                        InstrumentName(controller.instrument));

    SDL_Joystick* joystick =
        controller.sdl_id ? SDL_GetJoystickFromID(controller.sdl_id) : nullptr;
    auto profile = BindingStore::Get().Find(controller.guid);

    if (controller.instrument == Instrument::kGuitar) {
      bool disable_tilt = profile && profile->disable_tilt;
      if (ImGui::Checkbox("Disable tilt", &disable_tilt)) {
        // No profile yet: start from SDL's mapping.
        Profile updated = profile ? *profile : DefaultProfile(controller.sdl_id);
        updated.disable_tilt = disable_tilt;
        BindingStore::Get().Save(updated);
        profile = updated;
      }
      ImGui::SameLine();
      ImGui::TextDisabled("(tilting won't deploy overdrive; use Select instead)");
    }
    ImGui::Separator();

    if (!profile) {
      ImGui::TextWrapped(
          "This controller uses SDL's built-in mapping. Customize it to rebind any input.");
      if (ImGui::Button("Customize bindings")) {
        BindingStore::Get().Save(DefaultProfile(controller.sdl_id));
      }
    } else {
      DrawProfile(*profile, controller, joystick);
    }

    if (joystick && ImGui::CollapsingHeader("Raw input (what the controller sends)")) {
      DrawRawInput(joystick);
    }
    ImGui::End();
  }

 private:
  void DrawProfile(Profile profile, const ControllerInfo& controller, SDL_Joystick* joystick) {
    if (capture_ && capture_guid_ == controller.guid) {
      if (auto source = capture_->Poll(joystick)) {
        profile.sources[size_t(capture_output_)] = *source;
        BindingStore::Get().Save(profile);
        capture_.reset();
      } else if (std::chrono::steady_clock::now() - capture_start_ > kCaptureTimeout) {
        capture_.reset();
      }
    }

    PadState live = Evaluate(profile, joystick);
    ImGui::TextDisabled("Click Bind, then press or move the control. Saved automatically.");
    if (ImGui::BeginTable("bindings", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableSetupColumn("Bound to", ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthStretch, 0.8f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < kOutputCount; ++i) {
        auto output = Output(i);
        ImGui::PushID(int(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(OutputLabel(output, controller.instrument).c_str());
        ImGui::TableNextColumn();
        bool capturing = capture_ && capture_guid_ == controller.guid && capture_output_ == output;
        if (capturing) {
          ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press or move a control...");
        } else {
          ImGui::TextUnformatted(profile.sources[i].Describe().c_str());
        }
        ImGui::TableNextColumn();
        DrawLiveValue(output, live);
        ImGui::TableNextColumn();
        if (capturing) {
          if (ImGui::SmallButton("Cancel")) capture_.reset();
        } else {
          if (ImGui::SmallButton("Bind")) {
            capture_.emplace(joystick, output, controller.instrument);
            capture_guid_ = controller.guid;
            capture_output_ = output;
            capture_start_ = std::chrono::steady_clock::now();
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear")) {
            profile.sources[i] = {};
            BindingStore::Get().Save(profile);
          }
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    if (ImGui::Button("Reset to SDL defaults")) {
      auto defaults = DefaultProfile(controller.sdl_id);
      BindingStore::Get().Save(defaults);
      capture_.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove custom bindings")) {
      BindingStore::Get().Remove(controller.guid);
      capture_.reset();
    }
  }

  static void DrawLiveValue(Output output, const PadState& live) {
    switch (output) {
      case Output::kLeftX: ImGui::Text("%d", live.thumb_lx); return;
      case Output::kLeftY: ImGui::Text("%d", live.thumb_ly); return;
      case Output::kRightX: ImGui::Text("%d", live.thumb_rx); return;
      case Output::kRightY: ImGui::Text("%d", live.thumb_ry); return;
      case Output::kLeftTrigger: ImGui::Text("%d", live.left_trigger); return;
      case Output::kRightTrigger: ImGui::Text("%d", live.right_trigger); return;
      default: break;
    }
    bool held = (live.buttons & OutputButton(output)) != 0;
    if (held) {
      ImGui::TextColored(ImVec4(0.3f, 1, 0.4f, 1), "pressed");
    } else {
      ImGui::TextDisabled("-");
    }
  }

  static void DrawRawInput(SDL_Joystick* joystick) {
    std::string buttons;
    for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); ++i) {
      if (SDL_GetJoystickButton(joystick, i)) buttons += std::to_string(i) + " ";
    }
    ImGui::Text("Buttons held: %s", buttons.empty() ? "none" : buttons.c_str());
    for (int i = 0; i < SDL_GetNumJoystickHats(joystick); ++i) {
      ImGui::Text("Hat %d: %d", i, SDL_GetJoystickHat(joystick, i));
    }
    if (ImGui::BeginTable("axes", 4, ImGuiTableFlags_SizingStretchSame)) {
      for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i) {
        ImGui::TableNextColumn();
        ImGui::Text("Axis %d: %d", i, SDL_GetJoystickAxis(joystick, i));
      }
      ImGui::EndTable();
    }
  }

  size_t selected_ = 0;
  std::optional<Capture> capture_;
  std::string capture_guid_;
  Output capture_output_ = Output::kA;
  std::chrono::steady_clock::time_point capture_start_;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateBindingsDialog(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<BindingsDialog>(drawer);
}

}  // namespace rb3pc
