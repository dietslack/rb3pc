// MIDI input ports (ALSA seq / WinMM) as Xbox 360 instruments. A pro drums
// device takes a player slot; each note-on briefly presses the kit's buttons.
// Note map per device in <exe dir>/bindings/midi_<device>.toml, GM by default.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "input_bindings.h"

namespace rb3pc {

enum class MidiType { kNone, kProDrums };
const char* MidiTypeName(MidiType type);  // "none", "pro_drums"
const char* MidiTypeLabel(MidiType type);  // "None", "Pro drums"

// Display order.
enum class DrumInput : uint8_t {
  kRedPad, kYellowPad, kBluePad, kGreenPad,
  kYellowCymbal, kBlueCymbal, kGreenCymbal,
  kKick,
  kStart, kBack, kUp, kDown, kLeft, kRight,
  kCount,
};
constexpr size_t kDrumInputCount = size_t(DrumInput::kCount);
const char* DrumInputKey(DrumInput input);    // stable name used in profile files
const char* DrumInputLabel(DrumInput input);  // for the UI

struct MidiProfile {
  std::string key;   // the device's identity (MidiDevice::key)
  std::string name;  // the device's name, for the file
  MidiType type = MidiType::kProDrums;
  std::array<std::vector<uint8_t>, kDrumInputCount> notes{};  // MIDI note numbers
};
// The General MIDI drum map, laid out as Rock Band's pro kit.
MidiProfile DefaultMidiProfile(const std::string& key, const std::string& name);
std::string DescribeNotes(const std::vector<uint8_t>& notes);  // "38 (snare), 40"

struct MidiDevice {
  std::string key;   // stable across reconnects: "<client>:<port>" names
  std::string name;  // for display
  bool connected = false;
};

struct MidiHit {
  uint8_t note = 0, velocity = 0, channel = 0;
  uint64_t serial = 0;  // counts hits, so a repeat of the same note shows
  std::chrono::steady_clock::time_point at{};
};

// Opens every MIDI input port and keeps watching for new ones. Thread-safe.
class MidiInput {
 public:
  static MidiInput& Get();

  void SetBindingsDirectory(std::filesystem::path dir);

  // Starts the reading thread (once).
  void Start();
  void Stop();

  // Every device seen since Start, in discovery order.
  std::vector<MidiDevice> Devices();

  MidiProfile Profile(const std::string& key);
  bool SaveProfile(const MidiProfile& profile);
  void ResetProfile(const std::string& key);

  // The Xbox 360 state of a pro drums device, advancing its hits (call from
  // the input poll). `changed` is set if it differs from the last call's.
  PadState PollDrums(const std::string& key, bool& changed);
  // Inputs hit in the last moment, without advancing anything (for display).
  std::array<bool, kDrumInputCount> RecentlyHit(const std::string& key);
  std::optional<MidiHit> LastHit(const std::string& key);

  struct Impl;

 private:
  MidiInput() = default;
  Impl* impl();
};

}  // namespace rb3pc
