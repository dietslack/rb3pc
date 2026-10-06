#include "ark.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace rb3pc {

namespace {

namespace fs = std::filesystem;

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// The cipher's keystream: one byte per step of a Park-Miller style LCG.
void Crypt(uint8_t* data, size_t size, int32_t key) {
  for (size_t i = 0; i < size; ++i) {
    int32_t r = (key % 0x1F31D) * 0x41A7 - (key / 0x1F31D) * 0xB14;
    if (r <= 0) r += 0x7FFFFFFF;
    key = r;
    data[i] ^= uint8_t(key);
  }
}

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}
  uint32_t U32() {
    Need(4);
    uint32_t v = Le32(&data_[pos_]);
    pos_ += 4;
    return v;
  }
  uint64_t U64() {
    uint64_t lo = U32();
    return lo | uint64_t(U32()) << 32;
  }
  void Bytes(uint8_t* out, size_t n) {
    Need(n);
    std::memcpy(out, &data_[pos_], n);
    pos_ += n;
  }
  std::string String() {
    uint32_t n = U32();
    Need(n);
    std::string s(reinterpret_cast<const char*>(&data_[pos_]), n);
    pos_ += n;
    return s;
  }

 private:
  void Need(size_t n) {
    if (pos_ + n > data_.size()) throw std::out_of_range("ark header");
  }
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
};

class Writer {
 public:
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v));
    U32(uint32_t(v >> 32));
  }
  void Bytes(const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }
  void String(const std::string& s) {
    U32(uint32_t(s.size()));
    Bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  std::vector<uint8_t> out;
};

}  // namespace

std::vector<uint8_t> HarmonixDecrypt(const std::vector<uint8_t>& data) {
  if (data.size() < 4) return {};
  std::vector<uint8_t> out(data.begin() + 4, data.end());
  Crypt(out.data(), out.size(), int32_t(Le32(data.data())));
  return out;
}

std::vector<uint8_t> HarmonixEncrypt(const std::vector<uint8_t>& plain, uint32_t key) {
  std::vector<uint8_t> out(4 + plain.size());
  for (int i = 0; i < 4; ++i) out[i] = uint8_t(key >> (8 * i));
  std::memcpy(out.data() + 4, plain.data(), plain.size());
  Crypt(out.data() + 4, plain.size(), int32_t(key));
  return out;
}

std::optional<ArkHeader> ArkHeader::Parse(const std::vector<uint8_t>& plain) {
  ArkHeader h;
  try {
    Reader r(plain);
    h.version = r.U32();
    if (h.version != 6) return std::nullopt;
    r.Bytes(h.guid, sizeof(h.guid));
    h.part_count = r.U32();
    h.part_sizes.resize(r.U32());
    for (auto& v : h.part_sizes) v = r.U32();
    h.part_paths.resize(r.U32());
    for (auto& v : h.part_paths) v = r.String();
    h.cache_priorities.resize(r.U32());
    for (auto& v : h.cache_priorities) v = int32_t(r.U32());
    h.string_blob.resize(r.U32());
    r.Bytes(h.string_blob.data(), h.string_blob.size());
    h.string_offsets.resize(r.U32());
    for (auto& v : h.string_offsets) v = r.U32();
    h.entries.resize(r.U32());
    for (auto& e : h.entries) {
      e.offset = r.U64();
      e.name = r.U32();
      e.dir = r.U32();
      e.size = r.U32();
      e.ucsize = r.U32();
    }
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
  return h;
}

std::vector<uint8_t> ArkHeader::Serialize() const {
  Writer w;
  w.U32(version);
  w.Bytes(guid, sizeof(guid));
  w.U32(part_count);
  w.U32(uint32_t(part_sizes.size()));
  for (auto v : part_sizes) w.U32(v);
  w.U32(uint32_t(part_paths.size()));
  for (const auto& v : part_paths) w.String(v);
  w.U32(uint32_t(cache_priorities.size()));
  for (auto v : cache_priorities) w.U32(uint32_t(v));
  w.U32(uint32_t(string_blob.size()));
  w.Bytes(string_blob.data(), string_blob.size());
  w.U32(uint32_t(string_offsets.size()));
  for (auto v : string_offsets) w.U32(v);
  w.U32(uint32_t(entries.size()));
  for (const auto& e : entries) {
    w.U64(e.offset);
    w.U32(e.name);
    w.U32(e.dir);
    w.U32(e.size);
    w.U32(e.ucsize);
  }
  return std::move(w.out);
}

std::string ArkHeader::Path(const Entry& entry) const {
  auto str = [&](uint32_t index) -> std::string {
    if (index >= string_offsets.size() || string_offsets[index] >= string_blob.size()) return {};
    const char* s = reinterpret_cast<const char*>(&string_blob[string_offsets[index]]);
    return std::string(s, strnlen(s, string_blob.size() - string_offsets[index]));
  };
  return str(entry.dir) + "/" + str(entry.name);
}

std::optional<size_t> ArkHeader::Find(const std::string& path) const {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (Path(entries[i]) == path) return i;
  }
  return std::nullopt;
}

bool Ark::Open(const fs::path& game_root) {
  root_ = game_root;
  auto raw = ReadFile(game_root / "gen" / "main_xbox.hdr");
  if (raw.size() < 8) return false;
  header_key_ = Le32(raw.data());
  auto header = ArkHeader::Parse(HarmonixDecrypt(raw));
  if (!header || header->entries.empty()) return false;
  header_ = std::move(*header);
  for (size_t i = 0; i < header_.entries.size(); ++i) {
    index_.emplace(header_.Path(header_.entries[i]), i);
  }
  return true;
}

std::vector<uint8_t> Ark::Read(const std::string& path) const {
  auto it = index_.find(path);
  if (it == index_.end()) return {};
  const auto& entry = header_.entries[it->second];
  uint64_t offset = entry.offset;
  for (size_t i = 0; i < header_.part_sizes.size() && i < header_.part_paths.size(); ++i) {
    if (offset < header_.part_sizes[i]) {
      std::ifstream in(root_ / header_.part_paths[i], std::ios::binary);
      in.seekg(std::streamoff(offset));
      std::vector<uint8_t> data(entry.size);
      in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
      return in ? data : std::vector<uint8_t>{};
    }
    offset -= header_.part_sizes[i];
  }
  return {};
}

}  // namespace rb3pc
