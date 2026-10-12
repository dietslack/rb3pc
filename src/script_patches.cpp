#include "script_patches.h"

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "ark.h"
#include "dtb.h"
#include "exe.h"
#include "sha1.h"

REXCVAR_DEFINE_BOOL(quick_start, false, "rb3pc",
                    "Skip the intro videos and title screen and go straight to the main menu");

REXCVAR_DECLARE(std::string, debug_input_script);
REXCVAR_DEFINE_BOOL(debug_autoplay, false, "rb3pc",
                    "Testing aid: players picking a difficulty in the overshell play on autoplay")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(debug_venue, "", "rb3pc",
                      "Testing aid: play every song in this venue (e.g. small_club_01) instead of "
                      "a random one, for repeatable performance measurements")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rb3pc {

namespace {

namespace fs = std::filesystem;

// Same as the title screen's SELECT_MSG handler, for slot 1.
constexpr std::string_view kSplashAutoStart = R"(
  {if {&& {== [splash_state] kSplashScreen_Entered}
          {! {ui in_transition}}
          {== {ui current_screen} splash_screen}}
      {set [last_user] {user_mgr get_user_from_pad_num 0}}
      {$this set_splash_state kSplashScreen_ActivateSaveLoad}}
)";

struct PatchedFile {
  std::string ark_path;        // e.g. "ui/gen/ui.dtb"
  std::array<uint8_t, 20> original_sha1, patched_sha1;
};
std::vector<PatchedFile> g_patched;

bool PatchUi(DtbArray& root) {
  DtbArray* set = FindArray(root, {"set", "$first_screen", "intro_movie_screen"});
  if (!set) return false;
  set->nodes[2].text = "splash_screen";
  return true;
}

bool PatchSplash(DtbArray& root) {
  // Passed straight through; skip loading sv8_panel.
  DtbArray* panels = FindArray(root, {"panels", "meta", "sv8_panel"});
  if (!panels) return false;
  panels->nodes.erase(panels->nodes.begin() + 2);
  DtbArray* panel = FindArray(root, {"new", "UIPanel", "splash_panel"});
  if (!panel) return false;
  auto code = ParseDta(kSplashAutoStart);
  if (!code) return false;
  for (auto& node : panel->nodes) {
    if (node.array && !node.array->nodes.empty() && node.array->nodes[0].IsSymbol("poll")) {
      auto& nodes = node.array->nodes;
      nodes.insert(nodes.begin() + 1, code->begin(), code->end());
      return true;
    }
  }
  return false;
}

// kState_ChooseDiff sets no default option, so the list resets to Easy each
// time. Entries are in Difficulty order (easy, medium, hard, expert).
constexpr std::string_view kChooseDiffEnter = R"(
  (enter
    {if {$this get_user}
        {handle ($this set_default_option_index {{$this get_user} get_difficulty})}})
)";

bool PatchSlotStates(DtbArray& root) {
  DtbArray* state = FindArray(root, {"kState_ChooseDiff"});
  if (!state) return false;
  for (const auto& node : state->nodes) {
    if (node.array && !node.array->nodes.empty() && node.array->nodes[0].IsSymbol("enter")) {
      return false;
    }
  }
  std::string enter(kChooseDiffEnter);
  if (REXCVAR_GET(debug_autoplay)) {
    enter.insert(enter.rfind(')'), "\n    {if {$this get_user} {{$this get_user} set autoplay TRUE}}");
  }
  auto code = ParseDta(enter);
  if (!code) return false;
  state->nodes.insert(state->nodes.end(), code->begin(), code->end());
  return true;
}

// debug_venue: replaces {meta_performer select_random_venue} on panel load.
bool PatchSelDiff(DtbArray& root) {
  DtbArray* call = FindArray(root, {"meta_performer", "select_random_venue"});
  if (!call) return false;
  call->nodes[1].text = "set_venue";
  DtbNode venue;
  venue.type = DtbNode::kSymbol;
  venue.text = REXCVAR_GET(debug_venue);
  call->nodes.push_back(venue);
  return true;
}

std::optional<std::vector<uint8_t>> PatchDtb(const Ark& ark, const std::string& path,
                                             bool (*patch)(DtbArray&)) {
  auto stored = ark.Read(path);
  if (stored.size() < 5) return std::nullopt;
  auto root = ParseDtb(HarmonixDecrypt(stored));
  if (!root || !patch(*root)) return std::nullopt;
  uint32_t key = stored[0] | stored[1] << 8 | stored[2] << 16 | uint32_t(stored[3]) << 24;
  auto out = HarmonixEncrypt(SerializeDtb(*root), key);
  g_patched.push_back({path, Sha1(stored), Sha1(out)});
  return out;
}

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

}  // namespace

bool QuickStartEnabled() {
  // Input scripts expect the normal screens; DX uses its own options.
  return REXCVAR_GET(quick_start) && REXCVAR_GET(debug_input_script).empty() && !exe::kDeluxe;
}

