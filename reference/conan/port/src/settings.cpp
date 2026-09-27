#include "settings.h"

#include "launcher_dialog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(write_default_settings, false, "Launcher",
                    "Write a default conan.cfg next to the executable and exit (packaging)");
REXCVAR_DEFINE_BOOL(show_launcher, true, "Launcher",
                    "Show the settings launcher at startup (per conan.cfg ShowAtStartup)");

namespace conan {

namespace {

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n\"");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n\"");
  return s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool ParseBool(const std::string& v, bool fallback) {
  std::string l = Lower(v);
  if (l == "1" || l == "true" || l == "yes" || l == "on") return true;
  if (l == "0" || l == "false" || l == "no" || l == "off") return false;
  return fallback;
}

int ParseInt(const std::string& v, int fallback) {
  if (v.empty()) return fallback;
  char* end = nullptr;
  long n = std::strtol(v.c_str(), &end, 10);
  return end && *end == '\0' ? int(n) : fallback;
}

// Nearest allowed value.
int Snap(int v, std::initializer_list<int> allowed) {
  int best = *allowed.begin();
  for (int a : allowed) {
    if (std::abs(a - v) < std::abs(best - v)) best = a;
  }
  return best;
}

struct LanguageEntry {
  const char* code;
  uint32_t xlanguage;  // rex::system::XLanguage
};
constexpr LanguageEntry kLanguages[] = {
    {"en", 1}, {"es", 5}, {"fr", 4}, {"de", 3}, {"it", 6},
};

void SetCvar(const char* name, const std::string& value) {
  // Explicit command-line flags (benchmarks, debugging) win over conan.cfg.
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine) return;
  if (!rex::cvar::SetFlagByName(name, value)) {
    REXLOG_WARN("settings: could not set {} = {}", name, value);
  }
}

}  // namespace

int Settings::ResolvedRenderHeight() const {
  if (render_resolution != 0) return render_resolution;
  // Physical mode of the primary display (not DPI-virtualized metrics).
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  int height = 720;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsHeight) {
    // A 16:9 image fitted to the display: wider-than-16:9 displays are limited
    // by height, narrower ones (16:10) by width.
    height = int(std::min<double>(mode.dmPelsHeight, mode.dmPelsWidth * 9.0 / 16.0));
  }
  return std::clamp(height, 720, 2880);
}

std::string Settings::ResolvedLanguage() const {
  return language == "auto" ? DefaultLanguage() : language;
}

uint32_t LanguageId(const std::string& code) {
  for (const LanguageEntry& l : kLanguages) {
    if (code == l.code) return l.xlanguage;
  }
  return 1;
}

std::string DefaultLanguage() {
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_SPANISH:
      return "es";
    case LANG_FRENCH:
      return "fr";
    case LANG_GERMAN:
      return "de";
    case LANG_ITALIAN:
      return "it";
    default:
      return "en";
  }
}

void Settings::Sanitize() {
  window_width = std::clamp(window_width, 640, 7680);
  window_height = std::clamp(window_height, 360, 4320);
  fps_limit = std::clamp(fps_limit, 30, 120);
  // Before arbitrary resolutions the value was a 720p multiple (1-4).
  if (render_resolution >= 1 && render_resolution <= 4) render_resolution *= 720;
  if (render_resolution != 0) render_resolution = std::clamp(render_resolution, 720, 2880);
  // 3 was "Ultra (3072)" before EXP-042: keep it Ultra.
  shadow_quality = shadow_quality >= 3 ? 4 : std::clamp(shadow_quality, 1, 2);
  // 1 ("off") was removed: same as the game default.
  anisotropic = anisotropic <= 1 ? 0 : Snap(anisotropic, {2, 4, 8, 16});
  // Before EXP-048: 0 = the game default (4x), 2 = 2x.
  msaa = msaa == 1 ? 1 : msaa >= 8 ? 8 : 4;
  bloom_quality = Snap(bloom_quality, {1, 2, 4});
  shadow_smoothing = std::clamp(shadow_smoothing, 0, 2);
  bool known = language == "auto";
  for (const LanguageEntry& l : kLanguages) known |= language == l.code;
  if (!known) language = "auto";
}

