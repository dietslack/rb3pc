// RB3-styled drawing for the app's own screens (launcher, shader loading),
// using fonts and UI art read from the ARK in the background.

#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "rb3_assets.h"

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace rb3pc {

constexpr ImU32 kUiBackground = IM_COL32(6, 6, 8, 255);
constexpr ImU32 kUiText = IM_COL32(228, 228, 232, 255);
constexpr ImU32 kUiAccent = IM_COL32(214, 216, 54, 255);
constexpr ImU32 kUiMuted = IM_COL32(150, 150, 158, 255);
constexpr ImU32 kUiTrack = IM_COL32(40, 40, 46, 255);
constexpr ImU32 kUiDark = IM_COL32(12, 12, 14, 255);

// Fallback font. Call from ReXApp::OnConfigureFonts.
void AddRb3UiFonts(ImFontAtlas* atlas);

enum class UiFont {
  kDisplay,  // Pentatonic Regular, large atlas (title-screen style)
  kHub,      // Pentatonic Hub, the main menu's ("PLAY NOW", "CAREER")
  kButtons,  // controller icons: A B X Y, g r y b o fret bars, s / S arrows
};

class Rb3UiRenderer {
 public:
  Rb3UiRenderer(std::function<rex::ui::ImmediateDrawer*()> immediate_drawer,
                std::filesystem::path game_root);
  ~Rb3UiRenderer();

  // UI thread, every frame before drawing.
  void Update();

  float TextWidth(UiFont font, const std::string& text, float size) const;
  void DrawText(ImDrawList* draw, UiFont font, const std::string& text, float size, float x,
                float y, ImU32 color) const;
  void DrawTextCentered(ImDrawList* draw, UiFont font, const std::string& text, float size,
                        float center_x, float y, ImU32 color) const;
  void DrawTextShadowed(ImDrawList* draw, UiFont font, const std::string& text, float size,
                        float x, float y, ImU32 color) const;

  // Swirl wallpaper, teal wash, rays from `logo_center`, vignette.
  // `scale` = display height / 720.
  void DrawBackdrop(ImDrawList* draw, ImVec2 display, float scale, ImVec2 logo_center) const;
  void DrawAccentBar(ImDrawList* draw, ImVec2 min, ImVec2 max, float scale) const;

  // Valid before the logo has loaded.
  float logo_aspect() const { return logo_aspect_; }
  void DrawLogo(ImDrawList* draw, ImVec2 min, ImVec2 max) const;

  // Keys from kImages in rb3_ui.cpp. Missing textures draw nothing.
  bool HasImage(const std::string& key) const;
  void DrawImage(ImDrawList* draw, const std::string& key, ImVec2 min, ImVec2 max,
                 ImU32 tint = IM_COL32_WHITE, ImVec2 uv0 = ImVec2(0, 0),
                 ImVec2 uv1 = ImVec2(1, 1)) const;
  // `border` texels per edge drawn at `border_size` screen pixels.
  void DrawNineSlice(ImDrawList* draw, const std::string& key, ImVec2 min, ImVec2 max,
                     float border, float border_size, ImU32 tint = IM_COL32_WHITE) const;
  void DrawTiled(ImDrawList* draw, const std::string& key, ImVec2 min, ImVec2 max, float tile,
                 ImU32 tint = IM_COL32_WHITE) const;

 private:
  struct Texture {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    float width = 0, height = 0;
  };
  struct Font {
    BitmapFont glyphs;
    std::unique_ptr<rex::ui::ImmediateTexture> atlas;
  };

  const Font* FindFont(UiFont font) const;

  std::function<rex::ui::ImmediateDrawer*()> immediate_drawer_;
  std::future<Rb3UiAssets> assets_future_;
  bool uploaded_ = false;
  std::unique_ptr<rex::ui::ImmediateTexture> logo_;
  float logo_aspect_ = 4.0f;
  std::unordered_map<std::string, Texture> images_;
  std::unordered_map<int, Font> fonts_;
};

}  // namespace rb3pc
