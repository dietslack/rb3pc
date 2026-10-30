// GemPlayer::ChangeDifficulty (retail sub_826A5240 / DX sub_826C3D88) tells
// the player's track (mTrack) about the new difficulty without checking it
// has one; the source does check. The track panel unhooks every player's
// track while it rebuilds them (TrackPanel::CleanUpTracks), so a difficulty
// change landing then was a null read that crashed the game. Skip the call:
// the rebuilt track picks up the new difficulty when it hooks up again.
// See config/{retail,dx}/hooks.toml.

#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "exe.h"

// Before the track call: r31 = the GemPlayer. True skips the call.
bool Rb3pcChangeDifficultyNoTrack(PPCRegister& r31) {
  constexpr uint32_t kTrack = rb3pc::exe::kDeluxe ? 932 : 928;
  const uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t track;
  std::memcpy(&track, base + r31.u32 + kTrack, 4);
  if (track) return false;
  REXLOG_WARN("Difficulty changed while the player's track was being rebuilt; skipped the "
              "track update");
  return true;
}
