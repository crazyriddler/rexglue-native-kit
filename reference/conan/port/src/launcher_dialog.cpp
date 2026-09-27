#include "launcher_dialog.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <rex/cvar.h>

// Visual styles (themed buttons/combos) for the launcher's common controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

REXCVAR_DEFINE_STRING(launcher_ui_language, "", "Launcher",
                      "Launcher UI language (en/es/fr/de/it; empty = Windows language)");

namespace {

// A UI string in the game's languages. The launcher follows the Windows UI
// language when the game supports it (English otherwise); the game language
// itself is a separate setting.
struct Tr {
  const wchar_t* en;
  const wchar_t* es;
  const wchar_t* fr;
  const wchar_t* de;
  const wchar_t* it;
};
enum UiLanguage { kEn, kEs, kFr, kDe, kIt };

UiLanguage DetectUiLanguage() {
  const std::string& forced = REXCVAR_GET(launcher_ui_language);
  if (forced == "es") return kEs;
  if (forced == "fr") return kFr;
  if (forced == "de") return kDe;
  if (forced == "it") return kIt;
  if (forced == "en") return kEn;
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_SPANISH:
      return kEs;
    case LANG_FRENCH:
      return kFr;
    case LANG_GERMAN:
      return kDe;
    case LANG_ITALIAN:
      return kIt;
    default:
      return kEn;
  }
}

struct Text {
  UiLanguage language = kEn;
  const wchar_t* operator()(const Tr& s) const {
    switch (language) {
      case kEs:
        return s.es;
      case kFr:
        return s.fr;
      case kDe:
        return s.de;
      case kIt:
        return s.it;
      default:
        return s.en;
    }
  }
};

struct Choice {
  int value;
  Tr name;
};

