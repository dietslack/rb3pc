// Startup script patches. ui/overshell/gen/slot_states.dtb is always patched
// so the difficulty list opens on the player's current difficulty (vanilla
// starts on Easy). With quick_start, ui/gen/ui.dtb boots to splash_screen and
// ui/splash/gen/splash.dtb presses Start for slot 1 from the poll handler
// (sv8_panel isn't loaded). A Splash constructor hook skips the boot logo's
// minimum time.
// Patched files go in an extra ARK part in a symlinked overlay of the game
// folder; the game's built-in checksums are updated to match. game: writes
// (Deluxe only) persist in the overlay.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace rb3pc {

bool QuickStartEnabled();

// Returns the overlay to use as game data root; nullopt (logged) on failure.
// `replacements`: top-level names replace the game's, "gen/..." are added.
std::optional<std::filesystem::path> PrepareScriptPatches(
    const std::filesystem::path& game_root, const std::filesystem::path& overlay_dir,
    const std::map<std::string, std::filesystem::path>& replacements = {});

// After the executable is loaded.
void ApplyScriptPatchChecksums();

}  // namespace rb3pc
