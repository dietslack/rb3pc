#include "rb3_ui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>

#include <rex/ui/immediate_drawer.h>

namespace rb3pc {

namespace {

// Heavy condensed sans, for when the game font can't be read.
constexpr const char* kFallbackFonts[] = {
    "/usr/share/fonts/google-noto/NotoSans-CondensedBlack.ttf",
    "/usr/share/fonts/noto/NotoSans-CondensedBlack.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-CondensedBlack.ttf",
};
ImFont* g_fallback_font = nullptr;

ImFont* FallbackFont() { return g_fallback_font ? g_fallback_font : ImGui::GetFont(); }

constexpr const char* kFonts = "ui/resource/fonts/gen/";
constexpr const char* kOvershell = "ui/overshell/gen/overshell_player_common.milo_xbox";

std::vector<UiImageRequest> ImageRequests() {
  std::string display = std::string(kFonts) + "pentatonic_display.milo_xbox";
  return {
      // Greyscale; tinted at draw time.
      {"highlight_bar", display, "highlight_bar_color.tex"},
      {"highlight_glow", display, "highlight_bar_glow.tex"},
      {"wallpaper", "ui/global/gen/dialog_common.milo_xbox", "dialog_wallpaper.tex"},
      {"sunburst", kOvershell, "sunburst.tex"},
      {"checkbox", kOvershell, "checkbox.tex"},
      {"check", kOvershell, "check.tex"},
      // Right-pointing, white; marks a launcher category that opens.
      {"chevron", "ui/practice/gen/practice_panel.milo_xbox", "dpad_arrow.tex"},
  };
}

std::vector<UiFontRequest> FontRequests() {
  std::string fonts = kFonts;
  return {
      {"display", fonts + "pentatonic_display.milo_xbox", "Pentatonic_Regular_(9_00)4x.font",
       "Pentatonic_Regular_(9_00)4x.tex"},
      {"hub", fonts + "pentatonic.milo_xbox", "Pentatonic_Hub(5_00)4x.font",
       "Pentatonic_Hub(5_00)4x.tex"},
      {"buttons", fonts + "buttons.milo_xbox", "buttons.font", "buttons.tex"},
  };
}

const char* FontKey(UiFont font) {
  switch (font) {
    case UiFont::kDisplay: return "display";
    case UiFont::kHub: return "hub";
    case UiFont::kButtons: return "buttons";
  }
  return "";
}

}  // namespace

void AddRb3UiFonts(ImFontAtlas* atlas) {
  for (const char* path : kFallbackFonts) {
    if (std::filesystem::exists(path)) {
      g_fallback_font = atlas->AddFontFromFileTTF(path, 26.0f);
      if (g_fallback_font) return;
    }
  }
}

Rb3UiRenderer::Rb3UiRenderer(std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                             std::filesystem::path game_root)
    : immediate_drawer_(std::move(immediate_drawer)),
      assets_future_(std::async(std::launch::async, [game_root = std::move(game_root)] {
        return LoadRb3UiAssets(game_root, ImageRequests(), FontRequests());
      })) {}

Rb3UiRenderer::~Rb3UiRenderer() = default;

void Rb3UiRenderer::Update() {
  if (uploaded_ || !assets_future_.valid() ||
      assets_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }
  uploaded_ = true;
  auto assets = assets_future_.get();
  auto* drawer = immediate_drawer_ ? immediate_drawer_() : nullptr;
  if (!drawer) return;
  auto make = [&](const Rgba8Image& image, bool repeat) {
    return drawer->CreateTexture(image.width, image.height,
                                 rex::ui::ImmediateTextureFilter::kLinear, repeat,
                                 image.pixels.data());
  };
  if (assets.logo) {
    logo_ = make(*assets.logo, false);
    logo_aspect_ = float(assets.logo->width) / float(assets.logo->height);
  }
  for (auto& [key, image] : assets.images) {
    // Only the wallpaper tiles; clamping keeps the others' edges clean.
    if (auto texture = make(image, key == "wallpaper")) {
      images_[key] = Texture{std::move(texture), float(image.width), float(image.height)};
    }
  }
  for (UiFont font : {UiFont::kDisplay, UiFont::kHub, UiFont::kButtons}) {
    auto it = assets.fonts.find(FontKey(font));
    if (it == assets.fonts.end()) continue;
    if (auto atlas = make(it->second.atlas, false)) {
      fonts_[int(font)] = Font{std::move(it->second), std::move(atlas)};
    }
  }
}

const Rb3UiRenderer::Font* Rb3UiRenderer::FindFont(UiFont font) const {
  auto it = fonts_.find(int(font));
  if (it != fonts_.end()) return &it->second;
  if (font == UiFont::kHub) return FindFont(UiFont::kDisplay);
  return nullptr;
}

float Rb3UiRenderer::TextWidth(UiFont font, const std::string& text, float size) const {
  const Font* f = FindFont(font);
  if (!f) return FallbackFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
  float w = 0;
  for (unsigned char c : text) {
    auto it = f->glyphs.glyphs.find(c);
    if (it != f->glyphs.glyphs.end()) w += it->second.advance * size;
  }
  return w;
}

void Rb3UiRenderer::DrawText(ImDrawList* draw, UiFont font, const std::string& text, float size,
                             float x, float y, ImU32 color) const {
  x = std::floor(x);
  y = std::floor(y);
  const Font* f = FindFont(font);
  if (!f) {
    if (font != UiFont::kButtons) {
      draw->AddText(FallbackFont(), size, ImVec2(x, y), color, text.c_str());
    }
    return;
  }
  // Metrics are in line heights; atlas U width = width * cell_v * (H / W).
  const BitmapFont& g = f->glyphs;
  float aspect = float(g.atlas.height) / float(g.atlas.width);
  auto tex = reinterpret_cast<ImTextureID>(f->atlas.get());
  for (unsigned char c : text) {
    auto it = g.glyphs.find(c);
    if (it == g.glyphs.end()) continue;
    const auto& glyph = it->second;
    if (glyph.width > 0) {
      ImVec2 uv0(glyph.u, glyph.v);
      ImVec2 uv1(glyph.u + glyph.width * g.cell_v * aspect, glyph.v + g.cell_v);
      draw->AddImage(tex, ImVec2(x, y), ImVec2(x + glyph.width * size, y + size), uv0, uv1,
                     color);
    }
    x += glyph.advance * size;
  }
}

void Rb3UiRenderer::DrawTextCentered(ImDrawList* draw, UiFont font, const std::string& text,
                                     float size, float center_x, float y, ImU32 color) const {
  DrawText(draw, font, text, size, center_x - TextWidth(font, text, size) * 0.5f, y, color);
}

void Rb3UiRenderer::DrawTextShadowed(ImDrawList* draw, UiFont font, const std::string& text,
                                     float size, float x, float y, ImU32 color) const {
  float o = std::max(1.0f, size * 0.06f);
  ImU32 shadow = IM_COL32(0, 0, 0, 150 * ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255);
  DrawText(draw, font, text, size, x + o, y + o, shadow);
  DrawText(draw, font, text, size, x, y, color);
}

void Rb3UiRenderer::DrawBackdrop(ImDrawList* draw, ImVec2 display, float scale,
                                 ImVec2 logo_center) const {
  // The wallpaper is dark blue in the file; the game shows it teal.
  draw->AddRectFilledMultiColor(ImVec2(0, 0), display, IM_COL32(10, 42, 54, 255),
                                IM_COL32(10, 42, 54, 255), IM_COL32(3, 14, 20, 255),
                                IM_COL32(3, 14, 20, 255));
  DrawTiled(draw, "wallpaper", ImVec2(0, 0), display, 300.0f * scale,
            IM_COL32(150, 235, 245, 120));
  float burst = 900.0f * scale;
  DrawImage(draw, "sunburst",
            ImVec2(logo_center.x - burst * 0.5f, logo_center.y - burst * 0.5f),
            ImVec2(logo_center.x + burst * 0.5f, logo_center.y + burst * 0.5f),
            IM_COL32(140, 230, 240, 26));
  float band = display.y * 0.22f;
  ImU32 dark = IM_COL32(0, 0, 0, 170), clear = IM_COL32(0, 0, 0, 0);
  draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, band), dark, dark, clear, clear);
  draw->AddRectFilledMultiColor(ImVec2(0, display.y - band), display, clear, clear, dark, dark);
  float side = display.x * 0.18f;
  draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(side, display.y), dark, clear, clear, dark);
  draw->AddRectFilledMultiColor(ImVec2(display.x - side, 0), display, clear, dark, dark, clear);
}