bool Settings::Load(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    Sanitize();
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    size_t comment = line.find_first_of(";#");
    if (comment != std::string::npos) line.resize(comment);
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;  // blank line or [Section]
    std::string key = Lower(Trim(line.substr(0, eq)));
    std::string value = Trim(line.substr(eq + 1));
    if (key == "fullscreen") fullscreen = ParseBool(value, fullscreen);
    else if (key == "windowwidth") window_width = ParseInt(value, window_width);
    else if (key == "windowheight") window_height = ParseInt(value, window_height);
    else if (key == "vsync") vsync = ParseBool(value, vsync);
    else if (key == "frameratelimit") fps_limit = ParseInt(value, fps_limit);
    else if (key == "renderresolution") render_resolution = ParseInt(value, render_resolution);
    else if (key == "shadowquality") shadow_quality = ParseInt(value, shadow_quality);
    else if (key == "anisotropicfiltering") anisotropic = ParseInt(value, anisotropic);
    else if (key == "language") language = Lower(value);
    else if (key == "msaa") msaa = ParseInt(value, msaa);
    else if (key == "foliageantialiasing") foliage_antialiasing = ParseBool(value, foliage_antialiasing);
    else if (key == "fxaa") fxaa = ParseBool(value, fxaa);
    else if (key == "ambientocclusion") ambient_occlusion = ParseBool(value, ambient_occlusion);
    else if (key == "bloomquality") bloom_quality = ParseInt(value, bloom_quality);
    else if (key == "dithering") dithering = ParseBool(value, dithering);
    else if (key == "shadowsmoothing") shadow_smoothing = ParseInt(value, shadow_smoothing);
    else if (key == "softparticles") soft_particles = ParseBool(value, soft_particles);
    else if (key == "showatstartup") show_launcher = ParseBool(value, show_launcher);
  }
  Sanitize();
  return true;
}

bool Settings::Save(const std::filesystem::path& path) const {
  std::ostringstream o;
  o << "; Conan - settings\n"
       "; Edited by the launcher shown at startup (hold SHIFT while starting the game to\n"
       "; open it when ShowAtStartup = 0), or by hand. Changes apply on the next start.\n"
       "\n"
       "[Display]\n"
       "; 1 = fullscreen (borderless, desktop resolution), 0 = window\n"
       "Fullscreen = "
    << (fullscreen ? 1 : 0)
    << "\n"
       "; Window size when Fullscreen = 0\n"
       "WindowWidth = "
    << window_width << "\nWindowHeight = " << window_height
    << "\n"
       "; 1 = wait for the monitor refresh (no tearing), 0 = present immediately\n"
       "VSync = "
    << (vsync ? 1 : 0)
    << "\n"
       "; Frame rate limit: 30 (original) to 120\n"
       "FrameRateLimit = "
    << fps_limit
    << "\n\n"
       "[Graphics]\n"
       "; Internal render height (16:9): 720 (original) to 2880, e.g. 900, 1080, 1440, 2160;\n"
       "; 0 = the display's native height\n"
       "RenderResolution = "
    << render_resolution
    << "\n"
       "; Shadow map resolution: 1 = 1024 (original), 2 = 2048, 4 = 4096\n"
       "ShadowQuality = "
    << shadow_quality
    << "\n"
       "; Anisotropic texture filtering: 0 = game default, 2, 4, 8, 16\n"
       "AnisotropicFiltering = "
    << anisotropic
    << "\n"
       "; Antialiasing (MSAA) of the 3D scene: 1 = off, 4 = 4x (original), 8 = 8x\n"
       "MSAA = "
    << msaa
    << "\n\n"
       "[Enhancements]\n"
       "; 1 = on, 0 = off (all off = the original look)\n"
       "; Antialiased alpha-tested foliage (alpha to coverage; needs MSAA)\n"
       "FoliageAntialiasing = "
    << (foliage_antialiasing ? 1 : 0)
    << "\n"
       "; FXAA post-process antialiasing of the 3D image (the HUD stays sharp)\n"
       "FXAA = "
    << (fxaa ? 1 : 0)
    << "\n"
       "; Screen-space ambient occlusion\n"
       "AmbientOcclusion = "
    << (ambient_occlusion ? 1 : 0)
    << "\n"
       "; Bloom resolution: 1 = original, 2, 4\n"
       "BloomQuality = "
    << bloom_quality
    << "\n"
       "; Output dithering (less color banding)\n"
       "Dithering = "
    << (dithering ? 1 : 0)
    << "\n"
       "; Shadow edge smoothing: 0 = off (original), 1 = on, 2 = softer\n"
       "ShadowSmoothing = "
    << shadow_smoothing
    << "\n"
       "; Soft particles (effects fade near the geometry behind them)\n"
       "SoftParticles = "
    << (soft_particles ? 1 : 0)
    << "\n\n"
       "[Game]\n"
       "; Voice and text language: auto (Windows language), en, es, fr, de, it\n"
       "Language = "
    << language
    << "\n\n"
       "[Launcher]\n"
       "ShowAtStartup = "
    << (show_launcher ? 1 : 0) << "\n";
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << o.str();
  return bool(out);
}

