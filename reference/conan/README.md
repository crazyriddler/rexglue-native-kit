# Reference port: Conan (2007, Nihilistic / THQ), title 545107DA

The complete worked example this kit was distilled from (September 2026, beta release).
Read-only reference: copy from it, do not build it here (its game data and generated code
are not included).

| Path | What |
|---|---|
| `port/conan_manifest.toml` | codegen manifest: every function-boundary override and midasm hook, each commented with its evidence (E004-E046) |
| `port/src/native/` | the native renderer: capture hooks (d3d_capture.cpp), PM4 mirror, renderer (native_renderer.cpp), NativeGraphicsSystem, texture decoder, shader registry, pipeline cache, hang watchdog, HLSL helpers (shaders/*.hlsl + compiled headers) |
| `port/src/settings.*`, `launcher_dialog.*` | cfg settings + Win32 launcher (EN/ES/FR/DE/IT) |
| `port/src/conan_app.h` | ReXApp integration (paths, logging, settings, native graphics system, overlays off, title) |
| `port/src/hooks.cpp` | midasm guard functions and the guest busy-wait idle hook |
| `port/src/cpu_check.cpp`, `port/src/conan.rc` | AVX2 check, icon + VERSIONINFO |
| `port/CMakeLists.txt`, `CMakePresets.json` | project build (shader pack RCDATA 2, pipeline base RCDATA 3, flags) |
| `port/docs/error_log.md` | E001-E051: every port problem with its evidence chain |
| `port/docs/port_status.md` | milestone history of the recompilation phase |
| `port/patches/sdk/` | the crash-diagnostics SDK patch used during crash triage |
| `port/scripts/` | original versions of the scripts generalized in the kit's scripts/port |
| `docs/EXPERIMENT_LOG.md` | EXP-001-048: every renderer/performance experiment with numbers |
| `docs/RENDERER_ANALYSIS.md` | recovered XDK D3D + game renderer map (addresses, structures, passes) |
| `docs/SHADER_CATALOG.md`, `PIPELINE_CATALOG.md`, `BENCHMARKS.csv`, `PROJECT_STATE.md`, `OPEN_QUESTIONS.md` | final state of the per-game documents (templates for yours) |
| `bench/*.txt` | autoinput scenario scripts (boot, FMV, new game, saved level, combat, pause, long session) |

Final numbers: A/B vs Xenos 47-58 dB; unlocked 4.0 ms/frame (legacy 4.77, ceiling 3.97);
30 fps cap at ~45% of one core; RAM 1.6 GB, VRAM 0.36 GB.
