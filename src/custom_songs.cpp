#include "custom_songs.h"

#include <cstring>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

REXCVAR_DEFINE_STRING(songs_folders, "", "rb3pc",
                      "More folders of custom songs and DLC as Xbox 360 CON / LIVE files, "
                      "separated by ';' (subfolders included). The songs folder in rb3pc's data "
                      "folder is always read. Takes effect on restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rb3pc {

std::filesystem::path DefaultSongsFolder() {
  return rex::filesystem::GetUserFolder() / "rb3pc" / "songs";
}

std::vector<std::filesystem::path> ExtraSongsFolders() {
  std::vector<std::filesystem::path> folders;
  std::string_view list = REXCVAR_GET(songs_folders);
  while (!list.empty()) {
    size_t end = std::min(list.find(';'), list.size());
    if (end) folders.push_back(rex::to_path(std::string(list.substr(0, end))));
    list.remove_prefix(std::min(end + 1, list.size()));
  }
  return folders;
}

void SetExtraSongsFolders(const std::vector<std::filesystem::path>& folders) {
  std::string list;
  for (const auto& folder : folders) {
    if (!list.empty()) list += ';';
    list += rex::path_to_utf8(folder);
  }
  rex::cvar::SetFlagByName("songs_folders", list);
}

std::vector<std::filesystem::path> SongsFolders() {
  std::vector<std::filesystem::path> folders = ExtraSongsFolders();
  folders.insert(folders.begin(), DefaultSongsFolder());
  std::vector<std::filesystem::path> resolved;
  for (const auto& folder : folders) {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::weakly_canonical(folder, ec);
    resolved.push_back(ec ? folder.lexically_normal() : path);
  }
  auto within = [](const std::filesystem::path& a, const std::filesystem::path& b) {
    std::filesystem::path rel = a.lexically_relative(b);
    return !rel.empty() && *rel.begin() != "..";
  };
  std::vector<std::filesystem::path> result;
  for (size_t i = 0; i < folders.size(); ++i) {
    bool covered = false;
    for (size_t j = 0; j < folders.size() && !covered; ++j) {
      if (j == i || !within(resolved[i], resolved[j])) continue;
      // The same folder twice: the first one stays.
      covered = !within(resolved[j], resolved[i]) || j < i;
    }
    if (!covered) result.push_back(folders[i]);
  }
  return result;
}

size_t CountSongPackages(const std::vector<std::filesystem::path>& folders) {
  size_t count = 0;
  for (const auto& folder : folders) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::ifstream file(it->path(), std::ios::binary);
      char magic[4] = {};
      if (file.read(magic, 4) && (!std::memcmp(magic, "CON ", 4) || !std::memcmp(magic, "LIVE", 4) ||
                                  !std::memcmp(magic, "PIRS", 4))) {
        ++count;
      }
    }
  }
  return count;
}

void RegisterCustomSongs(rex::system::KernelState* kernel_state) {
  std::error_code ec;
  std::filesystem::create_directories(DefaultSongsFolder(), ec);
  for (const auto& folder : SongsFolders()) {
    if (!std::filesystem::is_directory(folder, ec)) {
      REXLOG_WARN("Custom songs: {} isn't a folder", rex::path_to_utf8(folder));
      continue;
    }
    size_t count = kernel_state->content_manager()->AddPackageFolder(folder);
    REXLOG_INFO("Custom songs: {} package(s) in {}", count, rex::path_to_utf8(folder));
  }
}

}  // namespace rb3pc

// max_song_count: 3000 in RB3, 8000 in Deluxe, 32000 in Deluxe PS3 high-mem.
// The real limit is the main heap, so it's grown by what high-mem would add.
constexpr uint32_t kMaxSongCount = 32000;
constexpr uint32_t kMainHeapExtra = 128u << 20;

// BandSongMgr::Init (r3): mMaxSongCount at +0x16C.
#ifdef RB3PC_DX
#define CS_SONG_MGR_INIT sub_825796C0
#define CS_SONG_MGR_INIT_ORIG __imp__sub_825796C0
#else
#define CS_SONG_MGR_INIT sub_82562040
#define CS_SONG_MGR_INIT_ORIG __imp__sub_82562040
#endif
REX_EXTERN(CS_SONG_MGR_INIT_ORIG);
REX_HOOK_RAW(CS_SONG_MGR_INIT) {
  uint32_t mgr = ctx.r3.u32;
  CS_SONG_MGR_INIT_ORIG(ctx, base);
  uint8_t* max = base + mgr + 0x16C;
  uint32_t configured = uint32_t(max[0]) << 24 | uint32_t(max[1]) << 16 | uint32_t(max[2]) << 8 | max[3];
  if (configured < kMaxSongCount) {
    max[0] = uint8_t(kMaxSongCount >> 24), max[1] = uint8_t(kMaxSongCount >> 16);
    max[2] = uint8_t(kMaxSongCount >> 8), max[3] = uint8_t(kMaxSongCount);
    REXLOG_INFO("Custom songs: song limit raised from {} to {}", configured, kMaxSongCount);
  }
}

// Creates heap r3 (in the order mem.dta lists them; 0 is main) of r4 bytes.
#ifdef RB3PC_DX
#define CS_HEAP_CREATE sub_827BC2D0
#define CS_HEAP_CREATE_ORIG __imp__sub_827BC2D0
#else
#define CS_HEAP_CREATE sub_82797670
#define CS_HEAP_CREATE_ORIG __imp__sub_82797670
#endif
REX_EXTERN(CS_HEAP_CREATE_ORIG);
REX_HOOK_RAW(CS_HEAP_CREATE) {
  if (ctx.r3.u32 == 0) {
    REXLOG_INFO("Custom songs: main heap {} MB, made {} MB", ctx.r4.u32 >> 20, (ctx.r4.u32 + kMainHeapExtra) >> 20);
    ctx.r4.u64 = ctx.r4.u32 + kMainHeapExtra;
  }
  CS_HEAP_CREATE_ORIG(ctx, base);
}
