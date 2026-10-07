// The game's Xbox 360 title icon (the one shown on its saves and in the
// dashboard), taken from the XDBF resource in the loaded XEX at runtime.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace rb3pc {

// PNG bytes of the title icon, or empty if the XEX has none. Call after the
// XEX is loaded.
std::vector<uint8_t> TitleIconPng();

// Sets the icon of every SDL window (title bar / taskbar). UI thread only.
void ApplyWindowIcon(const std::vector<uint8_t>& png);

// Writes <dir>/rb3pc.png and a <dir>/rb3pc.desktop launcher that uses it, so
// file managers show the game with its icon. Linux has no per-executable icon.
void WriteDesktopLauncher(const std::vector<uint8_t>& png, const std::filesystem::path& dir,
                          const std::filesystem::path& executable);

}  // namespace rb3pc
