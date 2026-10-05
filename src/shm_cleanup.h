// Guest memory arena cleanup (Linux). The SDK's ~4.8 GB
// /dev/shm/xenia_memory_<ticks> is only unlinked on clean shutdown; the app
// hard-exits, and a full /dev/shm makes the next startup SIGBUS.

#pragma once

namespace rb3pc {

// Unmapped xenia_memory_* only. Call before the runtime creates its arena.
void RemoveOrphanedGuestMemory();

// Mapping stays valid; freed however the process exits. Call after setup.
void UnlinkOwnGuestMemory();

}  // namespace rb3pc
