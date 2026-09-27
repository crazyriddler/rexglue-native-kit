# Settings, launcher and release

Reference: `reference/conan/port/src/settings.*`, `launcher_dialog.*`, `conan_app.h`,
`cpu_check.cpp`, `conan.rc`, `tools/make_release.sh`.

## User preferences (this user; apply by default)

- Defaults = the original game (720p, original shadows, the game's MSAA, 30 fps if the
  game runs at 30, VSync on, fullscreen, language auto).
- No FSR/CAS, no bicubic image upscaling (native resolution or bilinear only).
- Fixes that make higher resolutions look like the original are always on (not options).
- Every option must work; test each one before exposing it. Dependent options grey out.
- Launcher follows the Windows language (EN/ES/FR/DE/IT; English otherwise).
- No SDK overlays, achievement toasts or debug hotkeys in the release.
- Clean portable release folder: exe, needed DLLs, cfg, data/ - nothing else.
- Saves stay in Documents\<game> (SDK default user data root).
- Target hardware includes handheld PCs (6 GB RAM / 6 GB VRAM, 1280x800 at 150%).

## Settings file (`<game>.cfg`, INI next to the exe)

`Settings::Load/Save/Apply/Sanitize`; Apply pushes values into cvars before the window,
GPU and game are created (ReXApp `OnPreSetup`); command-line flags win (`HasNonDefaultValue`).
Sections: [Display] Fullscreen, WindowWidth/Height, VSync, FrameRateLimit;
[Graphics] RenderResolution (16:9 height 720-2880 or 0 = native display), ShadowQuality
1/2/4, AnisotropicFiltering, MSAA 1/4/8; [Enhancements] FoliageAntialiasing, FXAA,
AmbientOcclusion, BloomQuality, Dithering, ShadowSmoothing, SoftParticles;
[Game] Language; [Launcher] ShowAtStartup. Old values are migrated in Sanitize (keep that
habit: never break a user's cfg). `--write_default_settings=true` writes defaults and exits
(used by the release script).

## Launcher (Win32, not ImGui: an SDK ImGuiDialog crashed that early in startup)

DPI-aware, themed, two columns (Display + Graphics | Enhancements + Game), 864x447 at
100% so it fits 1280x800 at 150%. Shown on first run, when ShowAtStartup=1, or with SHIFT
held; never with `--show_launcher=false` (automation). Buttons Defaults / Exit / Play; a
500 ms guard ignores input queued before the window appeared. `--launcher_ui_language=xx`
forces a language for screenshots. Capture it with `tools/capture_window.py <exe>
<WindowClass> out.png --close` (own process only).

## App integration (ReXApp overrides)

- `OnConfigurePaths`: cfg path next to the exe, optional advanced TOML, logging off in
  Release unless `--log_level` given, game data auto-discovery (`data/` next to the exe,
  then `game/` in cwd / exe dir / three levels up).
- `OnPreSetup`: default `gpu_plugin = xenos` (legacy), run settings + launcher, install
  `NativeGraphicsSystem` into `config.graphics` (no plugin).
- `OnCreateDialogs`: unregister SDK overlay binds; `CreateAchievementNotificationDialog`
  returns nullptr.
- `GetWindowTitle()` (kit SDK virtual): the game's name only.
- `.rc`: icon (ID 1) + VERSIONINFO (FileDescription/ProductName = game name).
- `cpu_check.cpp` (baseline ISA, init_priority 101): message instead of an
  illegal-instruction crash on CPUs without AVX2.

## Release

`bash tools/make_release.sh [name]` -> `release/<name>/`: builds Release, copies
`<game>.exe` (shaders + pipeline base embedded), `rexruntime.dll`, the four VC++ runtime
DLLs, writes a default cfg only if none exists (it may be the user's), mirrors
`$PORT_DIR/game` into `data/` (robocopy /MIR /XJ). Smoke-test from the release folder with
`EXE_DIR=<release dir> bench/run_safe.sh ...` and delete the `<game>_pipelines.bin` the
test run creates. Never touch a release folder the user is playing from without asking.

Before a release: pipeline base captured from all scenarios (`tools/make_pipeline_base.sh`,
merge), re-run cmake so the RC embeds it, A/B at original settings, long-session run,
every launcher option exercised once.