const std::array<Choice, 2> kDisplayModes = {{
    {1, {L"Fullscreen", L"Pantalla completa", L"Plein \u00e9cran", L"Vollbild", L"Schermo intero"}},
    {0, {L"Window", L"Ventana", L"Fen\u00eatre", L"Fenster", L"Finestra"}},
}};
const std::array<Choice, 4> kFrameRates = {{
    {30, {L"30 FPS (original)", L"30 FPS (original)", L"30 FPS (d'origine)", L"30 FPS (Original)",
          L"30 FPS (originale)"}},
    {60, {L"60 FPS", L"60 FPS", L"60 FPS", L"60 FPS", L"60 FPS"}},
    {90, {L"90 FPS", L"90 FPS", L"90 FPS", L"90 FPS", L"90 FPS"}},
    {120, {L"120 FPS", L"120 FPS", L"120 FPS", L"120 FPS", L"120 FPS"}},
}};
const std::array<Choice, 8> kResolutions = {{
    {0, {L"Native display", L"Nativa de la pantalla", L"Native de l'\u00e9cran",
         L"Native Bildschirmaufl\u00f6sung", L"Nativa dello schermo"}},
    {720, {L"720p (original)", L"720p (original)", L"720p (d'origine)", L"720p (Original)",
           L"720p (originale)"}},
    {900, {L"900p", L"900p", L"900p", L"900p", L"900p"}},
    {1080, {L"1080p", L"1080p", L"1080p", L"1080p", L"1080p"}},
    {1440, {L"1440p", L"1440p", L"1440p", L"1440p", L"1440p"}},
    {1800, {L"1800p", L"1800p", L"1800p", L"1800p", L"1800p"}},
    {2160, {L"2160p / 4K", L"2160p / 4K", L"2160p / 4K", L"2160p / 4K", L"2160p / 4K"}},
    {2880, {L"2880p", L"2880p", L"2880p", L"2880p", L"2880p"}},
}};
const std::array<Choice, 3> kShadowQualities = {{
    {1, {L"Original (1024)", L"Original (1024)", L"D'origine (1024)", L"Original (1024)",
         L"Originale (1024)"}},
    {2, {L"High (2048)", L"Alta (2048)", L"\u00c9lev\u00e9e (2048)", L"Hoch (2048)", L"Alta (2048)"}},
    {4, {L"Ultra (4096)", L"Ultra (4096)", L"Ultra (4096)", L"Ultra (4096)", L"Ultra (4096)"}},
}};
const std::array<Choice, 5> kAnisotropic = {{
    {0, {L"Game default", L"Por defecto del juego", L"Par d\u00e9faut du jeu", L"Spielstandard",
         L"Predefinito del gioco"}},
    {2, {L"2x", L"2x", L"2x", L"2x", L"2x"}},
    {4, {L"4x", L"4x", L"4x", L"4x", L"4x"}},
    {8, {L"8x", L"8x", L"8x", L"8x", L"8x"}},
    {16, {L"16x", L"16x", L"16x", L"16x", L"16x"}},
}};
const std::array<Choice, 3> kMsaa = {{
    {1, {L"Off", L"Desactivado", L"D\u00e9sactiv\u00e9", L"Aus", L"Disattivato"}},
    {4, {L"4x (original)", L"4x (original)", L"4x (d'origine)", L"4x (Original)",
         L"4x (originale)"}},
    {8, {L"8x", L"8x", L"8x", L"8x", L"8x"}},
}};
const std::array<Choice, 3> kBloomQualities = {{
    {1, {L"Original", L"Original", L"D'origine", L"Original", L"Originale"}},
    {2, {L"High", L"Alta", L"\u00c9lev\u00e9e", L"Hoch", L"Alta"}},
    {4, {L"Very high", L"Muy alta", L"Tr\u00e8s \u00e9lev\u00e9e", L"Sehr hoch", L"Molto alta"}},
}};
const std::array<Choice, 3> kShadowSmoothing = {{
    {0, {L"Off (original)", L"Desactivado (original)", L"D\u00e9sactiv\u00e9 (d'origine)",
         L"Aus (Original)", L"Disattivato (originale)"}},
    {1, {L"On", L"Activado", L"Activ\u00e9", L"An", L"Attivato"}},
    {2, {L"Softer", L"M\u00e1s suave", L"Plus doux", L"Weicher", L"Pi\u00f9 morbido"}},
}};
struct LanguageChoice {
  const char* code;
  const wchar_t* name;
};
const std::array<LanguageChoice, 5> kLanguages = {{
    {"en", L"English"},
    {"es", L"Español"},
    {"fr", L"Français"},
    {"de", L"Deutsch"},
    {"it", L"Italiano"},
}};
struct WindowSize {
  int w, h;
};
const std::array<WindowSize, 6> kWindowSizes = {{
    {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},
}};

enum ControlId : int {
  kPlay = IDOK,
  kExit = IDCANCEL,
  kDefaults = 100,
  kDisplayMode,
  kWindowSize,
  kFrameRate,
  kVsync,
  kResolution,
  kShadows,
  kAnisotropicCombo,
  kMsaaCombo,
  kLanguage,
  kShowAtStartup,
  kBloom,
  kFoliageAa,
  kFxaa,
  kSsao,
  kDithering,
  kShadowSmoothingCombo,
  kSoftParticles,
};

struct LauncherState {
  conan::Settings* settings = nullptr;
  Text t;
  int dpi = 96;
  HFONT font = nullptr, bold_font = nullptr;
  HWND display_mode = nullptr, window_size = nullptr, frame_rate = nullptr, vsync = nullptr,
       resolution = nullptr, shadows = nullptr, anisotropic = nullptr, msaa = nullptr,
       bloom = nullptr, foliage_aa = nullptr, fxaa = nullptr, ssao = nullptr,
       dithering = nullptr, shadow_smoothing = nullptr, soft_particles = nullptr,
       language = nullptr, show_at_startup = nullptr;
  std::vector<WindowSize> sizes;  // window size combo entries
  bool play = false;
  // Guards against a stray key/click queued before the window appeared
  // (scripted launches) confirming it instantly.
  DWORD shown_tick = 0;
};

