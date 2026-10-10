#include "display_dialog.h"

#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/system/shader_storage_progress.h>
#include <rex/ui/imgui_dialog.h>

#include "display_options.h"
#include "exe.h"
#include "frame_stats.h"
#include "launcher.h"
#include "profile.h"

REXCVAR_DEFINE_BOOL(show_fps, false, "rb3pc", "Show the game's frame rate in the corner");

namespace rb3pc {

namespace {

int GetInt(const char* name) { return GetIntFlag(name); }

bool GetBool(const char* name) { return GetBoolFlag(name); }

void Save(const std::filesystem::path& path) {
  if (!path.empty()) rex::cvar::SaveConfig(path);
}

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    // Nothing to count while the shader cache loading screen is up. Deluxe
    // shows its own counter (set from show_fps at boot, src/deluxe.h).
    if (!REXCVAR_GET(show_fps) || rex::system::shader_storage_progress().active ||
        LauncherActive() || rb3pc::exe::kDeluxe) {
      return;
    }
    auto stats = GuestFrameStats();
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##rb3pc_fps", nullptr, flags)) {
      ImGui::Text("%.0f FPS  %.1f ms", stats.fps, stats.frame_time_ms);
    }
    ImGui::End();
  }
};

class DisplayDialog final : public rex::ui::ImGuiDialog {
 public:
  DisplayDialog(rex::ui::ImGuiDrawer* drawer, DisplayHooks hooks)
      : ImGuiDialog(drawer),
        hooks_(std::move(hooks)),
        start_scale_(GetInt("resolution_scale")),
        start_refresh_(GetInt("video_mode_refresh_rate")),
        start_vsync_(GetBool("vsync")),
        start_profile_(ProfileName()) {
    std::string name = rex::cvar::GetFlagByName("profile_name");
    name.copy(profile_buf_, sizeof(profile_buf_) - 1);
  }

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Display##rb3pc", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    bool fullscreen = hooks_.is_fullscreen();
    if (ImGui::Checkbox("Fullscreen (F11)", &fullscreen)) {
      hooks_.set_fullscreen(fullscreen);
    }

    bool show_fps = REXCVAR_GET(show_fps);
    if (exe::kDeluxe) {
      ImGui::TextDisabled("FPS counter: Deluxe's own (set in the launcher)");
    } else if (ImGui::Checkbox("Show FPS counter (F5)", &show_fps)) {
      rex::cvar::SetFlagByName("show_fps", show_fps ? "true" : "false");
      Save(hooks_.config_path);
    }

    ImGui::Separator();
    int scale_index = ScaleIndex();
    if (ImGui::Combo("Internal resolution", &scale_index, kScaleLabels, kScaleCount)) {
      SetScaleIndex(scale_index);
      Save(hooks_.config_path);
    }

    int limit_index = LimitIndex();
    if (ImGui::Combo("Frame-rate limit", &limit_index, kLimitLabels, kLimitCount)) {
      SetLimitIndex(limit_index);
      Save(hooks_.config_path);
    }
    ImGui::TextDisabled("Above 60 is experimental: RB3 was built for 60.");

    ImGui::Separator();
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputTextWithHint("Profile name", start_profile_.c_str(), profile_buf_,
                                 kMaxProfileNameLength + 1)) {
      rex::cvar::SetFlagByName("profile_name", SanitizeProfileName(profile_buf_));
      Save(hooks_.config_path);
    }
    ImGui::TextDisabled("Shown in-game. Empty uses your login name. Saves are kept.");

    bool restart = GetInt("resolution_scale") != start_scale_ ||
                   GetInt("video_mode_refresh_rate") != start_refresh_ ||
                   GetBool("vsync") != start_vsync_;
    if (restart) {
      ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                         "Restart the game to apply the resolution / frame-rate change.");
    }
    if (ProfileName() != start_profile_) {
      ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1),
                         "Restart the game to sign in as \"%s\".", ProfileName().c_str());
    }
    ImGui::End();
  }

 private:
  DisplayHooks hooks_;
  int start_scale_;
  int start_refresh_;
  bool start_vsync_;
  std::string start_profile_;
  char profile_buf_[kMaxProfileNameLength + 1] = {};
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<FpsOverlay>(drawer);
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateDisplayDialog(rex::ui::ImGuiDrawer* drawer,
                                                          DisplayHooks hooks) {
  return std::make_unique<DisplayDialog>(drawer, std::move(hooks));
}

void ToggleFpsCounter(const std::filesystem::path& config_path) {
  if (exe::kDeluxe) return;  // Deluxe's own counter, set from the launcher at boot
  rex::cvar::SetFlagByName("show_fps", REXCVAR_GET(show_fps) ? "false" : "true");
  Save(config_path);
}

}  // namespace rb3pc
