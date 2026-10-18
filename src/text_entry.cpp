#include "text_entry.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/kernel/xam/keyboard_ui.h>

#include "menu_pads.h"
#include "rb3_ui.h"

namespace rb3pc {

namespace {

// Strum order, wrapping.
constexpr std::string_view kLetters =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,'-!?&";

std::string Upper(std::string text) {
  for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

class TextEntry final : public rex::kernel::xam::KeyboardUi {
 public:
  TextEntry(std::shared_ptr<Rb3UiRenderer> ui, const rex::kernel::xam::KeyboardUiRequest& request)
      : ui_(std::move(ui)),
        title_(request.title),
        description_(request.description),
        max_length_(request.max_length ? request.max_length : 64) {
    for (char c : request.default_text) {
      if (c >= 0x20 && c < 0x7F && text_.size() < max_length_) text_ += c;
    }
  }

  bool cancelled() const override { return cancelled_; }

  std::string text() const override {
    std::string text = text_;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
  }

  bool Draw(ImGuiIO& io) override {
    ui_->Update();
    HandleInput(io);
    if (!finished_) Render(io);
    return finished_;
  }

 private:
  // Changes the last letter.
  void Step(int delta) {
    if (text_.empty()) {
      text_ += delta > 0 ? kLetters.front() : kLetters.back();
      return;
    }
    char& c = text_.back();
    size_t at = kLetters.find(c);
    int count = int(kLetters.size());
    int index = at == std::string_view::npos ? 0 : (int(at) + delta % count + count) % count;
    c = kLetters[size_t(index)];
  }

  void NextLetter() {
    if (text_.size() >= max_length_) return;
    text_ += text_.empty() || text_.back() == ' ' ? 'A' : 'a';
  }

  void Erase() {
    if (!text_.empty()) text_.pop_back();
  }

  void Finish(bool cancelled) {
    cancelled_ = cancelled;
    finished_ = true;
  }

  void HandleInput(ImGuiIO& io) {
    if (!pads_) pads_.emplace();
    uint32_t pressed = repeater_.Update(pads_->Poll());
    auto key = [](ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat); };
    if (key(ImGuiKey_UpArrow) || io.MouseWheel > 0) pressed |= kUp;
    if (key(ImGuiKey_DownArrow) || io.MouseWheel < 0) pressed |= kDown;
    if (key(ImGuiKey_RightArrow) || key(ImGuiKey_Tab)) pressed |= kRight;
    if (key(ImGuiKey_Backspace)) pressed |= kBack;
    if (key(ImGuiKey_Enter, false) || key(ImGuiKey_KeypadEnter, false)) pressed |= kStart;
    if (key(ImGuiKey_Escape, false)) pressed |= kSelect;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (done_rect_.Contains(io.MousePos)) pressed |= kStart;
      if (cancel_rect_.Contains(io.MousePos)) pressed |= kSelect;
    }

    for (ImWchar c : io.InputQueueCharacters) {
      if (c >= 0x20 && c < 0x7F && text_.size() < max_length_) text_ += char(c);
    }
    io.InputQueueCharacters.resize(0);

    if (pressed & kStart) return Finish(false);
    if (pressed & kSelect) return Finish(true);
    if (pressed & kUp) Step(+1);
    if (pressed & kDown) Step(-1);
    if (pressed & (kConfirm | kRight)) NextLetter();
    if (pressed & kBack) Erase();
  }

