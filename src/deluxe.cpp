#include "deluxe.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>
#include <vector>

#include <zlib.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "exe.h"

#ifdef RB3PC_DX
#include "generated/dx_reference.h"
#include <rex/system/kernel_state.h>
#endif

REXCVAR_DEFINE_BOOL(deluxe, false, "rb3pc", "Load Rock Band 3 Deluxe (when installed)");


namespace rb3pc {

namespace {

namespace fs = std::filesystem;

constexpr const char* kHeaderName = "patch_xbox.hdr";
constexpr const char* kXexName = "default.xex";
constexpr const char* kNameFile = "installed_from.txt";
constexpr const char* kHandoffEnv = "RB3PC_HANDOFF";

fs::path OtherExecutable() {
#ifdef _WIN32
  const char* name = exe::kDeluxe ? "rb3pc.exe" : "rb3pc-dx.exe";
#else
  const char* name = exe::kDeluxe ? "rb3pc" : "rb3pc-dx";
#endif
  return rex::filesystem::GetExecutableFolder() / name;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool IsDeluxeFile(const std::string& lower_name) {
  return lower_name.starts_with("patch_xbox") &&
         (lower_name.ends_with(".hdr") || lower_name.ends_with(".ark"));
}

// A minimal zip reader: stored and deflated entries, zip64 sizes.
class ZipReader {
 public:
  struct Entry {
    std::string name;
    uint16_t method = 0;
    uint64_t compressed = 0, size = 0, local_offset = 0;
    uint32_t crc = 0;
  };

  bool Open(const fs::path& path) {
    in_.open(path, std::ios::binary);
    if (!in_) return false;
    in_.seekg(0, std::ios::end);
    uint64_t file_size = uint64_t(in_.tellg());
    // End of central directory: in the last 64 KiB + 22 bytes.
    uint64_t tail = std::min<uint64_t>(file_size, 65557);
    std::vector<uint8_t> buf(tail);
    if (!ReadAt(file_size - tail, buf.data(), tail)) return false;
    int64_t eocd = -1;
    for (int64_t i = int64_t(tail) - 22; i >= 0; --i) {
      if (U32(&buf[i]) == 0x06054b50) {
        eocd = i;
        break;
      }
    }
    if (eocd < 0) return false;
    uint64_t count = U16(&buf[eocd + 10]);
    uint64_t cd_size = U32(&buf[eocd + 12]);
    uint64_t cd_offset = U32(&buf[eocd + 16]);
    // Zip64: the locator sits just before the end record.
    if (eocd >= 20 && U32(&buf[eocd - 20]) == 0x07064b50) {
      uint8_t rec[56];
      if (!ReadAt(U64(&buf[eocd - 20 + 8]), rec, sizeof(rec)) || U32(rec) != 0x06064b50) {
        return false;
      }
      count = U64(rec + 32);
      cd_size = U64(rec + 40);
      cd_offset = U64(rec + 48);
    }
    std::vector<uint8_t> cd(cd_size);
    if (!ReadAt(cd_offset, cd.data(), cd_size)) return false;
    size_t p = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) return false;
      Entry e;
      e.method = U16(&cd[p + 10]);
      e.crc = U32(&cd[p + 16]);
      e.compressed = U32(&cd[p + 20]);
      e.size = U32(&cd[p + 24]);
      uint16_t name_len = U16(&cd[p + 28]), extra_len = U16(&cd[p + 30]),
               comment_len = U16(&cd[p + 32]);
      e.local_offset = U32(&cd[p + 42]);
      if (p + 46 + name_len + extra_len > cd.size()) return false;
      e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), name_len);
      std::replace(e.name.begin(), e.name.end(), '\\', '/');
      // Zip64 extra field: the 64-bit values, for each field saturated above.
      const uint8_t* x = &cd[p + 46 + name_len];
      for (size_t q = 0; q + 4 <= extra_len;) {
        uint16_t id = U16(x + q), len = U16(x + q + 2);
        if (id == 0x0001) {
          size_t r = q + 4;
          if (e.size == 0xFFFFFFFF && r + 8 <= q + 4 + len) e.size = U64(x + r), r += 8;
          if (e.compressed == 0xFFFFFFFF && r + 8 <= q + 4 + len) e.compressed = U64(x + r), r += 8;
          if (e.local_offset == 0xFFFFFFFF && r + 8 <= q + 4 + len) e.local_offset = U64(x + r);
        }
        q += 4 + len;
      }
      entries_.push_back(std::move(e));
      p += 46 + name_len + extra_len + comment_len;
    }
    return true;
  }

  const std::vector<Entry>& entries() const { return entries_; }

  // Checks CRC; `on_bytes` gets each chunk's uncompressed size.
  template <typename F>
  bool Extract(const Entry& e, const fs::path& out_path, F&& on_bytes) {
    uint8_t local[30];
    if (!ReadAt(e.local_offset, local, sizeof(local)) || U32(local) != 0x04034b50) return false;
    uint64_t data = e.local_offset + 30 + U16(local + 26) + U16(local + 28);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    in_.clear();
    in_.seekg(std::streamoff(data));
    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> src(kChunk), dst(kChunk);
    uint32_t crc = crc32(0, nullptr, 0);
    uint64_t remaining = e.compressed;
    if (e.method == 0) {
      while (remaining) {
        size_t n = size_t(std::min<uint64_t>(remaining, kChunk));
        if (!in_.read(reinterpret_cast<char*>(src.data()), std::streamsize(n))) return false;
        crc = crc32(crc, src.data(), uInt(n));
        out.write(reinterpret_cast<const char*>(src.data()), std::streamsize(n));
        on_bytes(n);
        remaining -= n;
      }
    } else if (e.method == 8) {
      z_stream z{};
      if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
      int ret = Z_OK;
      while (ret != Z_STREAM_END) {
        if (z.avail_in == 0) {
          size_t n = size_t(std::min<uint64_t>(remaining, kChunk));
          if (n == 0 || !in_.read(reinterpret_cast<char*>(src.data()), std::streamsize(n))) {
            inflateEnd(&z);
            return false;
          }
          remaining -= n;
          z.next_in = src.data();
          z.avail_in = uInt(n);
        }
        z.next_out = dst.data();
        z.avail_out = uInt(kChunk);
        ret = inflate(&z, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
          inflateEnd(&z);
          return false;
        }
        size_t n = kChunk - z.avail_out;
        crc = crc32(crc, dst.data(), uInt(n));
        out.write(reinterpret_cast<const char*>(dst.data()), std::streamsize(n));
        on_bytes(n);
      }
      inflateEnd(&z);
    } else {
      return false;
    }
    return bool(out) && crc == e.crc;
  }

 private:
  static uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
  static uint32_t U32(const uint8_t* p) { return U16(p) | uint32_t(U16(p + 2)) << 16; }
  static uint64_t U64(const uint8_t* p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }

  bool ReadAt(uint64_t offset, uint8_t* out, uint64_t size) {
    in_.clear();
    in_.seekg(std::streamoff(offset));
    return bool(in_.read(reinterpret_cast<char*>(out), std::streamsize(size)));
  }

  std::ifstream in_;
  std::vector<Entry> entries_;
};

