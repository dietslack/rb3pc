// Up to three PC recording devices as Xbox 360 USB mics (lead + harmonies).
// Replaces Synth360's per-USB-port mic threads, so mic_N is port N-1's mic
// with no USB emulation. Mics take no player slot.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rb3pc {

inline constexpr int kMicCount = 3;

std::vector<std::string> MicDeviceNames();

// Device name for `mic` (0-2); empty for none.
std::string MicSetting(int mic);
void SetMicSetting(int mic, const std::string& device);

bool MicOpen(int mic);

// Up to `max` samples recorded since the last call: 48 kHz, mono, 16-bit.
// Any thread.
size_t ReadMic(int mic, int16_t* out, size_t max);

// Current level, 0-1. Reads the device itself while the game isn't.
float MicLevel(int mic);

}  // namespace rb3pc