void Rb3UiRenderer::DrawAccentBar(ImDrawList* draw, ImVec2 min, ImVec2 max, float scale) const {
  // Tinting the greyscale texture alone comes out olive.
  float rounding = std::min(5.0f * scale, (max.y - min.y) * 0.5f);
  draw->AddRectFilled(min, max, kUiAccent, rounding);
  DrawNineSlice(draw, "highlight_bar", min, max, 40.0f, std::min(9.0f * scale, rounding * 2),
                IM_COL32(255, 255, 255, 80));
}

void Rb3UiRenderer::DrawLogo(ImDrawList* draw, ImVec2 min, ImVec2 max) const {
  if (logo_) draw->AddImage(reinterpret_cast<ImTextureID>(logo_.get()), min, max);
}

bool Rb3UiRenderer::HasImage(const std::string& key) const { return images_.contains(key); }

void Rb3UiRenderer::DrawImage(ImDrawList* draw, const std::string& key, ImVec2 min, ImVec2 max,
                              ImU32 tint, ImVec2 uv0, ImVec2 uv1) const {
  auto it = images_.find(key);
  if (it == images_.end()) return;
  draw->AddImage(reinterpret_cast<ImTextureID>(it->second.texture.get()), min, max, uv0, uv1,
                 tint);
}

