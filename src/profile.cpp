#include "profile.h"

#include <pwd.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_STRING(profile_name, "", "rb3pc",
                      "Profile name (gamertag) shown in-game, up to 15 characters. Empty uses "
                      "your login name. Takes effect on restart.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rb3pc {

std::string SanitizeProfileName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (c >= 0x20 && c < 0x7F && !(c == ' ' && (out.empty() || out.back() == ' '))) {
      out += char(c);
    }
  }
  if (out.size() > kMaxProfileNameLength) out.resize(kMaxProfileNameLength);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string ProfileName() {
  std::string name = SanitizeProfileName(REXCVAR_GET(profile_name));
  if (!name.empty()) return name;
  if (const passwd* pw = getpwuid(getuid())) name = SanitizeProfileName(pw->pw_name);
  if (name.empty()) {
    if (const char* user = std::getenv("USER")) name = SanitizeProfileName(user);
  }
  if (name.empty()) return "Player";
  name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

void ApplyProfileName(rex::system::KernelState* kernel_state) {
  auto* profile = kernel_state ? kernel_state->user_profile() : nullptr;
  if (!profile) return;
  profile->set_name(ProfileName());
  REXLOG_INFO("Profile: signed in as \"{}\" (XUID {:016X})", profile->name(), profile->xuid());
}

}  // namespace rb3pc
