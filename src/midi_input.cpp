#include "midi_input.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <toml++/toml.hpp>

#include <rex/input/input.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <poll.h>
#include <alsa/asoundlib.h>
#endif

namespace rb3pc {

namespace {

using Clock = std::chrono::steady_clock;

// Min hold per hit, and at least one poll down and one up, so fast rolls
// arrive as separate presses.
constexpr auto kHitHold = std::chrono::milliseconds(30);
constexpr auto kRecentHit = std::chrono::milliseconds(150);

constexpr const char* kInputKeys[] = {
    "red_pad",       "yellow_pad",  "blue_pad",     "green_pad", "yellow_cymbal",
    "blue_cymbal",   "green_cymbal", "kick",        "start",     "back",
    "up",            "down",        "left",         "right",
};
constexpr const char* kInputLabels[] = {
    "Red pad",       "Yellow pad",   "Blue pad",     "Green pad",
    "Yellow cymbal", "Blue cymbal",  "Green cymbal", "Kick pedal",
    "Start",         "Back",         "D-pad up",     "D-pad down",
    "D-pad left",    "D-pad right",
};
static_assert(std::size(kInputKeys) == kDrumInputCount);
static_assert(std::size(kInputLabels) == kDrumInputCount);

const char* GmDrumName(uint8_t note) {
  switch (note) {
    case 22: return "hi-hat closed edge";
    case 26: return "hi-hat open edge";
    case 35: return "kick 2";
    case 36: return "kick";
    case 37: return "side stick";
    case 38: return "snare";
    case 40: return "snare rim";
    case 41: return "floor tom 2";
    case 42: return "hi-hat closed";
    case 43: return "floor tom";
    case 44: return "hi-hat pedal";
    case 45: return "tom 2";
    case 46: return "hi-hat open";
    case 47: return "tom 2 rim";
    case 48: return "tom 1";
    case 49: return "crash";
    case 50: return "tom 1 rim";
    case 51: return "ride";
    case 52: return "china";
    case 53: return "ride bell";
    case 55: return "splash";
    case 57: return "crash 2";
    case 59: return "ride 2";
    default: return nullptr;
  }
}

std::string FileStem(const std::string& key) {
  std::string stem = "midi_";
  for (char c : key) stem += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
  return stem;
}

struct InputState {
  int pending = 0;  // hits not shown yet
  bool down = false;
  bool polled = false;  // a poll has seen the current down / up state
  Clock::time_point since{};
  uint8_t velocity = 0;
  Clock::time_point last_hit{};
};

struct DeviceState {
  MidiDevice device;
  std::optional<MidiProfile> profile;  // loaded on first use
  std::array<InputState, kDrumInputCount> inputs{};
  uint16_t last_buttons = 0;
  std::optional<MidiHit> last_hit;
  uint64_t hits = 0;
};

}  // namespace

struct MidiInput::Impl {
  std::mutex mutex;
  std::filesystem::path dir;
  std::vector<DeviceState> devices;
  std::thread thread;
  std::atomic<bool> running{false};

  DeviceState& DeviceFor(const std::string& key, const std::string& name) {
    for (auto& d : devices) {
      if (d.device.key == key) return d;
    }
    devices.push_back({});
    devices.back().device = {key, name, false};
    return devices.back();
  }
  DeviceState* Find(const std::string& key) {
    for (auto& d : devices) {
      if (d.device.key == key) return &d;
    }
    return nullptr;
  }

  const MidiProfile& ProfileOf(DeviceState& d) {
    if (!d.profile) d.profile = Load(d.device.key, d.device.name);
    return *d.profile;
  }

  MidiProfile Load(const std::string& key, const std::string& name) {
    MidiProfile profile = DefaultMidiProfile(key, name);
    auto path = dir / (FileStem(key) + ".toml");
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(path, ec)) return profile;
    try {
      auto table = toml::parse_file(path.string());
      profile.type = table["type"].value_or(std::string{}) == "none" ? MidiType::kNone
                                                                     : MidiType::kProDrums;
      if (auto* notes = table["notes"].as_table()) {
        for (size_t i = 0; i < kDrumInputCount; ++i) {
          if (auto* list = (*notes)[kInputKeys[i]].as_array()) {
            profile.notes[i].clear();
            for (auto& n : *list) {
              if (auto v = n.value<int64_t>(); v && *v >= 0 && *v < 128) {
                profile.notes[i].push_back(uint8_t(*v));
              }
            }
          }
        }
      }
      REXLOG_INFO("MIDI: loaded {} ({})", path.string(), name);
    } catch (const toml::parse_error& e) {
      REXLOG_ERROR("MIDI: failed to parse {}: {}", path.string(), e.what());
    }
    return profile;
  }

