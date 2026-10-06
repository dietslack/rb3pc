#include "shader_warmup.h"

#include <cstring>
#include <set>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/shader_warmup.h>

#include "ark.h"

REXCVAR_DEFINE_BOOL(precompile_shaders, true, "rb3pc",
                    "Compile all of the game's shaders before it starts (once; a few minutes "
                    "on first launch) so new shaders don't stall the game mid-song");

namespace rb3pc {

namespace {

// Shader cache format (loader sub_82711818), little-endian except containers:
//   "XOBX", u32 1, u32 types; per type: u32 len + name, u32 variants,
//   pixel then vertex: u32 virt size, u32 phys size, virt pool, phys pool;
//   per variant: u64 key, (u32 virt, u32 phys) for pixel then vertex.
// Virt pool entry: D3D object (0x28 pixel / 0x368 vertex) + BE container
// (magic 0x102A1100/1, ..., +0x18 -> header {ucode offset, size}).
class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
  bool ok() const { return ok_; }
  uint32_t U32() {
    uint32_t v = 0;
    if (Need(4)) std::memcpy(&v, &data_[pos_], 4);
    pos_ += 4;
    return v;
  }
  uint64_t U64() {
    uint64_t lo = U32();
    return lo | uint64_t(U32()) << 32;
  }
  std::pair<size_t, size_t> Span(size_t size) {
    size_t start = pos_;
    if (Need(size)) pos_ += size;
    return {start, size};
  }
  bool AtEnd() const { return ok_ && pos_ == data_.size(); }

 private:
  bool Need(size_t n) {
    if (pos_ + n > data_.size()) ok_ = false;
    return ok_;
  }
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

uint32_t Be32(const std::vector<uint8_t>& data, size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return uint32_t(data[offset]) << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 |
         data[offset + 3];
}

constexpr uint32_t kObjectSize[2] = {0x28, 0x368};  // pixel, vertex
constexpr uint32_t kMagic[2] = {0x102A1100, 0x102A1101};

// Guest-endian dwords.
std::vector<uint32_t> Ucode(const std::vector<uint8_t>& data, std::pair<size_t, size_t> virt,
                            std::pair<size_t, size_t> phys, uint32_t virt_offset,
                            uint32_t phys_offset, int kind) {
  size_t container = virt.first + virt_offset + kObjectSize[kind];
  if (virt_offset + kObjectSize[kind] + 0x1C > virt.second ||
      Be32(data, container) != kMagic[kind]) {
    return {};
  }
  size_t header = container + Be32(data, container + 0x18);
  uint32_t ucode_offset = Be32(data, header), ucode_size = Be32(data, header + 4);
  uint64_t start = uint64_t(phys_offset) + ucode_offset;
  if (header + 8 > virt.first + virt.second || start + ucode_size > phys.second ||
      ucode_size % 4 || !ucode_size) {
    return {};
  }
  std::vector<uint32_t> ucode(ucode_size / 4);
  std::memcpy(ucode.data(), &data[phys.first + start], ucode_size);
  return ucode;
}

bool ParseShaderCache(const std::vector<uint8_t>& data,
                      std::set<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>>& pairs) {
  Reader r(data);
  if (r.U32() != 0x58424F58 || r.U32() != 1) return false;  // "XOBX"
  uint32_t types = r.U32();
  for (uint32_t t = 0; t < types && r.ok(); ++t) {
    r.Span(r.U32());  // name
    uint32_t variants = r.U32();
    std::pair<size_t, size_t> pools[2][2];
    for (auto& pool : pools) {
      uint32_t virt_size = r.U32();
      uint32_t phys_size = r.U32();
      pool[0] = r.Span(virt_size);
      pool[1] = r.Span(phys_size);
    }
    for (uint32_t v = 0; v < variants && r.ok(); ++v) {
      r.U64();  // key
      std::vector<uint32_t> ucode[2];
      for (int kind = 0; kind < 2; ++kind) {
        uint32_t virt_offset = r.U32();
        uint32_t phys_offset = r.U32();
        if (r.ok()) {
          ucode[kind] = Ucode(data, pools[kind][0], pools[kind][1], virt_offset, phys_offset, kind);
        }
      }
      if (!ucode[0].empty() && !ucode[1].empty()) {
        pairs.emplace(std::move(ucode[1]), std::move(ucode[0]));
      }
    }
  }
  return r.AtEnd();
}

}  // namespace

void RegisterShaderWarmup(const std::filesystem::path& game_root) {
  if (!REXCVAR_GET(precompile_shaders)) {
    rex::system::SetShaderWarmupSource(nullptr);
    return;
  }
  rex::system::SetShaderWarmupSource([game_root] {
    std::vector<rex::system::ShaderWarmupPair> result;
    Ark ark;
    if (!ark.Open(game_root)) {
      REXLOG_WARN("Shader warm-up: couldn't open the ARK in {}", game_root.string());
      return result;
    }
    // (vertex, pixel) ucode; a set, since many variants share both.
    std::set<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> pairs;
    for (const char* name : {"xbox_preinit_shaders", "xbox_shaders"}) {
      std::vector<uint8_t> data;
      for (const auto& entry : ark.header().entries) {
        std::string path = ark.header().Path(entry);
        if (path.size() >= std::strlen(name) &&
            path.compare(path.size() - std::strlen(name), std::string::npos, name) == 0 &&
            (entry.ucsize == 0 || entry.ucsize == entry.size)) {  // 0: stored uncompressed
          data = ark.Read(path);
          break;
        }
      }
      if (data.empty() || !ParseShaderCache(data, pairs)) {
        REXLOG_WARN("Shader warm-up: couldn't read {} from the ARK", name);
      }
    }
    for (auto& [vertex, pixel] : pairs) {
      result.push_back({vertex, pixel});
    }
    REXLOG_INFO("Shader warm-up: {} shader pairs in the game's shader caches", result.size());
    return result;
  });
}

}  // namespace rb3pc
