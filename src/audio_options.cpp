#include "audio_options.h"

#include <cstdlib>

#include <SDL3/SDL_hints.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(audio_device_frames, 0, "Audio",
                     "Audio device buffer in samples (0 = system default; 256 = 5.3 ms at "
                     "48 kHz). Smaller is less latency but may crackle. Read at startup");

namespace rb3pc {

namespace {

int GetInt(const char* name) { return std::atoi(rex::cvar::GetFlagByName(name).c_str()); }

}  // namespace

int AudioQueueIndex() {
  int frames = GetInt("audio_maxqframes");
  for (int i = 0; i < kAudioQueueCount; ++i) {
    if (kAudioQueueFrames[i] == frames) return i;
  }
  return 2;
}

void SetAudioQueueIndex(int index) {
  rex::cvar::SetFlagByName("audio_maxqframes", std::to_string(kAudioQueueFrames[index]));
}

int AudioDeviceIndex() {
  int frames = REXCVAR_GET(audio_device_frames);
  for (int i = 0; i < kAudioDeviceCount; ++i) {
    if (kAudioDeviceFrames[i] == frames) return i;
  }
  return 0;
}

void SetAudioDeviceIndex(int index) {
  rex::cvar::SetFlagByName("audio_device_frames", std::to_string(kAudioDeviceFrames[index]));
}

void ApplyAudioDeviceBuffer() {
  int frames = REXCVAR_GET(audio_device_frames);
  if (frames <= 0) return;
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(frames).c_str());
  REXLOG_INFO("Audio: requesting a {}-sample device buffer", frames);
}

}  // namespace rb3pc
