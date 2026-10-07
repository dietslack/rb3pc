#include "title_icon.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace rb3pc {

namespace {

// Wayland matches the app ID to a .desktop file for the taskbar icon. SDL's
// default "rb3pc" can clash with another rb3pc.desktop; set before any window.
constexpr const char* kAppId = "rock-band-3";
const bool kAppIdHintSet = SDL_SetHint(SDL_HINT_APP_ID, kAppId);

void WriteIfChanged(const std::filesystem::path& path, const std::string& bytes) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream current;
  current << in.rdbuf();
  if (in && current.str() == bytes) return;
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), std::streamsize(bytes.size()));
  if (!out) REXLOG_WARN("Icon: failed to write {}", path.string());
}

}  // namespace

std::vector<uint8_t> TitleIconPng() {
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return {};
  auto icon = kernel->title_xdbf().icon();
  if (!icon) return {};
  return std::vector<uint8_t>(icon.buffer, icon.buffer + icon.size);
}

void ApplyWindowIcon(const std::vector<uint8_t>& png) {
  if (png.empty()) return;
  SDL_IOStream* io = SDL_IOFromConstMem(png.data(), png.size());
  SDL_Surface* surface = io ? SDL_LoadPNG_IO(io, true) : nullptr;
  if (!surface) {
    REXLOG_WARN("Icon: couldn't decode the title icon: {}", SDL_GetError());
    return;
  }
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  for (int i = 0; windows && i < count; ++i) {
    SDL_SetWindowIcon(windows[i], surface);
  }
  SDL_free(windows);
  SDL_DestroySurface(surface);
}

void WriteDesktopLauncher(const std::vector<uint8_t>& png, const std::filesystem::path& dir,
                          const std::filesystem::path& executable) {
  if (png.empty()) return;
  auto icon_path = dir / "rb3pc.png";
  WriteIfChanged(icon_path, std::string(png.begin(), png.end()));

  // Exec/Icon must be quoted or absolute: the path may contain spaces.
  std::ostringstream desktop;
  desktop << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=Rock Band 3\n"
          << "Comment=Rock Band 3 PC port\n"
          << "Exec=\"" << executable.string() << "\"\n"
          << "Path=" << dir.string() << "\n"
          << "Icon=" << icon_path.string() << "\n"
          << "Terminal=false\n"
          << "Categories=Game;\n"
          << "StartupWMClass=" << kAppId << "\n";

  // Next to the binary, for file managers.
  auto local_path = dir / "rb3pc.desktop";
  WriteIfChanged(local_path, desktop.str());
  std::error_code ec;
  std::filesystem::permissions(local_path,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add, ec);

  // Named by app ID so the compositor matches it.
  std::filesystem::path data_home;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    data_home = xdg;
  } else if (const char* home = std::getenv("HOME"); home && *home) {
    data_home = std::filesystem::path(home) / ".local/share";
  }
  if (!data_home.empty()) {
    auto apps = data_home / "applications";
    std::filesystem::create_directories(apps, ec);
    WriteIfChanged(apps / (std::string(kAppId) + ".desktop"), desktop.str());
  }
}

}  // namespace rb3pc