void Settings::Apply() const {
  SetCvar("fullscreen", fullscreen ? "true" : "false");
  SetCvar("window_width", std::to_string(window_width));
  SetCvar("window_height", std::to_string(window_height));
  SetCvar("vsync", vsync ? "true" : "false");
  SetCvar("fps_limit", std::to_string(fps_limit));
  SetCvar("render_scale", std::to_string(double(ResolvedRenderHeight()) / 720.0));
  SetCvar("shadow_quality", std::to_string(shadow_quality));
  SetCvar("anisotropic_filtering", std::to_string(anisotropic == 0 ? -1 : anisotropic));
  // msaa_samples: 0 = the game's own 4x.
  SetCvar("msaa_samples", std::to_string(msaa == 4 ? 0 : msaa));
  SetCvar("full_scene_resolution", "true");
  SetCvar("foliage_antialiasing", foliage_antialiasing && msaa > 1 ? "true" : "false");
  SetCvar("fxaa", fxaa ? "true" : "false");
  SetCvar("ambient_occlusion", ambient_occlusion ? "true" : "false");
  SetCvar("bloom_quality", std::to_string(bloom_quality));
  SetCvar("present_dither", dithering ? "true" : "false");
  SetCvar("shadow_smoothing", std::to_string(shadow_smoothing));
  SetCvar("soft_particles", soft_particles ? "true" : "false");
  SetCvar("smooth_effects", "true");
  SetCvar("user_language", std::to_string(LanguageId(ResolvedLanguage())));
}

void LoadSettingsAndRunLauncher(const std::filesystem::path& path) {
  Settings settings;
  if (REXCVAR_GET(write_default_settings)) {
    // Packaging (tools/make_release.sh): write the default conan.cfg and quit.
    settings.Sanitize();
    std::exit(settings.Save(path) ? 0 : 1);
  }
  bool exists = settings.Load(path);
  bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  if (REXCVAR_GET(show_launcher) && (!exists || settings.show_launcher || shift)) {
    if (!ShowConanLauncher(settings)) {
      std::exit(0);  // Exit pressed / launcher closed
    }
    settings.Save(path);
  } else if (!exists) {
    settings.Save(path);
  }
  settings.Apply();
  REXLOG_INFO("settings: {} fullscreen {} window {}x{} vsync {} fps {} render height {} shadows x{} aniso {} "
              "language {}",
              path.string(), settings.fullscreen, settings.window_width, settings.window_height,
              settings.vsync, settings.fps_limit, settings.ResolvedRenderHeight(),
              settings.shadow_quality, settings.anisotropic, settings.language);
}

}  // namespace conan
