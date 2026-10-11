// Guest-output screenshots (F12 by default).

#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace rex::ui {
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui

namespace rb3pc {

// Captures the game's last presented frame, without overlays, and writes it as
// a PNG to <dir>/rb3pc_<timestamp>.png. Returns the written path, or an empty
// path on failure.
std::filesystem::path SaveScreenshot(rex::ui::Presenter& presenter,
                                     const std::filesystem::path& dir);

// Registers the screenshot keybind (bind_screenshot, F12) and, when the
// screenshot_interval cvar is non-zero, takes a screenshot every that many
// seconds. Captures always run on the UI thread.
class ScreenshotService {
 public:
  ~ScreenshotService() { Stop(); }

  void Start(rex::ui::WindowedAppContext& app_context,
             std::function<rex::ui::Presenter*()> get_presenter, std::filesystem::path dir);
  void Stop();

 private:
  void Capture();

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::function<rex::ui::Presenter*()> get_presenter_;
  std::filesystem::path dir_;
  std::thread interval_thread_;
  std::mutex mutex_;
  std::condition_variable stop_cv_;
  bool stopping_ = false;
};

}  // namespace rb3pc
