// Custom songs and DLC from CON / LIVE packages in the songs folders, mounted
// as profile-less content of type 1 (Content/0000000000000000/45410914/00000001)
// or 2 (marketplace) per their headers; RB3's own enumeration finds them.

#pragma once

#include <filesystem>
#include <vector>

namespace rex::system {
class KernelState;
}  // namespace rex::system

namespace rb3pc {

constexpr size_t kMaxExtraSongsFolders = 5;

// <user data>/rb3pc/songs, always read.
std::filesystem::path DefaultSongsFolder();

std::vector<std::filesystem::path> ExtraSongsFolders();

// ';' is the separator, so folders containing it can't be saved.
void SetExtraSongsFolders(const std::vector<std::filesystem::path>& folders);

// Default + extra, minus duplicates and folders nested in another.
std::vector<std::filesystem::path> SongsFolders();

// CON / LIVE / PIRS, recursive.
size_t CountSongPackages(const std::vector<std::filesystem::path>& folders);

// Call before the title starts.
void RegisterCustomSongs(rex::system::KernelState* kernel_state);

}  // namespace rb3pc
