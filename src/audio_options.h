// Audio buffer cvars (launcher rows), both read when audio starts.
// audio_maxqframes is the SDK's queue of 256-sample XAudio frames (5.3 ms at
// 48 kHz). audio_device_frames is the SDL device buffer, 0 for the system
// default (1024 samples on PipeWire, 21 ms).

#pragma once

#include <string>

namespace rb3pc {

constexpr int kAudioQueueFrames[] = {4, 6, 8, 12, 16};
constexpr const char* kAudioQueueLabels[] = {"21 ms (4 frames)", "32 ms (6 frames)",
                                             "43 ms (8 frames, default)", "64 ms (12 frames)",
                                             "85 ms (16 frames)"};
constexpr int kAudioQueueCount = 5;

constexpr int kAudioDeviceFrames[] = {0, 128, 256, 512, 1024, 2048};
constexpr const char* kAudioDeviceLabels[] = {"System default", "128 samples (2.7 ms)",
                                              "256 samples (5.3 ms)", "512 samples (11 ms)",
                                              "1024 samples (21 ms)", "2048 samples (43 ms)"};
constexpr int kAudioDeviceCount = 6;

int AudioQueueIndex();
void SetAudioQueueIndex(int index);
int AudioDeviceIndex();
void SetAudioDeviceIndex(int index);

// Asks SDL for audio_device_frames; call before the runtime starts audio.
void ApplyAudioDeviceBuffer();

}  // namespace rb3pc
