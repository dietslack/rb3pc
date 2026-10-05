#include "shm_cleanup.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <rex/logging.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace rb3pc {

namespace {

constexpr const char* kShmDir = "/dev/shm";
constexpr const char* kArenaPrefix = "xenia_memory_";

// Names of xenia_memory_* objects mapped by the processes whose maps files
// are given.
std::set<std::string> MappedArenas(const std::vector<std::filesystem::path>& maps_files) {
  std::set<std::string> names;
  for (const auto& maps : maps_files) {
    std::ifstream in(maps);
    std::string line;
    while (std::getline(in, line)) {
      auto pos = line.find(std::string(kShmDir) + "/" + kArenaPrefix);
      if (pos == std::string::npos) {
        continue;
      }
      auto name = line.substr(pos + std::string(kShmDir).size() + 1);
      if (auto end = name.find(' '); end != std::string::npos) {
        name.resize(end);  // drop a trailing " (deleted)"
      }
      names.insert(name);
    }
  }
  return names;
}

}  // namespace

void RemoveOrphanedGuestMemory() {
#if defined(__linux__)
  std::error_code ec;
  std::vector<std::filesystem::path> maps_files;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    auto name = entry.path().filename().string();
    if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos) {
      maps_files.push_back(entry.path() / "maps");
    }
  }
  auto in_use = MappedArenas(maps_files);

  uint32_t removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(kShmDir, ec)) {
    auto name = entry.path().filename().string();
    if (name.rfind(kArenaPrefix, 0) != 0 || in_use.count(name)) {
      continue;
    }
    if (shm_unlink(("/" + name).c_str()) == 0) {
      ++removed;
    }
  }
  if (removed) {
    REXLOG_INFO("Removed {} orphaned guest memory arena(s) from {}", removed, kShmDir);
  }
#endif
}

void UnlinkOwnGuestMemory() {
#if defined(__linux__)
  for (const auto& name : MappedArenas({"/proc/self/maps"})) {
    shm_unlink(("/" + name).c_str());
  }
#endif
}

}  // namespace rb3pc