  void Hit(const std::string& key, uint8_t note, uint8_t velocity, uint8_t channel) {
    std::lock_guard lock(mutex);
    DeviceState* d = Find(key);
    if (!d) return;
    auto now = Clock::now();
    d->last_hit = MidiHit{note, velocity, channel, ++d->hits, now};
    const MidiProfile& profile = ProfileOf(*d);
    for (size_t i = 0; i < kDrumInputCount; ++i) {
      const auto& notes = profile.notes[i];
      if (std::find(notes.begin(), notes.end(), note) == notes.end()) continue;
      auto& input = d->inputs[i];
      input.pending = std::min(input.pending + 1, 8);
      input.velocity = velocity;
      input.last_hit = now;
    }
  }

  void Connected(const std::string& key, const std::string& name) {
    std::lock_guard lock(mutex);
    DeviceState& d = DeviceFor(key, name);
    if (!d.device.connected) REXLOG_INFO("MIDI: {} connected", name);
    d.device.connected = true;
  }
  void Disconnected(const std::string& key) {
    std::lock_guard lock(mutex);
    if (DeviceState* d = Find(key); d && d->device.connected) {
      REXLOG_INFO("MIDI: {} disconnected", d->device.name);
      d->device.connected = false;
      d->inputs = {};
    }
  }

  void Run();
};

#ifndef _WIN32

// One own port subscribed to every readable port except Midi Through.
void MidiInput::Impl::Run() {
  snd_seq_t* seq = nullptr;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    REXLOG_WARN("MIDI: couldn't open the ALSA sequencer; no MIDI instruments");
    return;
  }
  snd_seq_set_client_name(seq, "rb3pc");
  int own_port = snd_seq_create_simple_port(
      seq, "rb3pc instruments", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  int own_client = snd_seq_client_id(seq);
  snd_seq_connect_from(seq, own_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

  std::map<std::pair<int, int>, std::string> subscribed;  // (client, port) -> key
  auto scan = [&] {
    std::map<std::pair<int, int>, std::string> found;
    snd_seq_client_info_t* client;
    snd_seq_port_info_t* port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0) {
      int id = snd_seq_client_info_get_client(client);
      std::string client_name = snd_seq_client_info_get_name(client);
      if (id == own_client || id == SND_SEQ_CLIENT_SYSTEM || client_name == "Midi Through") continue;
      snd_seq_port_info_set_client(port, id);
      snd_seq_port_info_set_port(port, -1);
      while (snd_seq_query_next_port(seq, port) >= 0) {
        unsigned caps = snd_seq_port_info_get_capability(port);
        if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
                (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ) ||
            (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
          continue;
        }
        std::string port_name = snd_seq_port_info_get_name(port);
        int port_id = snd_seq_port_info_get_port(port);
        found[{id, port_id}] = client_name + ":" + port_name;
      }
    }
    for (const auto& [addr, key] : found) {
      if (subscribed.count(addr)) continue;
      if (snd_seq_connect_from(seq, own_port, addr.first, addr.second) >= 0) {
        std::string name = key.substr(key.find(':') + 1);
        Connected(key, name);
      }
    }
    for (const auto& [addr, key] : subscribed) {
      if (!found.count(addr)) Disconnected(key);
    }
    subscribed = std::move(found);
  };
  scan();

  int count = snd_seq_poll_descriptors_count(seq, POLLIN);
  std::vector<pollfd> fds(size_t(std::max(count, 1)));
  auto last_scan = Clock::now();
  while (running) {
    snd_seq_poll_descriptors(seq, fds.data(), unsigned(fds.size()), POLLIN);
    ::poll(fds.data(), nfds_t(fds.size()), 200);
    bool rescan = Clock::now() - last_scan > std::chrono::seconds(2);
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
      switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
          if (ev->data.note.velocity > 0) {
            if (auto it = subscribed.find({ev->source.client, ev->source.port}); it != subscribed.end()) {
              Hit(it->second, ev->data.note.note, ev->data.note.velocity, ev->data.note.channel);
            }
          }
          break;
        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_CLIENT_START:
        case SND_SEQ_EVENT_CLIENT_EXIT:
          rescan = true;
          break;
        default:
          break;
      }
    }
    if (rescan) {
      scan();
      last_scan = Clock::now();
    }
  }
  snd_seq_close(seq);
}

#else

// WinMM: reopens all devices when the list changes (polled every 2 s).
namespace {
MidiInput::Impl* g_winmm_impl = nullptr;
std::vector<std::string> g_winmm_keys;  // by open slot

void CALLBACK WinmmCallback(HMIDIIN, UINT message, DWORD_PTR slot, DWORD_PTR param1, DWORD_PTR) {
  if (message != MIM_DATA || !g_winmm_impl || slot >= g_winmm_keys.size()) return;
  uint8_t status = uint8_t(param1), note = uint8_t(param1 >> 8), velocity = uint8_t(param1 >> 16);
  if ((status & 0xF0) == 0x90 && velocity > 0) {
    g_winmm_impl->Hit(g_winmm_keys[slot], note, velocity, status & 0x0F);
  }
}
}  // namespace

