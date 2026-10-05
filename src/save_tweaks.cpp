// Settings vanilla RB3 loses between boots (hooks in config/retail/hooks.toml).
// Breakneck Speed has no use_save_value, so it's never loaded or marked dirty.
// Difficulty is never saved at all; we keep it in rb3pc.toml as
// tour.default_difficulty. Changing a stand-in doesn't dirty the band profile.

#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(remember_breakneck_speed, true, "rb3pc",
                    "Keep the Breakneck Speed modifier on between sessions");
REXCVAR_DEFINE_BOOL(remember_difficulty, true, "rb3pc",
                    "Start players at the difficulty last picked instead of the game's default");
REXCVAR_DEFINE_INT32(remembered_difficulty, -1, "rb3pc",
                     "Last difficulty picked (0 easy - 3 expert, -1 none); set by the game");
REXCVAR_DEFINE_BOOL(save_stand_ins, true, "rb3pc",
                    "Save the band profile after changing stand-ins");

namespace {

constexpr int kNumDifficulties = 4;

uint8_t* Base() { return rex::system::kernel_state()->memory()->virtual_membase(); }

uint32_t GuestU32(uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, Base() + addr, sizeof(v));
  return __builtin_bswap32(v);
}

std::string_view GuestString(uint32_t addr) {
  if (!addr) return {};
  const char* s = reinterpret_cast<const char*>(Base() + addr);
  return std::string_view(s, strnlen(s, 64));
}

}  // namespace

// r29 = Modifier (mData, a DataArray*, at +0), r30 = UseSaveValue() result.
void Rb3pcModifierUseSaveValue(PPCRegister& r29, PPCRegister& r30) {
  if (r30.u32 || !REXCVAR_GET(remember_breakneck_speed)) return;
  // Node 0 is the name symbol (-> char*).
  uint32_t data = GuestU32(r29.u32);
  uint32_t nodes = data ? GuestU32(data) : 0;
  if (nodes && GuestString(GuestU32(nodes)) == "mod_doublespeed") {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("Save tweaks: Breakneck Speed is saved and restored");
    }
    r30.u64 = 1;
  }
}

// r3 = the default difficulty about to be returned.
void Rb3pcDefaultDifficulty(PPCRegister& r3) {
  int remembered = REXCVAR_GET(remembered_difficulty);
  if (REXCVAR_GET(remember_difficulty) && remembered >= 0 && remembered < kNumDifficulties) {
    static int logged = -1;
    if (logged != remembered) {
      logged = remembered;
      REXLOG_INFO("Save tweaks: players start at difficulty {} (last picked)", remembered);
    }
    r3.u64 = uint32_t(remembered);
  }
}

// r30 = difficulty a local player just picked.
void Rb3pcSetDifficulty(PPCRegister& r30) {
  int difficulty = int(r30.u32);
  if (difficulty < 0 || difficulty >= kNumDifficulties) return;
  if (difficulty == REXCVAR_GET(remembered_difficulty)) return;
  REXLOG_INFO("Save tweaks: remembering difficulty {}", difficulty);
  rex::cvar::SetFlagByName("remembered_difficulty", std::to_string(difficulty));
  rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "rb3pc.toml");
}

// r30 = ManageBandPanel; its mProfile (BandProfile*) is at +80.
void Rb3pcStandInChanged(PPCRegister& r30) {
  if (!REXCVAR_GET(save_stand_ins)) return;
  uint32_t profile = GuestU32(r30.u32 + 80);
  if (!profile) return;
  // Profile: bool mDirty +0xC, int mPadNum +0x10; sanity-checked.
  uint8_t& dirty = Base()[profile + 0xC];
  uint32_t pad = GuestU32(profile + 0x10);
  if (pad >= 4 || dirty > 1) {
    REXLOG_WARN("Stand-ins: unexpected profile layout at {:08X} (pad {}, dirty {})", profile,
                pad, dirty);
    return;
  }
  if (!dirty) REXLOG_INFO("Save tweaks: stand-in changed, band profile will be saved");
  dirty = 1;
}
