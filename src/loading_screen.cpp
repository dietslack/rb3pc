#include "loading_screen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/system/shader_storage_progress.h>
#include <rex/ui/imgui_dialog.h>

#include "rb3_ui.h"

namespace rb3pc {

namespace {

// Don't flash the screen for the ~1 s it takes when the cache is warm.
constexpr auto kShowDelay = std::chrono::milliseconds(1500);

class LoadingScreen final : public rex::ui::ImGuiDialog {
 public:
  LoadingScreen(rex::ui::ImGuiDrawer* drawer,
                std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                std::filesystem::path game_root)
      : ImGuiDialog(drawer), ui_(std::move(immediate_drawer), std::move(game_root)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& progress = rex::system::shader_storage_progress();
    auto now = std::chrono::steady_clock::now();
    if (!progress.active) {
      active_since_.reset();
      return;
    }
    if (!active_since_) active_since_ = now;
    if (now - *active_since_ < kShowDelay) return;
    ui_.Update();

    ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (ImGui::Begin("##rb3pc_loading", nullptr, flags)) {
      Draw(ImGui::GetWindowDrawList(), display, progress);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  void Draw(ImDrawList* draw, ImVec2 display, rex::system::ShaderStorageProgress& progress) {
    float scale = std::max(display.y / 720.0f, 0.5f);  // layout designed at 720p
    float cx = display.x * 0.5f;
    auto now = std::chrono::steady_clock::now();
    float seconds = std::chrono::duration<float>(now - *active_since_).count();
    float logo_w = std::min(560.0f * scale, display.x * 0.75f);
    float logo_h = logo_w / ui_.logo_aspect();
    float block_h = logo_h + (46 + 36 + 22 + 22 + 12 + 21) * scale;
    float logo_y = std::max((display.y - 58.0f * scale - block_h) * 0.5f, 20.0f * scale);
    ui_.DrawBackdrop(draw, display, scale, ImVec2(cx, logo_y + logo_h * 0.5f));
    ui_.DrawLogo(draw, ImVec2(cx - logo_w * 0.5f, logo_y),
                 ImVec2(cx + logo_w * 0.5f, logo_y + logo_h));

    uint32_t total = progress.total;
    uint32_t done = std::min<uint32_t>(progress.done, total);

    float heading = 36.0f * scale;
    float heading_y = logo_y + logo_h + 46.0f * scale;
    std::string title = total ? "BUILDING SHADER CACHE" : "PREPARING SHADERS";
    ui_.DrawTextShadowed(draw, UiFont::kHub, title, heading,
                         cx - ui_.TextWidth(UiFont::kHub, title, heading) * 0.5f, heading_y,
                         kUiText);

    float bar_w = std::min(620.0f * scale, display.x * 0.8f);
    float bar_h = 22.0f * scale;
    float bar_y = heading_y + heading + 22.0f * scale;
    float inset = 3.0f * scale;
    ImVec2 bar_a(cx - bar_w * 0.5f, bar_y);
    ImVec2 bar_b(cx + bar_w * 0.5f, bar_y + bar_h);
    draw->AddRectFilled(bar_a, bar_b, IM_COL32(0, 0, 0, 150), 7.0f * scale);
    draw->AddRect(bar_a, bar_b, IM_COL32(150, 230, 240, 70), 7.0f * scale, 0,
                  std::max(1.0f, 1.5f * scale));
    ImVec2 fill_a(bar_a.x + inset, bar_a.y + inset);
    float fill_w = bar_w - inset * 2, fill_h = bar_h - inset * 2;
    float min_fill = 12.0f * scale;  // so the bar's rounded ends always show
    if (total) {
      float fill = fill_w * float(done) / float(total);
      if (done) {
        ui_.DrawAccentBar(draw, fill_a,
                          ImVec2(fill_a.x + std::max(fill, min_fill), fill_a.y + fill_h), scale);
      }
    } else {
      // Indeterminate.
      float t = std::fmod(seconds * 0.6f, 1.0f);
      float seg = fill_w * 0.22f;
      float x0 = std::max(fill_a.x + (fill_w + seg) * t - seg, fill_a.x);
      float x1 = std::min(fill_a.x + (fill_w + seg) * t, fill_a.x + fill_w);
      if (x1 - x0 >= min_fill) {
        ui_.DrawAccentBar(draw, ImVec2(x0, fill_a.y), ImVec2(x1, fill_a.y + fill_h), scale);
      }
    }

    float small = 21.0f * scale;
    float info_y = bar_b.y + 12.0f * scale;
    if (total) {
      std::string count = std::to_string(done) + " OF " + std::to_string(total);
      ui_.DrawTextShadowed(draw, UiFont::kHub, count, small, bar_a.x, info_y, kUiMuted);
      std::string percent = std::to_string(uint32_t(100.0 * done / total)) + "%";
      ui_.DrawTextShadowed(draw, UiFont::kHub, percent, small,
                           bar_b.x - ui_.TextWidth(UiFont::kHub, percent, small), info_y,
                           kUiAccent);
      std::string left = TimeLeft(done, total, now);
      if (!left.empty()) {
        ui_.DrawTextShadowed(draw, UiFont::kHub, left, small,
                             cx - ui_.TextWidth(UiFont::kHub, left, small) * 0.5f, info_y,
                             kUiText);
      }
    }

    float strip_h = 44.0f * scale;
    float strip_y = display.y - strip_h - 14.0f * scale;
    draw->AddRectFilledMultiColor(ImVec2(0, strip_y), ImVec2(display.x, strip_y + strip_h),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 140));
    float note = 18.0f * scale;
    std::string text =
        "ONLY ON FIRST LAUNCH, AT A NEW RESOLUTION OR AFTER A GRAPHICS DRIVER UPDATE";
    ui_.DrawTextShadowed(draw, UiFont::kHub, text, note,
                         cx - ui_.TextWidth(UiFont::kHub, text, note) * 0.5f,
                         strip_y + (strip_h - note) * 0.5f, kUiText);
  }

  // "ABOUT 3 MINUTES LEFT" from the rate so far, once it's settled.
  std::string TimeLeft(uint32_t done, uint32_t total,
                       std::chrono::steady_clock::time_point now) {
    if (total != rate_total_) {
      // New phase (stored pipelines, then warm-up).
      rate_total_ = total;
      rate_start_ = now;
      rate_start_done_ = done;
    }
    float elapsed = std::chrono::duration<float>(now - rate_start_).count();
    uint32_t progressed = done - std::min(done, rate_start_done_);
    if (elapsed < 5.0f || progressed < total / 50 || !progressed || done >= total) return {};
    float left = elapsed * float(total - done) / float(progressed);
    if (left < 60.0f) return "LESS THAN A MINUTE LEFT";
    int minutes = int(std::ceil(left / 60.0f));
    return "ABOUT " + std::to_string(minutes) + (minutes == 1 ? " MINUTE" : " MINUTES") +
           " LEFT";
  }

  Rb3UiRenderer ui_;
  std::optional<std::chrono::steady_clock::time_point> active_since_;
  uint32_t rate_total_ = 0;
  uint32_t rate_start_done_ = 0;
  std::chrono::steady_clock::time_point rate_start_;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateLoadingScreen(
    rex::ui::ImGuiDrawer* drawer, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
    std::filesystem::path game_root) {
  return std::make_unique<LoadingScreen>(drawer, std::move(immediate_drawer),
                                         std::move(game_root));
}

}  // namespace rb3pc
