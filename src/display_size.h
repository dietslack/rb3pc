// The internal resolution follows what the picture is shown on: the window's
// drawable area in windowed mode (so a title bar or taskbar isn't counted),
// the display in fullscreen (or a fixed level picked for fullscreen). It
// changes while the game runs, as the window is resized or fullscreen toggled.
//
// RB3 renders at the video mode it's told at boot (it draws a wider or
// narrower scene for other shapes: 4:3 to 32:9 and beyond); rb3pc tells it
// 720 lines as wide as the window or display (wide_view), so the picture
// fills it without bars. The SDK's resolution_scale multiplies that by a
// whole number: the smallest that's at least as sharp as the picture fitted
// into the output (so it's downsampled, never upscaled, up to 4x). The shape
// is set at boot; the multiple follows resizes live.

#pragma once

#include <cstdint>
#include <string>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

namespace rb3pc {

// The guest video mode: 720 lines, and RB3 draws its scene this wide.
struct GuestMode {
  uint32_t width = 1280, height = 720;
};
// The mode for the window's shape now (its display's when `fullscreen`):
// 1280x720 with wide_view off or before there's a window.
GuestMode GuestModeFor(bool fullscreen);
// The mode the game was started with (set by ApplyResolutionScale).
GuestMode CurrentGuestMode();
// "16:9", "21:9", "4:3", or "2.10:1".
std::string AspectLabel(GuestMode mode);

// Makes the game's cameras draw the current mode's shape (patches the
// widescreen entry of Rnd::YRatio's table). Once the executable is loaded,
// before it runs.
void ApplyWideView();

// fullscreen_resolution: 0 = match the display, 1-4 = that multiple.
int FullscreenResolutionSetting();
void SetFullscreenResolutionSetting(int value);

// The multiple for a picture shown in a `width` x `height` pixel area.
int ScaleForOutput(uint32_t width, uint32_t height);

// False if there's no window yet.
bool OutputPixelSize(bool fullscreen, uint32_t& width, uint32_t& height);

// The multiple to render at, for the window's current state.
int WantedResolutionScale(bool fullscreen);

// Sets the video mode and resolution_scale for the game and GPU about to
// start (not saved: they're worked out again every boot).
void ApplyResolutionScale(bool fullscreen);

// Once the game runs: asks the GPU to switch to the multiple wanted now, if
// it's a different one (from the game's next frame). Call when the window's
// size or fullscreen state, or the fullscreen setting, changes. UI thread.
void FollowResolutionScale(rex::system::IGraphicsSystem* graphics, bool fullscreen);

// False if a change needs a restart.
bool ResolutionScaleLive();

// The multiple rendered at (or being switched to).
int CurrentResolutionScale();

// "2x (2560x1440)", in the current mode.
std::string ScaleLabel(int scale);

// Windowed: shrinks (same shape) and centres the window if it doesn't fit the
// display's usable area.
void FitWindowToUsableArea();

}  // namespace rb3pc
