// Game frame-rate tracking, fed by a hook on the game's VdSwap call
// (config/retail/hooks.toml).

#pragma once

#include <rex/ui/overlay/debug_overlay.h>

namespace rb3pc {

// Frames presented by the game over roughly the last second.
rex::ui::FrameStats GuestFrameStats();

// The longest gap between two of those frames, in ms.
double GuestMaxFrameGapMs();

}  // namespace rb3pc