void MidiInput::Impl::Run() {
  g_winmm_impl = this;
  std::vector<HMIDIIN> open;
  std::vector<std::string> names;
  auto close_all = [&] {
    for (HMIDIIN h : open) {
      midiInStop(h);
      midiInClose(h);
    }
    open.clear();
  };
  while (running) {
    std::vector<std::string> now;
    UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
      MIDIINCAPSW caps{};
      if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name) - 1, nullptr, nullptr);
      std::string key = name;
      int same = int(std::count(now.begin(), now.end(), key));
      if (same) key += " #" + std::to_string(same + 1);
      now.push_back(key);
    }
    if (now != names) {
      close_all();
      for (const auto& key : names) {
        if (std::find(now.begin(), now.end(), key) == now.end()) Disconnected(key);
      }
      g_winmm_keys = now;
      for (UINT i = 0; i < now.size(); ++i) {
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, DWORD_PTR(&WinmmCallback), DWORD_PTR(i), CALLBACK_FUNCTION) ==
            MMSYSERR_NOERROR) {
          midiInStart(h);
          open.push_back(h);
          Connected(now[i], now[i]);
        }
      }
      names = now;
    }
    for (int i = 0; i < 20 && running; ++i) Sleep(100);
  }
  close_all();
  g_winmm_impl = nullptr;
}

#endif

const char* MidiTypeName(MidiType type) { return type == MidiType::kNone ? "none" : "pro_drums"; }
const char* MidiTypeLabel(MidiType type) { return type == MidiType::kNone ? "None" : "Pro drums"; }
const char* DrumInputKey(DrumInput input) { return kInputKeys[size_t(input)]; }
const char* DrumInputLabel(DrumInput input) { return kInputLabels[size_t(input)]; }

MidiProfile DefaultMidiProfile(const std::string& key, const std::string& name) {
  MidiProfile p;
  p.key = key;
  p.name = name;
  auto set = [&](DrumInput input, std::vector<uint8_t> notes) { p.notes[size_t(input)] = std::move(notes); };
  set(DrumInput::kRedPad, {38, 40, 37});
  set(DrumInput::kYellowPad, {48, 50});
  set(DrumInput::kBluePad, {45, 47});
  set(DrumInput::kGreenPad, {43, 41});
  set(DrumInput::kYellowCymbal, {42, 46, 22, 26});
  set(DrumInput::kBlueCymbal, {51, 53, 59});
  set(DrumInput::kGreenCymbal, {49, 57, 55, 52});
  set(DrumInput::kKick, {36, 35});
  return p;
}

std::string DescribeNotes(const std::vector<uint8_t>& notes) {
  if (notes.empty()) return "-";
  std::string text;
  for (uint8_t n : notes) {
    if (!text.empty()) text += ", ";
    text += std::to_string(n);
    if (const char* name = GmDrumName(n)) text += std::string(" (") + name + ")";
  }
  return text;
}

MidiInput& MidiInput::Get() {
  static MidiInput instance;
  return instance;
}

MidiInput::Impl* MidiInput::impl() {
  static Impl instance;
  return &instance;
}

void MidiInput::SetBindingsDirectory(std::filesystem::path dir) {
  std::lock_guard lock(impl()->mutex);
  impl()->dir = std::move(dir);
}

void MidiInput::Start() {
  Impl* i = impl();
  if (i->running.exchange(true)) return;
  i->thread = std::thread([i] { i->Run(); });
}

void MidiInput::Stop() {
  Impl* i = impl();
  if (!i->running.exchange(false)) return;
  if (i->thread.joinable()) i->thread.join();
}

std::vector<MidiDevice> MidiInput::Devices() {
  std::lock_guard lock(impl()->mutex);
  std::vector<MidiDevice> out;
  for (const auto& d : impl()->devices) out.push_back(d.device);
  return out;
}

MidiProfile MidiInput::Profile(const std::string& key) {
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  DeviceState* d = i->Find(key);
  return d ? i->ProfileOf(*d) : DefaultMidiProfile(key, key);
}

