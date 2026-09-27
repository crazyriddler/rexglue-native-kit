// User settings: conan.cfg next to the executable (INI-style, replaces the
// old Xenia-style conan.toml). Edited by the launcher (launcher_dialog.cpp)
// or by hand; applied to the runtime/renderer cvars before the window, GPU
// and game start.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace conan {

struct Settings {
  // [Display] (defaults = the original game)
  bool fullscreen = true;
  int window_width = 1280;
  int window_height = 720;
  bool vsync = true;
  int fps_limit = 30;  // 30 (original) .. 120
  // [Graphics]
  // Internal render height (16:9, 720 = original .. 2880), 0 = the display's
  // native height.
  int render_resolution = 720;
  int shadow_quality = 1;     // shadow map multiple of 1024: 1 (original), 2, 4
  int anisotropic = 0;        // 0 = game, 2, 4, 8, 16
  int msaa = 4;               // 1 = off, 4 (original), 8
  // [Enhancements] (EXP-045; all off = the original look). The full-resolution
  // 3D scene and smooth effects above 720p are fixes, always on (EXP-048).
  bool foliage_antialiasing = false;
  bool fxaa = false;
  bool ambient_occlusion = false;
  int bloom_quality = 1;  // 1 = original, 2, 4
  bool dithering = false;
  int shadow_smoothing = 0;       // 0 = off, 1 = on, 2 = softer
  bool soft_particles = false;
  // [Game]
  std::string language = "auto";  // auto (Windows language), en, es, fr, de, it
  // [Launcher]
  bool show_launcher = true;

  // Missing file or keys keep the defaults above. Returns false if the file
  // does not exist.
  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;
  // Pushes the settings into the cvars (flags given on the command line win).
  void Apply() const;
  // Clamps every value to the supported set.
  void Sanitize();
  // Render height with "native" (0) resolved to the primary display height.
  int ResolvedRenderHeight() const;
  // `language` with "auto" resolved to the Windows language.
  std::string ResolvedLanguage() const;
};

// Startup: loads conan.cfg, shows the launcher (first run, ShowAtStartup,
// or SHIFT held; never with --show_launcher=false), saves, applies. Exits
// the process when the launcher is closed without pressing Play.
void LoadSettingsAndRunLauncher(const std::filesystem::path& path);

// Xbox 360 XLanguage id for a language code (English when unknown).
uint32_t LanguageId(const std::string& code);
// Language code for the Windows UI language (supported game languages only).
std::string DefaultLanguage();

}  // namespace conan