void Rb3UiRenderer::DrawNineSlice(ImDrawList* draw, const std::string& key, ImVec2 min,
                                  ImVec2 max, float border, float border_size,
                                  ImU32 tint) const {
  auto it = images_.find(key);
  if (it == images_.end()) return;
  const Texture& t = it->second;
  auto id = reinterpret_cast<ImTextureID>(t.texture.get());
  float bw = std::min(border_size, (max.x - min.x) * 0.5f);
  float bh = std::min(border_size, (max.y - min.y) * 0.5f);
  float xs[4] = {min.x, min.x + bw, max.x - bw, max.x};
  float ys[4] = {min.y, min.y + bh, max.y - bh, max.y};
  float us[4] = {0, border / t.width, 1 - border / t.width, 1};
  float vs[4] = {0, border / t.height, 1 - border / t.height, 1};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      if (xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row]) continue;
      draw->AddImage(id, ImVec2(xs[col], ys[row]), ImVec2(xs[col + 1], ys[row + 1]),
                     ImVec2(us[col], vs[row]), ImVec2(us[col + 1], vs[row + 1]), tint);
    }
  }
}

void Rb3UiRenderer::DrawTiled(ImDrawList* draw, const std::string& key, ImVec2 min, ImVec2 max,
                              float tile, ImU32 tint) const {
  auto it = images_.find(key);
  if (it == images_.end() || tile <= 0) return;
  draw->AddImage(reinterpret_cast<ImTextureID>(it->second.texture.get()), min, max,
                 ImVec2(0, 0), ImVec2((max.x - min.x) / tile, (max.y - min.y) / tile), tint);
}

}  // namespace rb3pc
