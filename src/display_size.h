// The internal resolution follows what the picture is shown on: the window's
// drawable area in windowed mode (so a title bar or taskbar isn't counted),
// the display in fullscreen (or a fixed level picked for fullscreen). It
// changes while the game runs, as the window is resized or fullscreen toggled.
//
// RB3 renders 1280x720; the SDK's resolution_scale multiplies that by a
// whole number. The level picked is the smallest that's at least as sharp as
// the 16:9 picture fitted into the output (so it's downsampled, never
// upscaled, up to 4x).

#pragma once

#include <cstdint>
#include <string>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace rb3pc {

// fullscreen_resolution: 0 = match the display, 1-4 = that multiple.
int FullscreenResolutionSetting();
void SetFullscreenResolutionSetting(int value);

// The multiple for a picture shown in a `width` x `height` pixel area.
int ScaleForOutput(uint32_t width, uint32_t height);

// False if there's no window yet.
bool OutputPixelSize(bool fullscreen, uint32_t& width, uint32_t& height);

// The multiple to render at, for the window's current state.
int WantedResolutionScale(bool fullscreen);

// Sets resolution_scale for the GPU about to start (not saved: it's worked
// out again every boot).
void ApplyResolutionScale(bool fullscreen);

// Once the game runs: asks the GPU to switch to the multiple wanted now, if
// it's a different one (from the game's next frame). Call when the window's
// size or fullscreen state, or the fullscreen setting, changes. UI thread.
void FollowResolutionScale(rex::system::IGraphicsSystem* graphics, bool fullscreen);

// False if a change needs a restart.
bool ResolutionScaleLive();

// The multiple rendered at (or being switched to).
int CurrentResolutionScale();

// "2x (2560x1440)".
std::string ScaleLabel(int scale);

// Windowed: if the window, title bar included, doesn't fit in its display's
// usable area (the taskbar or panels left out), shrinks it to the largest
// 16:9 size that does, and centres it there.
void FitWindowToUsableArea();

}  // namespace rb3pc