bool MidiInput::SaveProfile(const MidiProfile& profile) {
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  std::error_code ec;
  std::filesystem::create_directories(i->dir, ec);
  toml::table notes;
  for (size_t n = 0; n < kDrumInputCount; ++n) {
    toml::array list;
    for (uint8_t note : profile.notes[n]) list.push_back(int64_t(note));
    notes.insert_or_assign(kInputKeys[n], std::move(list));
  }
  toml::table table;
  table.insert_or_assign("name", profile.name);
  table.insert_or_assign("device", profile.key);
  table.insert_or_assign("type", MidiTypeName(profile.type));
  table.insert_or_assign("notes", std::move(notes));
  auto path = i->dir / (FileStem(profile.key) + ".toml");
  std::ofstream out(path);
  out << "# rb3pc MIDI instrument bindings for " << profile.name << "\n"
      << "# type: pro_drums or none. notes: the MIDI note numbers that hit each input.\n\n"
      << table << "\n";
  if (!out) {
    REXLOG_ERROR("MIDI: failed to write {}", path.string());
    return false;
  }
  if (DeviceState* d = i->Find(profile.key)) d->profile = profile;
  REXLOG_INFO("MIDI: saved {}", path.string());
  return true;
}

void MidiInput::ResetProfile(const std::string& key) {
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  std::error_code ec;
  std::filesystem::remove(i->dir / (FileStem(key) + ".toml"), ec);
  if (DeviceState* d = i->Find(key)) d->profile.reset();
}

PadState MidiInput::PollDrums(const std::string& key, bool& changed) {
  using namespace rex::input;
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  PadState pad;
  changed = false;
  DeviceState* d = i->Find(key);
  if (!d) return pad;
  auto now = Clock::now();
  std::array<bool, kDrumInputCount> held{};
  for (size_t n = 0; n < kDrumInputCount; ++n) {
    InputState& s = d->inputs[n];
    if (s.down && s.polled && now - s.since >= kHitHold) {
      s.down = false;
      s.polled = false;
      s.since = now;
    } else if (!s.down && s.pending > 0 && (s.polled || s.since == Clock::time_point{})) {
      s.down = true;
      s.polled = false;
      s.since = now;
      --s.pending;
    }
    if (!s.polled) s.polled = true;
    held[n] = s.down;
  }
  auto down = [&](DrumInput input) { return held[size_t(input)]; };
  auto velocity = [&](DrumInput pad_input, DrumInput cymbal_input) -> int16_t {
    uint8_t v = 0;
    if (down(pad_input)) v = d->inputs[size_t(pad_input)].velocity;
    if (down(cymbal_input)) v = std::max(v, d->inputs[size_t(cymbal_input)].velocity);
    return int16_t(std::min(32767, v * 258));
  };
  // RB3 pro kit: pad = colour + RS click, cymbal = colour + RB (+ d-pad up
  // yellow, down blue), kick = LB. Velocity: red LX, yellow LY, blue RX, green RY.
  uint16_t b = 0;
  if (down(DrumInput::kRedPad)) b |= X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (down(DrumInput::kYellowPad)) b |= X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (down(DrumInput::kBluePad)) b |= X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (down(DrumInput::kGreenPad)) b |= X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (down(DrumInput::kYellowCymbal)) b |= X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_DPAD_UP;
  if (down(DrumInput::kBlueCymbal)) b |= X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_DPAD_DOWN;
  if (down(DrumInput::kGreenCymbal)) b |= X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (down(DrumInput::kKick)) b |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (down(DrumInput::kStart)) b |= X_INPUT_GAMEPAD_START;
  if (down(DrumInput::kBack)) b |= X_INPUT_GAMEPAD_BACK;
  if (down(DrumInput::kUp)) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (down(DrumInput::kDown)) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (down(DrumInput::kLeft)) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (down(DrumInput::kRight)) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  pad.buttons = b;
  pad.thumb_lx = velocity(DrumInput::kRedPad, DrumInput::kRedPad);
  pad.thumb_ly = velocity(DrumInput::kYellowPad, DrumInput::kYellowCymbal);
  pad.thumb_rx = velocity(DrumInput::kBluePad, DrumInput::kBlueCymbal);
  pad.thumb_ry = velocity(DrumInput::kGreenPad, DrumInput::kGreenCymbal);
  changed = b != d->last_buttons;
  d->last_buttons = b;
  return pad;
}

std::array<bool, kDrumInputCount> MidiInput::RecentlyHit(const std::string& key) {
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  std::array<bool, kDrumInputCount> hit{};
  if (DeviceState* d = i->Find(key)) {
    auto now = Clock::now();
    for (size_t n = 0; n < kDrumInputCount; ++n) {
      hit[n] = d->inputs[n].last_hit != Clock::time_point{} && now - d->inputs[n].last_hit < kRecentHit;
    }
  }
  return hit;
}

std::optional<MidiHit> MidiInput::LastHit(const std::string& key) {
  Impl* i = impl();
  std::lock_guard lock(i->mutex);
  DeviceState* d = i->Find(key);
  return d ? d->last_hit : std::nullopt;
}

}  // namespace rb3pc
