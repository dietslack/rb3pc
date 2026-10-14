// Rock Band 3 Deluxe (RB3DX): install and load.
// Release = patch ARK (gen/patch_xbox.hdr + _N.ark) plus DX's own TU5-based
// default.xex, built as rb3pc-dx (src/exe.h). rb3pc hands off to rb3pc-dx when
// DX is on and vice versa; both read rb3pc.toml.

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace rb3pc {

// <user folder>/rb3pc/deluxe.
std::filesystem::path DeluxeInstallDir();

bool DeluxeInstalled();

// Source zip name, or empty.
std::string DeluxeInstalledName();

// Empty if DX can run.
std::string DeluxeProblem();

// `deluxe` cvar and installed.
bool DeluxeEnabled();
void SetDeluxeEnabled(bool on);

// From a release zip or folder; replaces any earlier install. Returns an
// error on failure. Any thread.
std::optional<std::string> InstallDeluxe(const std::filesystem::path& source,
                                         std::atomic<float>* progress = nullptr);

// rb3pc-dx: overlay files keyed by overlay path (default.xex, gen/patch_xbox*).
// Empty (and logged) if DX can't run.
std::map<std::string, std::filesystem::path> DeluxeOverlayFiles();

// rb3pc-dx is only built when game/rb3dx has DX's executable.
bool DeluxeExecutableAvailable();

// DX on in rb3pc, or off in rb3pc-dx.
bool NeedsOtherExecutable();

// exec()s the other executable with the same args, skipping its launcher.
// Returns an error message on failure.
std::string RunOtherExecutable();

// Set in a process started by RunOtherExecutable: the launcher already ran.
bool LaunchedByOtherExecutable();

// rb3pc-dx: writes dx_settings.dta in the overlay from launcher options.
void ApplyDeluxeOptions(const std::filesystem::path& game_root, bool fps_counter,
                        bool quick_start);

// rb3pc-dx: logs whether the installed DX version is one of the variants the
// generated code switches on (tools/dx_variants.py).
void CheckDeluxeCode();

}  // namespace rb3pc
