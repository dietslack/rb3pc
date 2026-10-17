// rb3pc - ReXGlue Recompiled Project

#pragma once

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>

#include "bindings_dialog.h"
#include "display_dialog.h"
#include "audio_options.h"
#include "frame_stats.h"
#include "shader_warmup.h"
#include "instrument_input.h"
#include "deluxe.h"
#include "exe.h"
#include "launcher.h"
#include "loading_screen.h"
#include "mouse_menus.h"
#include "rb3_ui.h"
#include "custom_songs.h"
#include "mouse_cursor.h"
#include "profile.h"
#include "screenshot.h"
#include "script_patches.h"
#include "shm_cleanup.h"
#include "title_icon.h"

class Rb3pcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Rb3pcApp>(new Rb3pcApp(ctx, "rb3pc",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    rb3pc::ApplyMouseKeyboardDefaults();
    config.input_factory = REX_INPUT_BACKEND(rb3pc::CreateInputSystem);
    // Loaded here so its cvars exist before the GPU defaults below are set.
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    }
    // Host RT path draws a red haze over much of the 3D scene; FSI is correct
    // and no slower.
    if (!rex::cvar::HasNonDefaultValue("render_target_path_vulkan")) {
      rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi");
    }
    // Async compile skips draws; RB3 composites the venue a frame later, so it
    // showed as navy clear color for 0.3-0.9 s per new shader. The stall
    // happens at submission end anyway.
    if (!rex::cvar::HasNonDefaultValue("async_shader_compilation")) {
      rex::cvar::SetFlagByName("async_shader_compilation", "false");
    }
    // Runtime-built character textures have fetch constant type 0; without
    // this the backend binds null and characters render black.
    if (!rex::cvar::HasNonDefaultValue("gpu_allow_invalid_fetch_constants")) {
      rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    }
    // RB3 opens custom song packages one at a time when it first adds them
    // to its song cache, each open completing asynchronously. The SDK waits
    // 100 ms before completing each (console-like latency), so a first scan
    // of 1,000 packages took 100 s just waiting. 1 ms keeps it asynchronous.
    if (!rex::cvar::HasNonDefaultValue("xam_deferred_overlapped_delay_ms")) {
      rex::cvar::SetFlagByName("xam_deferred_overlapped_delay_ms", "1");
    }
    // Character textures are GPU-composited then CPU-compressed into the same
    // memory each time; "some" reads back only once (stale characters), "full"
    // stalls. "auto" is patch 0007. Migrates the old saved "some".
    if (!rex::cvar::HasNonDefaultValue("readback_resolve") ||
        rex::cvar::GetFlagByName("readback_resolve") == "some") {
      rex::cvar::SetFlagByName("readback_resolve", "auto");
    }
  }
  void OnPostSetup() override {
    rb3pc::UnlinkOwnGuestMemory();
    SetGuestFrameStats(rb3pc::GuestFrameStats);  // F3 debug overlay
    rb3pc::ApplyProfileName(runtime()->kernel_state());
    rb3pc::RegisterCustomSongs(runtime()->kernel_state());
    rb3pc::ApplyScriptPatchChecksums();
    if constexpr (rb3pc::exe::kDeluxe) rb3pc::CheckDeluxeCode();

    // Title (save) icon for the window and a .desktop launcher.
    auto icon = rb3pc::TitleIconPng();
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    rb3pc::WriteDesktopLauncher(icon, exe_dir, exe_dir / "rb3pc");
    app_context().CallInUIThread([icon] { rb3pc::ApplyWindowIcon(icon); });
    screenshots_.Start(
        app_context(),
        [this]() -> rex::ui::Presenter* {
          auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
          return graphics ? graphics->presenter() : nullptr;
        },
        rex::filesystem::GetExecutableFolder() / "screenshots");
  }
  void OnConfigureFonts(ImFontAtlas* atlas) override { rb3pc::AddRb3UiFonts(atlas); }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    imgui_drawer_ = drawer;
    loading_screen_ = rb3pc::CreateLoadingScreen(
        drawer, [this] { return immediate_drawer(); }, game_data_root());
    // ReXApp titles the window "<app name> <SDK build stamp>" just before this.
    window()->SetTitle("Rock Band 3");

    rex::ui::RegisterBind("bind_controller_bindings", "F6", "Toggle controller bindings window",
                          [this, drawer] {
                            if (bindings_dialog_) {
                              bindings_dialog_.reset();
                            } else {
                              bindings_dialog_ = rb3pc::CreateBindingsDialog(drawer);
                            }
                          });

    mouse_cursor_ = rb3pc::CreateMouseCursorController(drawer, window());
    fps_overlay_ = rb3pc::CreateFpsOverlay(drawer);
    pointer_overlay_ = rb3pc::CreatePointerOverlay(
        drawer, [this] { return immediate_drawer(); }, game_data_root());
    rex::ui::RegisterBind("bind_fps_counter", "F5", "Toggle FPS counter",
                          [this] { rb3pc::ToggleFpsCounter(ConfigPath()); });
    rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle fullscreen",
                          [this] { SetFullscreen(!window()->IsFullscreen()); });
    rex::ui::RegisterBind("bind_display_settings", "F8", "Toggle display settings window",
                          [this, drawer] {
                            if (display_dialog_) {
                              display_dialog_.reset();
                            } else {
                              display_dialog_ = rb3pc::CreateDisplayDialog(
                                  drawer, {[this] { return window()->IsFullscreen(); },
                                           [this](bool on) { SetFullscreen(on); },
                                           ConfigPath()});
                            }
                          });
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override {
    screenshots_.Stop();
    bindings_dialog_.reset();
    display_dialog_.reset();
    fps_overlay_.reset();
    pointer_overlay_.reset();
    mouse_cursor_.reset();
    loading_screen_.reset();
    launcher_.reset();
  }
  // Launcher runs here: window exists, runtime/GPU not yet built. PLAY resumes.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    if (!rb3pc::ShouldShowLauncher() || !imgui_drawer_) return WithScriptPatches(defaults);
    rb3pc::LauncherHooks hooks;
    hooks.is_fullscreen = [this] { return window()->IsFullscreen(); };
    hooks.set_fullscreen = [this](bool on) { SetFullscreen(on); };
    hooks.play = [this, defaults, resume] {
      // Deferred: the launcher is mid-draw when this is called.
      app_context().CallInUIThreadDeferred([this, defaults, resume] {
        launcher_.reset();
        resume(WithScriptPatches(defaults));
      });
    };
    hooks.quit = [this] {
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
    };
    hooks.request_paint = [this] { window()->RequestPaint(); };
    hooks.config_path = ConfigPath();
    launcher_ = rb3pc::CreateLauncher(
        imgui_drawer_, [this] { return immediate_drawer(); }, defaults.game_data_root,
        std::move(hooks));
    return std::nullopt;
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    rb3pc::RemoveOrphanedGuestMemory();
    rb3pc::BindingStore::Get().SetDirectory(rex::filesystem::GetExecutableFolder() / "bindings");
    // Without --game_data_root, look for the game files from the executable's
    // folder upwards, so double-clicking rb3pc from the build tree works.
    if (paths.game_data_root.empty()) {
      paths.game_data_root = FindGameDataRoot();
    }
    rb3pc::RegisterShaderWarmup(paths.game_data_root);
  }

 private:
  rb3pc::ScreenshotService screenshots_;
  std::unique_ptr<rex::ui::ImGuiDialog> bindings_dialog_;
  std::unique_ptr<rex::ui::ImGuiDialog> display_dialog_;
  std::unique_ptr<rex::ui::ImGuiDialog> fps_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> pointer_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> mouse_cursor_;
  std::unique_ptr<rex::ui::ImGuiDialog> loading_screen_;
  std::unique_ptr<rex::ui::ImGuiDialog> launcher_;
  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;

  static std::filesystem::path ConfigPath() {
    return rex::filesystem::GetExecutableFolder() / "rb3pc.toml";
  }

  // The game runs from an overlay of its folder with some scripts patched
  // (see script_patches.h).
  static rex::PathConfig WithScriptPatches(rex::PathConfig paths) {
    // Last stop before the runtime (and its audio) starts, launcher or not.
    // Deluxe on in rb3pc, or off in rb3pc-dx: the other executable runs this
    // boot (src/deluxe.h). Only returns if it can't be started.
    if (rb3pc::NeedsOtherExecutable()) {
      REXLOG_ERROR("Deluxe: {}", rb3pc::RunOtherExecutable());
    }
    rb3pc::ApplyAudioDeviceBuffer();
    // rb3pc-dx: DX's default.xex and d:\gen\ patch ARK overlay the game folder.
    auto replacements = rb3pc::exe::kDeluxe ? rb3pc::DeluxeOverlayFiles()
                                            : std::map<std::string, std::filesystem::path>();
    auto overlay = rb3pc::PrepareScriptPatches(
        paths.game_data_root, rex::filesystem::GetUserFolder() / "rb3pc" / "overlay", replacements);
    if (overlay) paths.game_data_root = *overlay;
    if (rb3pc::exe::kDeluxe && overlay && !replacements.empty()) {
      // An input script drives startup itself, so it gets the usual screens.
      bool quick_start = rex::cvar::GetFlagByName("quick_start") == "true" &&
                         rex::cvar::GetFlagByName("debug_input_script").empty();
      rb3pc::ApplyDeluxeOptions(*overlay, rex::cvar::GetFlagByName("show_fps") == "true",
                                quick_start);
    }
    // Deluxe writes settings to game:; only allowed when that's the overlay.
    rex::cvar::SetFlagByName("allow_game_relative_writes",
                             !replacements.empty() && overlay ? "true" : "false");
    return paths;
  }

  void SetFullscreen(bool on) {
    window()->SetFullscreen(on);
    rex::cvar::SetFlagByName("fullscreen", on ? "true" : "false");
    rex::cvar::SaveConfig(ConfigPath());
  }

  static std::filesystem::path FindGameDataRoot() {
    static constexpr const char* kCandidates[] = {"game/xbox-retail", "game", "assets"};
    auto dir = rex::filesystem::GetExecutableFolder();
    while (true) {
      for (const char* candidate : kCandidates) {
        auto root = dir / candidate;
        if (std::filesystem::is_regular_file(root / "default.xex")) {
          return root;
        }
      }
      if (!dir.has_parent_path() || dir.parent_path() == dir) {
        return {};
      }
      dir = dir.parent_path();
    }
  }
};