std::optional<std::string> InstallFromZip(const fs::path& zip_path, const fs::path& gen,
                                          std::atomic<float>* progress) {
  ZipReader zip;
  if (!zip.Open(zip_path)) return "Not a zip file, or it's damaged.";
  // Releases may nest everything in a top-level folder.
  std::string gen_prefix;
  for (const auto& e : zip.entries()) {
    std::string lower = Lower(e.name);
    if (lower == "gen/" + std::string(kHeaderName) ||
        lower.ends_with("/gen/" + std::string(kHeaderName))) {
      gen_prefix = lower.substr(0, lower.size() - std::strlen(kHeaderName));
      break;
    }
  }
  if (gen_prefix.empty()) {
    return "This zip doesn't contain gen/patch_xbox.hdr. Pick the Xbox 360 release of Rock "
           "Band 3 Deluxe.";
  }
  std::string root_prefix = gen_prefix.substr(0, gen_prefix.size() - 4);
  const ZipReader::Entry* xex = nullptr;
  for (const auto& e : zip.entries()) {
    if (Lower(e.name) == root_prefix + kXexName) xex = &e;
  }
  if (!xex) return "This zip doesn't contain default.xex next to its gen/ folder.";
  std::vector<const ZipReader::Entry*> files;
  uint64_t total = xex->size, done = 0;
  auto count = [&](size_t n) {
    done += n;
    if (progress && total) progress->store(float(double(done) / double(total)));
  };
  if (!zip.Extract(*xex, gen.parent_path() / kXexName, count)) {
    return "Couldn't extract default.xex (damaged zip, or the disk is full).";
  }
  for (const auto& e : zip.entries()) {
    std::string lower = Lower(e.name);
    if (!lower.starts_with(gen_prefix)) continue;
    std::string leaf = lower.substr(gen_prefix.size());
    if (leaf.find('/') != std::string::npos || !IsDeluxeFile(leaf)) continue;
    files.push_back(&e);
    total += e.size;
  }
  for (const auto* e : files) {
    std::string leaf = Lower(e->name.substr(gen_prefix.size()));
    bool ok = zip.Extract(*e, gen / leaf, count);
    if (!ok) return "Couldn't extract " + e->name + " (damaged zip, or the disk is full).";
  }
  return std::nullopt;
}

std::optional<std::string> InstallFromFolder(const fs::path& folder, const fs::path& gen,
                                             std::atomic<float>* progress) {
  // The folder itself, its gen/, or one level further down.
  std::error_code ec;
  fs::path src;
  auto has_header = [&](const fs::path& dir) { return fs::exists(dir / kHeaderName, ec); };
  if (has_header(folder / "gen")) {
    src = folder / "gen";
  } else if (has_header(folder)) {
    src = folder;
  } else {
    for (const auto& sub : fs::directory_iterator(folder, ec)) {
      if (sub.is_directory(ec) && has_header(sub.path() / "gen")) {
        src = sub.path() / "gen";
        break;
      }
    }
  }
  if (src.empty()) return "This folder doesn't contain gen/patch_xbox.hdr.";
  if (!fs::copy_file(src.parent_path() / kXexName, gen.parent_path() / kXexName,
                     fs::copy_options::overwrite_existing, ec)) {
    return "Couldn't copy default.xex from next to gen/: " + ec.message();
  }
  std::vector<fs::path> files;
  uint64_t total = 0, done = 0;
  for (const auto& entry : fs::directory_iterator(src, ec)) {
    if (entry.is_regular_file(ec) && IsDeluxeFile(Lower(entry.path().filename().string()))) {
      files.push_back(entry.path());
      total += entry.file_size(ec);
    }
  }
  for (const auto& file : files) {
    if (!fs::copy_file(file, gen / Lower(file.filename().string()),
                       fs::copy_options::overwrite_existing, ec)) {
      return "Couldn't copy " + file.filename().string() + ": " + ec.message();
    }
    done += fs::file_size(file, ec);
    if (progress && total) progress->store(float(double(done) / double(total)));
  }
  return std::nullopt;
}

}  // namespace

fs::path DeluxeInstallDir() { return rex::filesystem::GetUserFolder() / "rb3pc" / "deluxe"; }

}  // namespace rb3pc
