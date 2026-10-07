#include "rb3_assets.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

#include <zlib.h>

#include <rex/logging.h>

#include "ark.h"

namespace rb3pc {

namespace {

namespace fs = std::filesystem;

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
float BeF32(const uint8_t* p) {
  uint32_t bits = Be32(p);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

std::vector<uint8_t> InflateMilo(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> out;
  if (data.size() < 16) return out;
  uint32_t magic = Le32(&data[0]), offset = Le32(&data[4]), count = Le32(&data[8]);
  size_t pos = offset;
  for (uint32_t i = 0; i < count && 16 + i * 4 + 4 <= data.size(); ++i) {
    uint32_t size = Le32(&data[16 + i * 4]);
    bool stored = (size >> 24) == 1;
    size &= 0x00FFFFFF;
    if (pos + size > data.size()) return {};
    const uint8_t* block = &data[pos];
    pos += size;
    if (magic == 0xCABEDEAF || stored) {
      out.insert(out.end(), block, block + size);
      continue;
    }
    size_t skip = magic == 0xCDBEDEAF ? 4 : 0;  // u32 inflated-size prefix
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return {};
    z.next_in = const_cast<uint8_t*>(block + skip);
    z.avail_in = uInt(size - skip);
    uint8_t chunk[65536];
    int ret;
    do {
      z.next_out = chunk;
      z.avail_out = sizeof(chunk);
      ret = inflate(&z, Z_NO_FLUSH);
      out.insert(out.end(), chunk, chunk + (sizeof(chunk) - z.avail_out));
    } while (ret == Z_OK);
    inflateEnd(&z);
    if (ret != Z_STREAM_END) return {};
  }
  return out;
}

// Objects in an inflated milo, split on the 0xADDEADDE terminator.
std::vector<std::string_view> SplitObjects(const std::vector<uint8_t>& buf) {
  std::vector<std::string_view> out;
  std::string_view all(reinterpret_cast<const char*>(buf.data()), buf.size());
  static const std::string_view kMarker("\xAD\xDE\xAD\xDE", 4);
  size_t start = 0;
  for (size_t at; (at = all.find(kMarker, start)) != std::string_view::npos; start = at + 4) {
    out.push_back(all.substr(start, at - start));
  }
  out.push_back(all.substr(start));
  return out;
}

// Directory: u32 version, type, name, 2 x u32, u32 count, count x (type,
// name). Object i is chunk i + 1.
struct MiloObject {
  std::string type;
  std::string_view data;
};

std::unordered_map<std::string, MiloObject> MiloDirectory(const std::vector<uint8_t>& buf) {
  std::unordered_map<std::string, MiloObject> out;
  size_t pos = 4;
  auto read_string = [&](std::string& s) {
    if (pos + 4 > buf.size()) return false;
    uint32_t n = Be32(&buf[pos]);
    if (pos + 4 + n > buf.size()) return false;
    s.assign(reinterpret_cast<const char*>(&buf[pos + 4]), n);
    pos += 4 + n;
    return true;
  };
  std::string dir_type, dir_name;
  if (!read_string(dir_type) || !read_string(dir_name) || pos + 12 > buf.size()) return out;
  pos += 8;
  uint32_t count = Be32(&buf[pos]);
  pos += 4;
  std::vector<std::pair<std::string, std::string>> entries;
  for (uint32_t i = 0; i < count; ++i) {
    std::string type, name;
    if (!read_string(type) || !read_string(name)) return out;
    entries.emplace_back(std::move(type), std::move(name));
  }
  auto parts = SplitObjects(buf);
  for (size_t i = 0; i < entries.size() && i + 1 < parts.size(); ++i) {
    out[entries[i].second] = MiloObject{entries[i].first, parts[i + 1]};
  }
  return out;
}

struct EmbeddedBitmap {
  uint32_t encoding, width, height;
  const uint8_t* data;
};

// Embedded RndBitmap (rev 1): u8 rev, u8 bpp, u32 encoding, u8 mips,
// u16 width, u16 height, u16 row bytes, 19 bytes padding, pixels.
std::optional<EmbeddedBitmap> FindBitmap(std::string_view obj) {
  auto* p = reinterpret_cast<const uint8_t*>(obj.data());
  for (size_t i = 0; i + 32 < obj.size(); ++i) {
    if (p[i] != 1) continue;
    uint8_t bpp = p[i + 1];
    uint32_t enc = Be32(p + i + 2);
    uint16_t w = Be16(p + i + 7), h = Be16(p + i + 9), row = Be16(p + i + 11);
    if ((bpp != 4 && bpp != 8) || (enc != 8 && enc != 24) || p[i + 6] > 12) continue;
    if (!w || !h || (w & (w - 1)) || (h & (h - 1)) || uint32_t(row) * 8 != uint32_t(w) * bpp)
      continue;
    size_t start = i + 13 + 19;
    if (obj.size() - start < size_t(row) * h) continue;
    return EmbeddedBitmap{enc, w, h, p + start};
  }
  return std::nullopt;
}

void Rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// DXT1/DXT5, Xbox 360 byte order (every 16-bit word swapped).
Rgba8Image DecodeDxt(const EmbeddedBitmap& bmp) {
  Rgba8Image img{bmp.width, bmp.height, std::vector<uint8_t>(size_t(bmp.width) * bmp.height * 4)};
  bool dxt5 = bmp.encoding == 24;
  size_t block_size = dxt5 ? 16 : 8;
  const uint8_t* src = bmp.data;
  for (uint32_t by = 0; by < bmp.height; by += 4) {
    for (uint32_t bx = 0; bx < bmp.width; bx += 4, src += block_size) {
      uint8_t b[16];
      for (size_t k = 0; k < block_size; k += 2) {
        b[k] = src[k + 1];
        b[k + 1] = src[k];
      }
      uint8_t alpha[16];
      std::fill(std::begin(alpha), std::end(alpha), 255);
      const uint8_t* color = b;
      if (dxt5) {
        uint8_t a0 = b[0], a1 = b[1], pal[8] = {a0, a1};
        for (int k = 0; k < 6; ++k) {
          pal[2 + k] = a0 > a1 ? uint8_t(((6 - k) * a0 + (1 + k) * a1) / 7)
                               : (k < 4 ? uint8_t(((4 - k) * a0 + (1 + k) * a1) / 5)
                                        : uint8_t(k == 4 ? 0 : 255));
        }
        uint64_t bits = 0;
        for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
        for (int k = 0; k < 16; ++k) alpha[k] = pal[(bits >> (3 * k)) & 7];
        color = b + 8;
      }
      uint16_t c0 = uint16_t(color[0] | color[1] << 8), c1 = uint16_t(color[2] | color[3] << 8);
      uint32_t idx = Le32(color + 4);
      uint8_t pal[4][4];
      Rgb565(c0, pal[0]);
      Rgb565(c1, pal[1]);
      pal[0][3] = pal[1][3] = 255;
      for (int ch = 0; ch < 3; ++ch) {
        if (c0 > c1 || dxt5) {
          pal[2][ch] = uint8_t((2 * pal[0][ch] + pal[1][ch]) / 3);
          pal[3][ch] = uint8_t((pal[0][ch] + 2 * pal[1][ch]) / 3);
        } else {
          pal[2][ch] = uint8_t((pal[0][ch] + pal[1][ch]) / 2);
          pal[3][ch] = 0;
        }
      }
      pal[2][3] = 255;
      pal[3][3] = (c0 > c1 || dxt5) ? 255 : 0;
      for (int k = 0; k < 16; ++k) {
        uint32_t x = bx + k % 4, y = by + k / 4;
        uint8_t* dst = &img.pixels[(size_t(y) * bmp.width + x) * 4];
        const uint8_t* c = pal[(idx >> (2 * k)) & 3];
        dst[0] = c[0];
        dst[1] = c[1];
        dst[2] = c[2];
        dst[3] = dxt5 ? alpha[k] : c[3];
      }
    }
  }
  return img;
}

// RndFont rev 17 ends with: Vector2 tex cell size, u32 count, count x
// (u16 char, f32 u, f32 v, f32 width, f32 advance), next-font name.
bool ParseFontGlyphs(std::string_view obj, BitmapFont& font) {
  auto* p = reinterpret_cast<const uint8_t*>(obj.data());
  size_t end = obj.size();
  for (size_t name_len = 0; name_len < 128 && end >= 16 + name_len; ++name_len) {
    size_t tail = end - 4 - name_len;
    if (Be32(p + tail) != name_len) continue;
    for (size_t count = 1; count < 2048 && tail >= 12 + count * 18; ++count) {
      size_t start = tail - count * 18;
      if (Be32(p + start - 4) != count) continue;
      font.cell_u = BeF32(p + start - 12);
      font.cell_v = BeF32(p + start - 8);
      for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = p + start + i * 18;
        font.glyphs[Be16(e)] = {BeF32(e + 2), BeF32(e + 6), BeF32(e + 10), BeF32(e + 14)};
      }
      return true;
    }
  }
  return false;
}

}  // namespace

Rb3UiAssets LoadRb3UiAssets(const fs::path& game_root, const std::vector<UiImageRequest>& images,
                            const std::vector<UiFontRequest>& fonts) {
  Rb3UiAssets assets;
  Ark ark;
  if (!ark.Open(game_root)) {
    REXLOG_WARN("UI assets: couldn't read {}/gen/main_xbox.hdr", game_root.string());
    return assets;
  }

  std::unordered_map<std::string, std::vector<uint8_t>> milos;
  std::unordered_map<std::string, std::unordered_map<std::string, MiloObject>> dirs;
  auto directory = [&](const std::string& path) -> const auto& {
    auto it = dirs.find(path);
    if (it != dirs.end()) return it->second;
    auto& milo = milos[path] = InflateMilo(ark.Read(path));
    return dirs[path] = MiloDirectory(milo);
  };
  auto texture = [&](const std::string& milo, const std::string& tex) -> std::optional<Rgba8Image> {
    const auto& dir = directory(milo);
    auto it = dir.find(tex);
    if (it == dir.end() || it->second.type != "Tex") return std::nullopt;
    auto bmp = FindBitmap(it->second.data);
    if (!bmp) return std::nullopt;
    return DecodeDxt(*bmp);
  };

  size_t missing = 0;
  for (const auto& request : images) {
    if (auto image = texture(request.milo, request.tex)) {
      assets.images[request.key] = std::move(*image);
    } else {
      REXLOG_WARN("UI assets: no texture {} in {}", request.tex, request.milo);
      ++missing;
    }
  }
  for (const auto& request : fonts) {
    BitmapFont font;
    const auto& dir = directory(request.milo);
    auto it = dir.find(request.font);
    auto atlas = texture(request.milo, request.tex);
    if (atlas && it != dir.end() && ParseFontGlyphs(it->second.data, font)) {
      font.atlas = std::move(*atlas);
      assets.fonts[request.key] = std::move(font);
    } else {
      REXLOG_WARN("UI assets: no font {} in {}", request.font, request.milo);
      ++missing;
    }
  }

  // In the rooftop "shell" vignette (from ui/image/rb3_logo.bmp).
  {
    auto milo = InflateMilo(ark.Read("world/vignette/shell/gen/sv8_a.milo_xbox"));
    for (auto obj : SplitObjects(milo)) {
      if (obj.substr(0, 300).find("rb3_logo.bmp") == std::string_view::npos) continue;
      if (auto bmp = FindBitmap(obj)) {
        assets.logo = DecodeDxt(*bmp);
        break;
      }
    }
  }

  REXLOG_INFO("UI assets: logo {}, {} images, {} fonts, {} missing",
              assets.logo ? "loaded" : "missing", assets.images.size(), assets.fonts.size(),
              missing);
  return assets;
}

}  // namespace rb3pc
