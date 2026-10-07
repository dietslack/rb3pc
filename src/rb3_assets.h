// UI assets (logo, Pentatonic font, textures) read straight from the ARK,
// before the game runs. Formats match tools/milo/milo.py. A .milo_xbox is a
// block-compressed (raw deflate) object directory. Tex objects embed an
// RndBitmap, DXT1/DXT5 with the 16-bit words byte-swapped. Font (rev 17) has a
// glyph table of (char, u, v, width, advance).

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rb3pc {

struct Rgba8Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;  // RGBA8, row-major
};

struct BitmapFont {
  struct Glyph {
    float u, v;     // top-left in the atlas, 0..1
    float width;    // in line heights
    float advance;  // in line heights
  };
  Rgba8Image atlas;
  float cell_u = 0, cell_v = 0;  // one cell (line) in atlas units
  std::unordered_map<uint16_t, Glyph> glyphs;
};

struct UiImageRequest {
  std::string key, milo, tex;
};
struct UiFontRequest {
  std::string key, milo, font, tex;
};

struct Rb3UiAssets {
  std::optional<Rgba8Image> logo;  // ui/image/rb3_logo, 1024x256
  std::unordered_map<std::string, Rgba8Image> images;
  std::unordered_map<std::string, BitmapFont> fonts;
};

// Best effort; DXT1/DXT5 textures only.
Rb3UiAssets LoadRb3UiAssets(const std::filesystem::path& game_root,
                            const std::vector<UiImageRequest>& images,
                            const std::vector<UiFontRequest>& fonts);

}  // namespace rb3pc
