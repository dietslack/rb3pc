// ARK archives (gen/main_xbox.hdr + _N.ark): reading, and rewriting the index
// to serve files from an extra part (script_patches.cpp).
// Header (v6, decrypted, little-endian):
//   u32 version, 20 bytes (GUID), u32 part count,
//   u32 n + n x u32 part sizes, u32 n + n x string part paths,
//   u32 n + n x i32 cache priorities,
//   u32 size + string blob, u32 n + n x u32 string offsets (the hash table),
//   u32 n + n x entry { u64 offset, u32 name, u32 dir, u32 size, u32 ucsize }
// Strings are u32 length + chars. Entry offsets run across all parts, in
// order; name and dir index the string offsets.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rb3pc {

// Harmonix's header/DTB cipher: an LCG seeded by the first 4 bytes. Decrypt
// drops the key; Encrypt prepends it.
std::vector<uint8_t> HarmonixDecrypt(const std::vector<uint8_t>& data);
std::vector<uint8_t> HarmonixEncrypt(const std::vector<uint8_t>& plain, uint32_t key);

class ArkHeader {
 public:
  struct Entry {
    uint64_t offset;
    uint32_t name, dir, size, ucsize;
  };

  static std::optional<ArkHeader> Parse(const std::vector<uint8_t>& plain);
  std::vector<uint8_t> Serialize() const;

  // "ui/gen/ui.dtb".
  std::optional<size_t> Find(const std::string& path) const;
  std::string Path(const Entry& entry) const;

  uint32_t version = 0;
  uint8_t guid[20] = {};
  uint32_t part_count = 0;
  std::vector<uint32_t> part_sizes;
  std::vector<std::string> part_paths;  // relative to the game root
  std::vector<int32_t> cache_priorities;
  std::vector<uint8_t> string_blob;
  std::vector<uint32_t> string_offsets;
  std::vector<Entry> entries;
};

class Ark {
 public:
  // header_name: e.g. "patch_xbox.hdr" for a patch ARK under the same root.
  bool Open(const std::filesystem::path& game_root, const std::string& header_name = "main_xbox.hdr");
  // As stored (DTBs encrypted); empty if missing.
  std::vector<uint8_t> Read(const std::string& path) const;

  const ArkHeader& header() const { return header_; }
  uint32_t header_key() const { return header_key_; }

 private:
  std::filesystem::path root_;
  ArkHeader header_;
  std::unordered_map<std::string, size_t> index_;
  uint32_t header_key_ = 0;
};

}  // namespace rb3pc