std::optional<fs::path> PrepareScriptPatches(const fs::path& game_root, const fs::path& overlay_dir,
                                             const std::map<std::string, fs::path>& replacements) {
  g_patched.clear();
  Ark ark;
  if (!ark.Open(game_root)) {
    REXLOG_WARN("Script patches: couldn't read the game's ARK; booting unpatched");
    return std::nullopt;
  }
  struct Patch {
    const char* path;
    bool (*fn)(DtbArray&);
  };
  std::vector<Patch> patches{{"ui/overshell/gen/slot_states.dtb", PatchSlotStates}};
  if (!REXCVAR_GET(debug_venue).empty()) patches.push_back({"ui/seldiff/gen/seldiff.dtb", PatchSelDiff});
  if (QuickStartEnabled()) {
    patches.push_back({"ui/gen/ui.dtb", PatchUi});
    patches.push_back({"ui/splash/gen/splash.dtb", PatchSplash});
  }
  ArkHeader header = ark.header();
  std::vector<uint8_t> part;  // extra ARK part
  uint64_t part_base = 0;
  for (auto size : header.part_sizes) part_base += size;
  for (const Patch& p : patches) {
    auto index = header.Find(p.path);
    auto data = index ? PatchDtb(ark, p.path, p.fn) : std::nullopt;
    if (!data) {
      REXLOG_WARN("Script patches: couldn't patch {}; left as is", p.path);
      continue;
    }
    auto& entry = header.entries[*index];
    bool stored_uncompressed = entry.ucsize == entry.size;
    entry.offset = part_base + part.size();
    entry.size = uint32_t(data->size());
    entry.ucsize = stored_uncompressed ? entry.size : 0;
    part.insert(part.end(), data->begin(), data->end());
  }
  if (g_patched.empty()) return std::nullopt;
  std::string part_path = "gen/main_xbox_" + std::to_string(header.part_sizes.size()) + ".ark";
  header.part_count += 1;
  header.part_sizes.push_back(uint32_t(part.size()));
  header.part_paths.push_back(part_path);
  header.cache_priorities.push_back(header.cache_priorities.empty() ? -1
                                                                    : header.cache_priorities.back());

  // Symlinked game folder; game-written top-level files are kept, except
  // discordrp.json: Deluxe hangs at boot on a stale one.
  std::error_code ec;
  fs::remove(overlay_dir / "discordrp.json", ec);
  for (const auto& entry : fs::directory_iterator(overlay_dir, ec)) {
    if (!entry.is_symlink(ec) && entry.is_regular_file(ec)) continue;
    fs::remove_all(entry.path(), ec);
  }
  fs::create_directories(overlay_dir / "gen", ec);
  fs::path root = fs::absolute(game_root);
  auto link = [&](const fs::path& target, const fs::path& at) {
    if (fs::is_directory(target)) {
      fs::create_directory_symlink(target, at, ec);
    } else {
      fs::create_symlink(target, at, ec);
    }
    return !ec;
  };
  bool ok = true;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    std::string name = entry.path().filename().string();
    if (name == "gen") continue;
    auto replacement = replacements.find(name);
    ok &= link(replacement != replacements.end() ? replacement->second : entry.path(),
               overlay_dir / name);
  }
  for (const auto& entry : fs::directory_iterator(root / "gen", ec)) {
    if (entry.path().filename() != "main_xbox.hdr") {
      ok &= link(entry.path(), overlay_dir / "gen" / entry.path().filename());
    }
  }
  for (const auto& [name, target] : replacements) {
    if (!name.starts_with("gen/")) continue;
    fs::remove(overlay_dir / name, ec);
    ok &= link(target, overlay_dir / name);
  }
  ok &= WriteFile(overlay_dir / part_path, part);
  ok &= WriteFile(overlay_dir / "gen" / "main_xbox.hdr",
                  HarmonixEncrypt(header.Serialize(), ark.header_key()));
  if (!ok) {
    g_patched.clear();
    REXLOG_WARN("Script patches: couldn't build {}; booting unpatched", overlay_dir.string());
    return std::nullopt;
  }
  REXLOG_INFO("Script patches: patched {} scripts into {}", g_patched.size(),
              (overlay_dir / part_path).string());
  return overlay_dir;
}

void ApplyScriptPatchChecksums() {
  if (g_patched.empty()) return;
  // Checksum table in .data: { char* path, u8 sha1[20] }, paths in .rdata.
  using exe::kRdata, exe::kRdataSize, exe::kData, exe::kDataSize;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (const auto& file : g_patched) {
    std::string name = "./" + file.ark_path;
    std::string needle = std::string(1, '\0') + name + std::string(1, '\0');
    const uint8_t* rdata = base + kRdata;
    auto* found = static_cast<const uint8_t*>(
        memmem(rdata, kRdataSize, needle.data(), needle.size()));
    bool patched = false;
    if (found) {
      uint32_t addr = kRdata + uint32_t(found - rdata) + 1;
      uint8_t ptr[4] = {uint8_t(addr >> 24), uint8_t(addr >> 16), uint8_t(addr >> 8), uint8_t(addr)};
      uint8_t* data = base + kData;
      for (uint32_t off = 0; off + 24 <= kDataSize && !patched; off += 4) {
        if (std::memcmp(data + off, ptr, 4) != 0) continue;
        uint8_t* sha1 = data + off + 4;
        if (std::memcmp(sha1, file.original_sha1.data(), 20) == 0) {
          std::memcpy(sha1, file.patched_sha1.data(), 20);
          patched = true;
        }
      }
    }
    if (patched) {
      REXLOG_INFO("Script patches: checksum for {} updated", name);
    } else {
      REXLOG_WARN("Script patches: no checksum entry for {}; the game may report a disc error", name);
    }
  }
}

}  // namespace rb3pc

// Mid-asm hook in Splash's ctor; r30 = Splash. Logos show only while loading.
void Rb3pcSplashCreated(PPCRegister& r30) {
  if (!rb3pc::QuickStartEnabled()) return;
  uint8_t* splash = rex::system::kernel_state()->memory()->virtual_membase() + r30.u32;
  std::memset(splash + 8, 0, 4);  // mSplashTime
  splash[12] = 0;                 // mWaitForSplash
}