int Scale(const LauncherState* s, int v) { return MulDiv(v, s->dpi, 96); }

HFONT MakeFont(int dpi, bool bold) {
  LOGFONTW lf{};
  lf.lfHeight = -MulDiv(9, dpi, 72);
  lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(lf.lfFaceName, L"Segoe UI");
  return CreateFontIndirectW(&lf);
}

HWND MakeControl(LauncherState* s, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, int id = 0, bool bold = false) {
  HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, Scale(s, x), Scale(s, y),
                              Scale(s, w), Scale(s, h), parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? s->bold_font : s->font), TRUE);
  return hwnd;
}

template <size_t N>
HWND MakeChoiceCombo(LauncherState* s, HWND parent, int x, int y, int w, int id,
                     const std::array<Choice, N>& choices, int value) {
  HWND combo = MakeControl(s, parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           x, y, w, 300, id);
  int selected = 0;
  for (size_t i = 0; i < N; ++i) {
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(s->t(choices[i].name)));
    if (choices[i].value == value) selected = int(i);
  }
  SendMessageW(combo, CB_SETCURSEL, selected, 0);
  return combo;
}

template <size_t N>
int ChoiceValue(HWND combo, const std::array<Choice, N>& choices, int fallback) {
  LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  return i >= 0 && size_t(i) < N ? choices[size_t(i)].value : fallback;
}

