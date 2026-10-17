#include "custom_songs.h"

#include <cstring>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
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