  void Render(ImGuiIO& io) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 display = io.DisplaySize;
    float s = std::max(display.y / 720.0f, 0.5f);

    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 150));

    float w = std::min(700.0f * s, display.x - 40.0f * s), h = 330.0f * s;
    ImVec2 p0((display.x - w) * 0.5f, (display.y - h) * 0.5f), p1(p0.x + w, p0.y + h);
    draw->AddRectFilledMultiColor(p0, p1, IM_COL32(12, 50, 64, 250), IM_COL32(12, 50, 64, 250),
                                  IM_COL32(4, 18, 26, 250), IM_COL32(4, 18, 26, 250));
    ui_->DrawTiled(draw, "wallpaper", p0, p1, 300.0f * s, IM_COL32(150, 235, 245, 110));
    float band = 74.0f * s;
    draw->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + band), IM_COL32(0, 0, 0, 170),
                                  IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 90),
                                  IM_COL32(0, 0, 0, 90));
    draw->AddRect(p0, p1, IM_COL32(150, 200, 210, 120), 0, 0, 2.0f * s);
    draw->AddLine(ImVec2(p0.x, p0.y + band), ImVec2(p1.x, p0.y + band), kUiAccent, 2.0f * s);

    float pad = 26.0f * s;
    std::string title = Upper(title_.empty() ? "Enter text" : title_);
    ui_->DrawTextShadowed(draw, UiFont::kHub, title, 34.0f * s, p0.x + pad, p0.y + 10.0f * s, kUiText);
    if (!description_.empty()) {
      std::string description = description_;
      float size = 17.0f * s;
      while (description.size() > 4 && ui_->TextWidth(UiFont::kDisplay, description, size) > w - 2 * pad) {
        description = description.substr(0, description.size() - 4) + "...";
      }
      ui_->DrawText(draw, UiFont::kDisplay, description, size, p0.x + pad, p0.y + 48.0f * s, kUiMuted);
    }

    // Slot = letter width + `gap`, so neighbouring underlines and the
    // highlight bar don't overlap.
    float size = 46.0f * s, slot_h = 62.0f * s, gap = 10.0f * s;
    float row_y = p0.y + band + 72.0f * s;
    std::vector<float> widths;
    float total = 0;
    std::string shown = text_.empty() ? std::string(" ") : text_;
    for (char c : shown) {
      float cw = std::max(ui_->TextWidth(UiFont::kDisplay, std::string(1, c), size), 12.0f * s);
      widths.push_back(cw);
      total += cw + gap;
    }
    float room = w - 2 * pad;
    float x = total <= room ? (display.x - total) * 0.5f : p1.x - pad - total;
    ImVec2 clip0(p0.x + pad * 0.5f, p0.y), clip1(p1.x - pad * 0.5f, p1.y);
    draw->PushClipRect(clip0, clip1, true);
    for (size_t i = 0; i < shown.size(); ++i) {
      float cw = widths[i];
      bool current = i + 1 == shown.size();
      float half = gap * 0.5f;
      ImVec2 a(x - half, row_y), b(x + cw + half, row_y + slot_h);
      if (current) {
        ui_->DrawAccentBar(draw, a, b, s);
        float cx = (a.x + b.x) * 0.5f, t = 10.0f * s;
        draw->AddTriangleFilled(ImVec2(cx, a.y - 2 * t), ImVec2(cx - t, a.y - t * 0.6f),
                                ImVec2(cx + t, a.y - t * 0.6f), kUiAccent);
        draw->AddTriangleFilled(ImVec2(cx, b.y + 2 * t), ImVec2(cx - t, b.y + t * 0.6f),
                                ImVec2(cx + t, b.y + t * 0.6f), kUiAccent);
      } else {
        draw->AddRectFilled(ImVec2(x - 1.0f * s, b.y - 3.0f * s), ImVec2(x + cw + 1.0f * s, b.y),
                            IM_COL32(150, 200, 210, 90));
      }
      std::string letter(1, shown[i]);
      float ty = row_y + (slot_h - size) * 0.5f;
      if (current) {
        ui_->DrawText(draw, UiFont::kDisplay, letter, size, x, ty, kUiDark);
      } else {
        ui_->DrawTextShadowed(draw, UiFont::kDisplay, letter, size, x, ty, kUiText);
      }
      x += cw + gap;
    }
    draw->PopClipRect();
    std::string count = std::to_string(text_.size()) + " / " + std::to_string(max_length_);
    float cs = 15.0f * s;
    ui_->DrawText(draw, UiFont::kDisplay, count, cs,
                  p1.x - pad - ui_->TextWidth(UiFont::kDisplay, count, cs), row_y + slot_h + 26.0f * s, kUiMuted);

    struct Hint {
      const char* icon;  // buttons font: g / r fret bars
      std::string text;
      HitRect* rect;
    };
    std::vector<Hint> hints = {{nullptr, "STRUM: CHANGE", nullptr},
                               {"g", "NEXT LETTER", nullptr},
                               {"r", "ERASE", nullptr},
                               {nullptr, "START: DONE", &done_rect_},
                               {nullptr, "BACK: CANCEL", &cancel_rect_}};
    float hs = 17.0f * s, icon = hs * 1.5f, hgap = 22.0f * s;
    float hw = 0;
    for (const auto& hint : hints) hw += ui_->TextWidth(UiFont::kHub, hint.text, hs) + (hint.icon ? icon : 0);
    hw += hgap * float(hints.size() - 1);
    float bar_y = p1.y - 52.0f * s;
    draw->AddRectFilled(ImVec2(p0.x, bar_y - 8.0f * s), p1, IM_COL32(0, 0, 0, 120));
    float hx = (display.x - hw) * 0.5f, hy = bar_y + 4.0f * s;
    for (const auto& hint : hints) {
      float start = hx;
      if (hint.icon) {
        ui_->DrawText(draw, UiFont::kButtons, hint.icon, icon, hx, hy + (hs - icon) * 0.5f, IM_COL32_WHITE);
        hx += icon;
      }
      float tw = ui_->TextWidth(UiFont::kHub, hint.text, hs);
      bool hovered = hint.rect && hint.rect->Contains(io.MousePos);
      ui_->DrawTextShadowed(draw, UiFont::kHub, hint.text, hs, hx, hy, hovered ? kUiAccent : kUiText);
      hx += tw;
      if (hint.rect) *hint.rect = HitRect(ImVec2(start - 4.0f * s, hy - 4.0f * s), ImVec2(hx + 4.0f * s, hy + hs + 4.0f * s));
      hx += hgap;
    }
    std::string keys = "OR TYPE ON THE KEYBOARD  -  ENTER: DONE  -  ESC: CANCEL";
    float ks = 13.0f * s;
    ui_->DrawText(draw, UiFont::kHub, keys, ks, (display.x - ui_->TextWidth(UiFont::kHub, keys, ks)) * 0.5f,
                  bar_y - 30.0f * s, kUiMuted);
  }

  struct HitRect {
    ImVec2 min{}, max{};
    HitRect() = default;
    HitRect(ImVec2 a, ImVec2 b) : min(a), max(b) {}
    bool Contains(ImVec2 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
  };

  std::shared_ptr<Rb3UiRenderer> ui_;
  std::string title_, description_;
  size_t max_length_;
  std::string text_;
  bool cancelled_ = true, finished_ = false;
  // Opened at first draw on the UI thread, not the constructing game thread.
  std::optional<MenuPads> pads_;
  Repeater repeater_;
  HitRect done_rect_, cancel_rect_;
};

}  // namespace

std::unique_ptr<rex::kernel::xam::KeyboardUi> CreateTextEntry(
    std::shared_ptr<Rb3UiRenderer> ui, const rex::kernel::xam::KeyboardUiRequest& request) {
  return std::make_unique<TextEntry>(std::move(ui), request);
}

void InstallTextEntry(std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                      const std::filesystem::path& game_root) {
  auto ui = std::make_shared<Rb3UiRenderer>(std::move(immediate_drawer), game_root);
  rex::kernel::xam::SetKeyboardUiFactory([ui](const rex::kernel::xam::KeyboardUiRequest& request) {
    return std::make_unique<TextEntry>(ui, request);
  });
}

}  // namespace rb3pc