void SelectWindowSize(LauncherState* s, int w, int h) {
  SendMessageW(s->window_size, CB_RESETCONTENT, 0, 0);
  s->sizes.assign(kWindowSizes.begin(), kWindowSizes.end());
  if (std::none_of(s->sizes.begin(), s->sizes.end(),
                   [&](const WindowSize& z) { return z.w == w && z.h == h; })) {
    s->sizes.push_back({w, h});  // custom size from conan.cfg
  }
  int selected = 0;
  for (size_t i = 0; i < s->sizes.size(); ++i) {
    std::wstring label = std::to_wstring(s->sizes[i].w) + L" x " + std::to_wstring(s->sizes[i].h);
    SendMessageW(s->window_size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (s->sizes[i].w == w && s->sizes[i].h == h) selected = int(i);
  }
  SendMessageW(s->window_size, CB_SETCURSEL, selected, 0);
}

void UpdateEnabled(LauncherState* s) {
  bool windowed = ChoiceValue(s->display_mode, kDisplayModes, 1) == 0;
  EnableWindow(s->window_size, windowed);
  // Foliage antialiasing is alpha to coverage: it needs MSAA.
  bool msaa = ChoiceValue(s->msaa, kMsaa, 4) > 1;
  if (!msaa) SendMessageW(s->foliage_aa, BM_SETCHECK, BST_UNCHECKED, 0);
  EnableWindow(s->foliage_aa, msaa);
}

void Populate(LauncherState* s) {
  const conan::Settings& c = *s->settings;
  auto select = [](HWND combo, int index) { SendMessageW(combo, CB_SETCURSEL, index, 0); };
  auto index_of = [](const auto& choices, int value) {
    for (size_t i = 0; i < choices.size(); ++i)
      if (choices[i].value == value) return int(i);
    return 0;
  };
  select(s->display_mode, index_of(kDisplayModes, c.fullscreen ? 1 : 0));
  SelectWindowSize(s, c.window_width, c.window_height);
  // Frame rate: nearest listed value.
  int fr = 0;
  for (size_t i = 0; i < kFrameRates.size(); ++i)
    if (std::abs(kFrameRates[i].value - c.fps_limit) <
        std::abs(kFrameRates[size_t(fr)].value - c.fps_limit))
      fr = int(i);
  select(s->frame_rate, fr);
  SendMessageW(s->vsync, BM_SETCHECK, c.vsync ? BST_CHECKED : BST_UNCHECKED, 0);
  {
    // A height typed into conan.cfg that is not in the list stays selectable.
    bool listed = false;
    for (const Choice& r : kResolutions) listed |= r.value == c.render_resolution;
    if (listed) {
      select(s->resolution, index_of(kResolutions, c.render_resolution));
    } else {
      std::wstring custom = std::to_wstring(c.render_resolution) + L"p";
      LRESULT i = SendMessageW(s->resolution, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(custom.c_str()));
      select(s->resolution, int(i));
    }
  }
  select(s->shadows, index_of(kShadowQualities, c.shadow_quality));
  select(s->anisotropic, index_of(kAnisotropic, c.anisotropic));
  select(s->msaa, index_of(kMsaa, c.msaa));
  select(s->bloom, index_of(kBloomQualities, c.bloom_quality));
  auto check = [](HWND box, bool on) {
    SendMessageW(box, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  };
  check(s->foliage_aa, c.foliage_antialiasing);
  check(s->fxaa, c.fxaa);
  check(s->ssao, c.ambient_occlusion);
  check(s->dithering, c.dithering);
  select(s->shadow_smoothing, index_of(kShadowSmoothing, c.shadow_smoothing));
  check(s->soft_particles, c.soft_particles);
  int lang = 0;
  for (size_t i = 0; i < kLanguages.size(); ++i)
    if (c.ResolvedLanguage() == kLanguages[i].code) lang = int(i);
  select(s->language, lang);
  SendMessageW(s->show_at_startup, BM_SETCHECK, c.show_launcher ? BST_CHECKED : BST_UNCHECKED, 0);
  UpdateEnabled(s);
}

// Reads the controls into the settings (before DestroyWindow, which also
// destroys the child controls).
void Capture(LauncherState* s) {
  conan::Settings& c = *s->settings;
  c.fullscreen = ChoiceValue(s->display_mode, kDisplayModes, 1) != 0;
  LRESULT size = SendMessageW(s->window_size, CB_GETCURSEL, 0, 0);
  if (size >= 0 && size_t(size) < s->sizes.size()) {
    c.window_width = s->sizes[size_t(size)].w;
    c.window_height = s->sizes[size_t(size)].h;
  }
  c.fps_limit = ChoiceValue(s->frame_rate, kFrameRates, c.fps_limit);
  c.vsync = SendMessageW(s->vsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
  c.render_resolution = ChoiceValue(s->resolution, kResolutions, c.render_resolution);
  c.shadow_quality = ChoiceValue(s->shadows, kShadowQualities, c.shadow_quality);
  c.anisotropic = ChoiceValue(s->anisotropic, kAnisotropic, c.anisotropic);
  c.msaa = ChoiceValue(s->msaa, kMsaa, c.msaa);
  c.bloom_quality = ChoiceValue(s->bloom, kBloomQualities, c.bloom_quality);
  auto checked = [](HWND box) { return SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED; };
  c.foliage_antialiasing = checked(s->foliage_aa) && c.msaa > 1;
  c.fxaa = checked(s->fxaa);
  c.ambient_occlusion = checked(s->ssao);
  c.dithering = checked(s->dithering);
  c.shadow_smoothing = ChoiceValue(s->shadow_smoothing, kShadowSmoothing, c.shadow_smoothing);
  c.soft_particles = checked(s->soft_particles);
  LRESULT lang = SendMessageW(s->language, CB_GETCURSEL, 0, 0);
  if (lang >= 0 && size_t(lang) < kLanguages.size()) c.language = kLanguages[size_t(lang)].code;
  c.show_launcher = SendMessageW(s->show_at_startup, BM_GETCHECK, 0, 0) == BST_CHECKED;
  c.Sanitize();
}

// Two columns (Display + Graphics | Enhancements + Game) so the window fits a
// 1280x800 handheld screen at 150% scaling.
constexpr int kColumnWidth = 400;
constexpr int kClientWidth = 16 + kColumnWidth + 16 + kColumnWidth + 16;
// Client height follows the layout (CreateControls stores the bottom edge).
int g_client_height = 480;
constexpr int kBottomMargin = 16;

void CreateControls(LauncherState* s, HWND hwnd) {
  const Text& t = s->t;
  const int margin = 16, label_w = 170, combo_w = 200;
  const int row = 30;
  int y = margin;
  // Current column.
  int col_x = margin;
  int combo_x = col_x + 12 + label_w;
  auto column = [&](int index) {
    col_x = margin + index * (kColumnWidth + margin);
    combo_x = col_x + 12 + label_w;
    y = margin;
  };

  auto group = [&](const wchar_t* title, int rows) {
    MakeControl(s, hwnd, L"BUTTON", title, BS_GROUPBOX, col_x, y, kColumnWidth, 24 + rows * row, 0,
                true);
    y += 24;
  };
  auto label = [&](const wchar_t* text) {
    MakeControl(s, hwnd, L"STATIC", text, SS_LEFT, col_x + 12, y + 4, label_w, 20);
  };
  column(0);

  group(t({L"Display", L"Pantalla", L"Affichage", L"Anzeige", L"Schermo"}), 4);
  label(t({L"Display mode", L"Modo de pantalla", L"Mode d'affichage", L"Anzeigemodus", L"Modalit\u00e0 schermo"}));
  s->display_mode = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kDisplayMode, kDisplayModes, 1);
  y += row;
  label(t({L"Window size", L"Tama\u00f1o de ventana", L"Taille de la fen\u00eatre", L"Fenstergr\u00f6\u00dfe", L"Dimensioni finestra"}));
  s->window_size = MakeControl(s, hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                               combo_x, y, combo_w, 300, kWindowSize);
  y += row;
  label(t({L"Frame rate limit", L"L\u00edmite de FPS", L"Limite d'images/s", L"Bildratenlimit", L"Limite FPS"}));
  s->frame_rate = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kFrameRate, kFrameRates, 60);
  y += row;
  s->vsync = MakeControl(s, hwnd, L"BUTTON",
                         t({L"VSync (sync to the monitor refresh, no tearing)",
                           L"VSync (sincroniza con el monitor, sin tearing)",
                           L"VSync (synchronis\u00e9 avec l'\u00e9cran, sans d\u00e9chirement)",
                           L"VSync (mit dem Monitor synchronisiert, kein Tearing)",
                           L"VSync (sincronizzato con il monitor, senza tearing)"}),
                         BS_AUTOCHECKBOX | WS_TABSTOP, col_x + 12, y + 2, kColumnWidth - 24, 22,
                         kVsync);
  y += row + 10;

  group(t({L"Graphics", L"Gr\u00e1ficos", L"Graphismes", L"Grafik", L"Grafica"}), 4);
  label(t({L"Render resolution", L"Resoluci\u00f3n de renderizado", L"R\u00e9solution de rendu", L"Renderaufl\u00f6sung", L"Risoluzione di rendering"}));
  s->resolution = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kResolution, kResolutions, 1);
  y += row;
  label(t({L"Shadow quality", L"Calidad de sombras", L"Qualit\u00e9 des ombres", L"Schattenqualit\u00e4t", L"Qualit\u00e0 delle ombre"}));
  s->shadows = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kShadows, kShadowQualities, 1);
  y += row;
  label(t({L"Anisotropic filtering", L"Filtrado anisotr\u00f3pico", L"Filtrage anisotrope", L"Anisotrope Filterung", L"Filtro anisotropico"}));
  s->anisotropic =
      MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kAnisotropicCombo, kAnisotropic, 16);
  y += row;
  label(t({L"Antialiasing (MSAA)", L"Antialiasing (MSAA)", L"Anticr\u00e9nelage (MSAA)",
           L"Kantengl\u00e4ttung (MSAA)", L"Antialiasing (MSAA)"}));
  s->msaa = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kMsaaCombo, kMsaa, 4);
  y += row + 10;

  const int left_bottom = y - 10;

  column(1);
  group(t({L"Enhancements", L"Mejoras gr\u00e1ficas", L"Am\u00e9liorations graphiques",
           L"Grafikverbesserungen", L"Miglioramenti grafici"}),
        7);
  label(t({L"Shadow smoothing", L"Suavizado de sombras", L"Adoucissement des ombres",
           L"Schattengl\u00e4ttung", L"Ammorbidimento ombre"}));
  s->shadow_smoothing =
      MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kShadowSmoothingCombo, kShadowSmoothing, 0);
  y += row;
  label(t({L"Bloom quality", L"Calidad del bloom", L"Qualit\u00e9 du bloom", L"Bloom-Qualit\u00e4t",
           L"Qualit\u00e0 del bloom"}));
  s->bloom = MakeChoiceCombo(s, hwnd, combo_x, y, combo_w, kBloom, kBloomQualities, 1);
  y += row;
  auto checkbox = [&](const wchar_t* text, int id) {
    HWND box = MakeControl(s, hwnd, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, col_x + 12,
                           y + 2, kColumnWidth - 24, 22, id);
    y += row;
    return box;
  };
  s->foliage_aa = checkbox(
      t({L"Foliage antialiasing", L"Antialiasing de vegetaci\u00f3n",
         L"Anticr\u00e9nelage de la v\u00e9g\u00e9tation", L"Vegetations-Kantengl\u00e4ttung",
         L"Antialiasing della vegetazione"}),
      kFoliageAa);
  s->fxaa = checkbox(t({L"FXAA antialiasing", L"Antialiasing FXAA", L"Anticr\u00e9nelage FXAA",
                        L"FXAA-Kantengl\u00e4ttung", L"Antialiasing FXAA"}),
                     kFxaa);
  s->ssao = checkbox(t({L"Ambient occlusion (SSAO)", L"Oclusi\u00f3n ambiental (SSAO)",
                        L"Occlusion ambiante (SSAO)", L"Umgebungsverdeckung (SSAO)",
                        L"Occlusione ambientale (SSAO)"}),
                     kSsao);
  s->dithering = checkbox(
      t({L"Dithering (less color banding)", L"Dithering (menos bandas de color)",
         L"Tramage (moins de bandes de couleur)", L"Dithering (weniger Farbstreifen)",
         L"Dithering (meno bande di colore)"}),
      kDithering);
  s->soft_particles = checkbox(
      t({L"Soft particles", L"Part\u00edculas suaves", L"Particules douces", L"Weiche Partikel",
         L"Particelle morbide"}),
      kSoftParticles);
  y += 10;
  group(t({L"Game", L"Juego", L"Jeu", L"Spiel", L"Gioco"}), 1);
  label(t({L"Language", L"Idioma", L"Langue", L"Sprache", L"Lingua"}));
  s->language = MakeControl(s, hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                            combo_x, y, combo_w, 300, kLanguage);
  for (const LanguageChoice& l : kLanguages) {
    SendMessageW(s->language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(l.name));
  }
  y += row;
  y = std::max(y, left_bottom) + 14;

  s->show_at_startup = MakeControl(
      s, hwnd, L"BUTTON",
      t({L"Show this window at startup (or hold SHIFT)",
        L"Mostrar al iniciar (o mant\u00e9n MAY\u00daS al arrancar)",
        L"Afficher au d\u00e9marrage (ou maintenir MAJ)",
        L"Beim Start anzeigen (oder UMSCHALT gedr\u00fcckt halten)",
        L"Mostra all'avvio (o tieni premuto MAIUSC)"}),
      BS_AUTOCHECKBOX | WS_TABSTOP, margin, y, kClientWidth - 2 * margin, 22, kShowAtStartup);
  y += 34;

  const int bw = 100, bh = 30;
  MakeControl(s, hwnd, L"BUTTON", t({L"Defaults", L"Por defecto", L"Par d\u00e9faut", L"Standard", L"Predefiniti"}), BS_PUSHBUTTON | WS_TABSTOP,
              margin, y, bw + 10, bh, kDefaults);
  MakeControl(s, hwnd, L"BUTTON", t({L"Exit", L"Salir", L"Quitter", L"Beenden", L"Esci"}), BS_PUSHBUTTON | WS_TABSTOP,
              kClientWidth - margin - 2 * bw - 8, y, bw, bh, kExit);
  g_client_height = y + bh + kBottomMargin;
  HWND play = MakeControl(s, hwnd, L"BUTTON", t({L"Play", L"Jugar", L"Jouer", L"Spielen", L"Gioca"}),
                          BS_DEFPUSHBUTTON | WS_TABSTOP, kClientWidth - margin - bw, y, bw, bh,
                          kPlay, true);
  Populate(s);
  SetFocus(play);
}

LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* s = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      s = reinterpret_cast<LauncherState*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
      CreateControls(s, hwnd);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      // Transparent labels on the window background.
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_COMMAND: {
      if (!s) break;
      int id = LOWORD(wparam);
      if ((id == kDisplayMode || id == kMsaaCombo) && HIWORD(wparam) == CBN_SELCHANGE) {
        UpdateEnabled(s);
        return 0;
      }
      if (id == kDefaults) {
        conan::Settings defaults;
        defaults.language = s->settings->language;
        defaults.show_launcher = s->settings->show_launcher;
        defaults.Sanitize();
        conan::Settings* keep = s->settings;
        conan::Settings temp = defaults;
        s->settings = &temp;
        Populate(s);
        s->settings = keep;
        return 0;
      }
      if (id == kPlay) {
        if (s->shown_tick && GetTickCount() - s->shown_tick < 500) return 0;
        Capture(s);
        s->play = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (id == kExit) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

bool ShowConanLauncher(conan::Settings& settings) {
  // Crisp text on high-DPI displays (SDL sets the same awareness later).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  LauncherState state;
  state.settings = &settings;
  state.t = Text{DetectUiLanguage()};
  state.dpi = int(GetDpiForSystem());
  state.font = MakeFont(state.dpi, false);
  state.bold_font = MakeFont(state.dpi, true);

  const wchar_t kClassName[] = L"ConanLauncher";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = LauncherWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));  // conan.rc
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  // Created hidden at a provisional size; resized to the laid-out height below.
  RECT rect{0, 0, Scale(&state, kClientWidth), Scale(&state, g_client_height)};
  AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_CONTROLPARENT, UINT(state.dpi));
  int w = rect.right - rect.left, h = rect.bottom - rect.top;
  HWND hwnd = CreateWindowExW(
      WS_EX_CONTROLPARENT, kClassName, state.t({L"Conan - Settings", L"Conan - Configuraci\u00f3n", L"Conan - Param\u00e8tres", L"Conan - Einstellungen", L"Conan - Impostazioni"}),
      style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w,
      h, nullptr, nullptr, wc.hInstance, &state);
  if (!hwnd) return true;  // no launcher: start with the saved settings
  rect = {0, 0, Scale(&state, kClientWidth), Scale(&state, g_client_height)};
  AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_CONTROLPARENT, UINT(state.dpi));
  w = rect.right - rect.left;
  h = rect.bottom - rect.top;
  SetWindowPos(hwnd, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
               (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

  // Drop input queued before the window existed (see LauncherState).
  MSG drain;
  while (PeekMessageW(&drain, nullptr, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
  }
  while (PeekMessageW(&drain, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {
  }
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  SetForegroundWindow(hwnd);
  state.shown_tick = GetTickCount();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    // Tab navigation, Enter = Play (IDOK), Esc = Exit (IDCANCEL).
    if (IsDialogMessageW(hwnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  DeleteObject(state.font);
  DeleteObject(state.bold_font);
  UnregisterClassW(kClassName, wc.hInstance);
  return state.play;
}
