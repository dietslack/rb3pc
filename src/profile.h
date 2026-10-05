// profile_name replaces the SDK's "User" gamertag. The XUID is fixed, so saves
// survive renames.

#pragma once

#include <string>

namespace rex::system {
class KernelState;
}  // namespace rex::system

namespace rb3pc {

constexpr size_t kMaxProfileNameLength = 15;

// profile_name, else the login name; sanitized.
std::string ProfileName();

// Printable ASCII, trimmed, truncated.
std::string SanitizeProfileName(const std::string& name);

// Call before the title starts.
void ApplyProfileName(rex::system::KernelState* kernel_state);

}  // namespace rb3pc
