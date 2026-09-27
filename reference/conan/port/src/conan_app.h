// conan - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_notification.h>

#include "launcher_dialog.h"
#include "native/native_graphics_system.h"
#include "settings.h"

class ConanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ConanApp>(new ConanApp(ctx, "conan",
        PPCImageConfig));
  }

  // E041's investigation-era hooks are gone: the two guest-memory
  // write-watches it armed here (on the light state cursor's page and on the
  // character object's own page) worked by write-protecting those physical
  // pages, so every guest store into them took an access violation through
  // the exception handler and back - on the character object, which the game
  // writes constantly, that alone cost more than everything else in this
  // file combined. The state-cursor seed is gone with them: the real fix for
  // E041 turned out to be GPU-side NaN sanitization (see error_log.md), and
  // the seed only pushed the guest down an allocate-and-dispatch path it
  // otherwise never takes, paying for per-frame work that changed nothing.
  //
  // No SDK overlays in this port: the debug/console/settings/achievements
  // hotkeys (F3, Backtick, F4, F7) are removed, and so is the achievement toast.
  // (An always-registered ImGui dialog also kept the UI thread repainting.)
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    for (const char* bind : {"bind_debug_overlay", "bind_console", "bind_settings",
                             "bind_achievements"}) {
      rex::ui::UnregisterBind(bind);
    }
  }
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    return nullptr;
  }

  // Release window title: no SDK build stamp.
  std::string GetWindowTitle() const override { return "Conan"; }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

  // `--game_data_root` has no SDK-level default (rex/system/runtime.cpp),
  // so conan.exe refuses to start without it being passed explicitly every
  // time (see docs/error_log.md). This project's own game/ directory is
  // always in a fixed place relative to either the current working
  // directory (when launched the way this project's docs say to, from the
  // conan-port root) or the exe's own build output folder (when launched
  // directly, e.g. by double-clicking conan.exe), so auto-discover it
  // instead of requiring the flag - --game_data_root still overrides this
  // when explicitly passed (paths.game_data_root is only empty here if it
  // wasn't).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // User settings live in conan.cfg (conan::Settings, edited by the
    // launcher). The SDK's TOML config is only an optional advanced override
    // file now; the old conan.toml is no longer read.
    auto exe_folder = rex::filesystem::GetExecutableFolder();
    settings_path_ = exe_folder / "conan.cfg";
    paths.config_path = exe_folder / "conan_advanced.toml";

    // Packaged Release builds shouldn't spend CPU time formatting/writing
    // log lines or fill the disk with rotated log files during normal
    // play - this bit us directly: a leftover per-register-write debug
    // log left in the SDK (since removed, see error_log.md E041) was
    // producing multiple 5MB rotated log files within minutes and a real
    // FPS hit, entirely because logging defaults to "info" whether or not
    // anyone's watching. Force it off for Release specifically, and only
    // when nothing already asked for logging explicitly (a real
    // `--log_level=...` on the command line, checked here before this
    // project's own config.toml has even loaded, still wins - this is for
    // someone who wants to debug a Release build without rebuilding).
    // Debug/RelWithDebInfo builds are for development and keep the SDK's
    // own "info" default.
    if (!rex::cvar::HasNonDefaultValue("log_level")) {
      constexpr std::string_view kBuildConfig = REXGLUE_BUILD_CONFIG;
      if (kBuildConfig == "Release") {
        rex::cvar::SetFlagByName("log_level", "off");
      }
    }

    if (!paths.game_data_root.empty()) return;
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    std::filesystem::path candidates[] = {
        exe_dir / "data",  // portable release layout (tools/make_release.sh)
        std::filesystem::current_path() / "game",
        exe_dir / "game",
        exe_dir / ".." / ".." / ".." / "game",  // out/build/<preset>/conan.exe -> conan-port/game
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.game_data_root = std::filesystem::absolute(candidate, ec);
        return;
      }
    }
  }

  // Runs after the (optional) TOML config is loaded but before the
  // window/GPU/ImGui are created (SetupPresentation creates those right
  // after calling this) - the point where conan.cfg and the launcher set the
  // display/graphics/language cvars, since they are read at creation. See
  // launcher_dialog.h for why this is a native Win32 window rather than an
  // SDK ImGuiDialog (the latter crashed this early in startup).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Same problem as game_data_root above: `--gpu_plugin` has no
    // SDK-level default, so a plain double-click launch (no flags at all,
    // matching how this project's packaged builds are meant to be run)
    // left config.gpu_plugin empty - LoadGpuPlugin() was then never
    // called, so the game ran with no GPU emulation at all: it doesn't
    // error out, it just hangs on a black screen forever waiting on a GPU
    // that was never there (confirmed via the runtime's own watchdog log:
    // "VdInitializeRingBuffer: no GPU emulation loaded (gpu_plugin not
    // set); call ignored"). This project only ships the xenos plugin, so
    // default to it - `--gpu_plugin` still overrides this when passed.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // Settings: conan.cfg -> launcher (first run, ShowAtStartup or SHIFT
    // held) -> cvars. Runs before the window, the GPU and the game exist, so
    // every setting applies at creation.
    conan::LoadSettingsAndRunLauncher(settings_path_);
    // Native renderer: no Xenos GPU plugin at all. The native graphics system
    // provides the D3D12 presenter and the guest GPU synchronization contract
    // (EXP-036); rendering comes from conan::native::Renderer. The A/B mode
    // still loads Xenos as its reference.
    if (conan::native::UseNativeGraphicsSystem()) {
      config.graphics = std::make_unique<conan::native::NativeGraphicsSystem>();
    }
  }

 private:
  std::filesystem::path settings_path_;
};
