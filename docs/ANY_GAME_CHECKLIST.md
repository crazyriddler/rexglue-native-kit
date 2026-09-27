# Any-game checklist: what varies between titles and how to handle it

Conan proved the pipeline on one game. This file lists what **will** differ on the next
title and the known answer for each item, taken from three other shipped static-recomp
ports (researched 2026-09-27, details and file references in UPSTREAM_RESEARCH.md):

| Project | Recompiler | Renderer strategy |
|---|---|---|
| UnleashedRecomp (Sonic Unleashed) | XenonRecomp | Hooks on XDK D3D entry points, own RHI (plume D3D12/Vulkan), XenosRecomp shaders |
| skate3recomp (Skate 3) | ReXGlue 0.8 fork | Engine-level semantic re-renderer (hand-ported shaders), Xenos emulation kept for exempt passes, hot fallback |
| The Darkness Recomp | XenonRecomp (patched) | Engine-level capture of completed device state, D3D11, engine shader sources transpiled |
| This kit (Conan) | ReXGlue 0.10 fork | Hooks on XDK D3D entry points, PM4 mirror for constants, own D3D12, XenosRecomp shaders |

Go through this list in phase 0 (inventory) and again in phase 4 (archaeology); record the
answers in PROJECT_STATE.md.

## 1. Inputs and recompilation (phases 0-2)

| Item | Check | Answer |
|---|---|---|
| Title update | Is there a TU (`default.xexp`, usually inside an STFS `TU_*` package)? Which revision does the user own? | Extract the `.xexp` with `tools/stfs_extract.py` and place it **next to** `default.xex` in `game/` *before codegen*. The kit SDK applies a sibling `<xex>p` automatically in `UserModule::LoadFromFile` (`sdk/src/system/user_module.cpp:104`) for both codegen and runtime, so generated code and loaded image match. Confirm with the log line `Loading XEX patch from`. Decide base vs TU once: switching later means a new codegen. skate3 does the same with `patched_file_path` + `cmake/ExtractTitleUpdateXexp.py` |
| Extra modules | `.xex`/`.dll` modules besides default.xex (skate3: `EAWebkit.xex`, also TU-patched) | `rexglue init --scan-dll`; each module is its own codegen target with its own output dir and function config |
| Dump format | Folder / ISO / GOD / XContent | The kit takes an extracted folder. ISO: extract with an external tool (extract-xiso) or port skate3's `skate3_iso_installer.cpp` / UnleashedRecomp `install/iso_file_system.cpp` if the release must accept ISOs |
| Revision identity | SHA-256 of default.xex (+ .xexp) | Record in PROJECT_STATE. Release builds should check it at startup and show a clear message on mismatch (Darkness README publishes the expected hashes); the recompiled code only matches one revision |
| DLC | Packages the game can mount | skate3: `dlc/` folder next to the exe; SDK content manager + `stfs_extract.py` layout |
| Jump tables the analyser misses | UnresolvedCall into switch targets | UnleashedRecomp/Darkness ship a `*_switch_tables.toml` (XenonRecomp format); ReXGlue has `switch_tables` in the manifest (`sdk/src/codegen/config.cpp:257`) |
| setjmp/longjmp, save/restore helpers | Needed for correct codegen and for G1 below. 11 of 12 surveyed ReXGlue ports set them; ReXGlue does not detect them | Late XDK: `tools/re/xdk_layout.py` (names setjmp/longjmp/`__savegprlr` from the 2012 XDK table); otherwise by signature/semantics. ReXGlue reads `setjmp_address` / `longjmp_address` (`config.cpp:124-130`) |
| Native CRT (`[rexcrt]`) | memcpy/memset/str*/wcs*, Rtl*Heap, file I/O, fibers implemented natively by the SDK (`sdk/src/kernel/crt/`) | Used by 6 of 12 surveyed ports, not by Conan. Addresses from xdk_layout.py or semantics; verify each (REXGLUE_PORTING_RULES Phase 11). Enable after the game boots, memory/string group first, heap group last (needs all four + `rexcrt_heap_enable`) -> PERFORMANCE_GUIDE G14 |
| Split manifest | Hundreds of overrides/hooks | `includes = [...]` with per-subsystem TOMLs (reblue `config/hooks/*.toml`, reNut/reDAHM `*_func/_crt/_hook.toml`) |
| Codegen register-locality flags (G1) | Performance vs risk | TiP-Recomp ships a middle ground (`skip_lr`, `skip_msr`, `ctr/xer/cr/reserved_as_local` on; `non_argument/non_volatile_as_local` off); UnleashedRecomp ships **all** of them on (`skip_lr`, `skip_msr`, `*_as_local`) with setjmp/longjmp addresses; Darkness deliberately ships them **all off** as a "correctness profile". Kit rule: start a new port with them off (Conan-validated path), enable in phase 8 one flag at a time with A/B + long session (PERFORMANCE_GUIDE G1) |
| Translator instruction bugs | Darkness had to fix vcmp*h record masks, fnmadd, vctuxs NaN, vpk* aliasing, mulhdu record flags in XenonRecomp | Checked 2026-09-27: the kit's ReXGlue codegen already has the fixed forms (0xFFFF masks, `-std::fma`, aliasing-safe vpkuhus/vpkuwus verified by test). Open: FP record forms (`fxxx.`) do not set CR1 from FPSCR in `builders/floating_point.cpp`; if the disassembly of a new game contains any, implement or guard them (Darkness rejects them explicitly). LostOdysseyRecomp's 9-class audit (UPSTREAM_RESEARCH): sraw/srad carry was wrong in the kit SDK too and is fixed (`scripts/tests/test_codegen_sra.py`); integer Rc forms and 64-bit high-word effects of update-form loads remain to be audited |
| Shaders inside compressed archives | UE3 and other engines store shaders in compressed packages; `tools/shaders/extract_shaders.py` scans raw bytes only and finds nothing there | Harvest containers at runtime from the CreateVertex/PixelShader hooks (the shader registry already hashes them) into a corpus dump over the scenario set, or add the engine's package decompression to the extractor (LostOdysseyRecomp: 20,686 shaders in UE3 packages). Record coverage in SHADER_CATALOG.md |

## 2. Renderer strategy (phase 4 decision)

Choose once, with evidence, and record it in RENDERER_ANALYSIS.md.

| Option | When | Cost | Examples |
|---|---|---|---|
| **XDK D3D hooks + device shadow + PM4 mirror** (kit default) | The game uses the statically linked XDK D3D (almost every retail title); you want frame-exact parity | Generic: 90% of the reference renderer is reusable | Conan, UnleashedRecomp |
| **Engine-level capture** (engine draw/material/mesh functions) | The engine exposes clean draw records and either ships shader *sources* (Darkness: `System/` programs transpiled to HLSL) or the scene has few material families worth porting by hand (skate3) | Game-specific; large; parity is approximate; best performance and easiest big enhancements (ultrawide, draw distance, new shadows) | skate3, The Darkness |
| **Hybrid with fallback** | Bring-up of an engine-level renderer on a large game | Needs the emulated path kept alive | skate3: native scene, emulated menus/FMV, auto-fallback + F5 toggle |

For this user's goals (exact original look, one generic workflow) keep the XDK-hook design;
consider engine-level only if the XDK path hits a wall (e.g. MEMEXPORT-heavy or
tiling-heavy content that cannot reach 45 dB) and document why.

## 3. Game-visible behaviour the port usually has to change (phase 9)

All must keep "defaults = original game" and be tested before being exposed.

| Feature | Known technique |
|---|---|
| Higher frame rate than the console cap | Find the engine's delta-time consumers and clamp/scale them with mid-asm hooks; keep the original behaviour at the original rate (UnleashedRecomp `patches/fps_patches.cpp`: clamp dt to >= 1/60 in physics paths, camera lerp fixes). Per game; verify physics/animation at 30/60/120 |
| Ultrawide / other aspect ratios | (a) Report a wider guest video mode so the engine builds a Hor+ projection **and** CPU culling for it, while keeping guest allocations within console limits (Darkness: guest mode <= 2560x720, host scale 2-3x); (b) widen the engine's cull frustum if it ignores the video mode (skate3 `skate3_ultrawide_guest.h`, injected by a deterministic post-codegen patch); (c) keep HUD/menus centred (UnleashedRecomp `aspect_ratio_patches.cpp`, offsets/scales per UI layer). Treat as an enhancement, off by default |
| FOV | Hook the camera's FOV input before the projection store; preserve zoom/scripted ratios (Darkness `fov_camera.cpp`, skate3 `skate3_fov.cpp`) |
| Mouse look / keyboard | SDK `--mnk_mode`; engine-level mouse look needs a camera hook (Darkness `mouse_look.cpp`). Controller prompts per input device (Darkness Xelu prompts, UnleashedRecomp) |
| Controllers beyond XInput | SDL backend (skate3, UnleashedRecomp) |
| In-game settings vs launcher | Kit: Win32 launcher (user preference). Darkness rebuilt the game's own menu files for an in-game Video Settings page: much more work, only if the user asks |
| Draw distance / LOD | Engine constants (skate3 `skate3_draw_distance.cpp`); enhancement only |
| Saves | Kit: Documents\<game> (user preference). Others offer portable mode (`saves/` or `portable.txt` next to the exe) |

| Unlocked fps, second technique | When logic cannot run faster (fixed-step accumulators everywhere): keep logic at 30 Hz and interpolate what is rendered between ticks (reblue `engine/frame_interp.cpp`: cameras, bone palettes). Much more work; only for games where delta fixes fail |
| Cutscene A/V sync | In-engine cutscenes that tick per rendered frame drift against audio after any long frame: audio-master catch-up with bounded extra ticks (AC6 `ac6_cutscene_resync.cpp`) |
| Effects at full resolution | Rewrite the game's own render-buffer registration at init so every downstream size follows (AC6 `ac6_fullres_effects.cpp`; Conan did the same idea host-side with `full_scene_resolution`) |
| Mods / texture packs | SetTexture hook dump + replace (reNut, TiP, AC6). Not a user requirement; possible later |

## 4. Runtime and pacing (phase 8)

| Item | Technique | Source |
|---|---|---|
| Guest hardware threads on host cores | Map the 6 Xenon hardware-thread affinities to distinct physical cores with Windows CPU Sets; respect process affinity and small CPUs; guest processor numbers unchanged | Darkness `runtime/native/thread_topology.h` (gated: measure on the target CPU, it gave no gain in one heavy scene) |
| Present thread priority | Display/present thread `ABOVE_NORMAL` so presents are not starved while loaders saturate cores (1.3-1.6 s Present blocks at transitions -> < 18 ms) | Darkness RENDERING.md |
| Precise guest sleeps | `KeDelayExecutionThread` on a high-resolution waitable timer, relative/absolute semantics kept | Darkness; the kit SDK already has waitable-timer sleeps (E049) |
| VRR | `ALLOW_TEARING` on creation and on sync-interval-0 presents | Darkness; kit SDK presenter already does it |
| Handheld CPU envelope | Run the benchmark scenario with the process restricted to 3 cores / 6 logical CPUs (affinity 0x3F, start the exe with `cmd //c start "" /affinity 3F <exe>` from Git Bash) as a handheld proxy; size worker pools from the process affinity (`GetProcessAffinityMask`), not `hardware_concurrency()` | LostOdysseyRecomp "3C6T" cards |
| Logging cost | Unbuffered log writes produced metronomic 45-73 ms hitches every report interval; buffer stdout/stderr, flush on exit/crash | Darkness. Kit: Release has logging off; do not benchmark hitches with a dev build that logs every 5 s |
| PSOs at load time | Compile the pipelines a loaded asset will need while the game's loading screen is up, and hold the "loaded" flag until they finish | UnleashedRecomp `EnqueuePipelineTask` + `eDatabaseDataFlags_CompilingPipelines`. XDK-level equivalent for the kit: at the CreateVertex/PixelShader hook, enqueue the recorded PsoRecords that use that shader hash at high priority (gated item G13) |
| Upload bursts | First-sight page-in of 25 MB geometry froze a frame 81 ms; Darkness caps fresh decode/upload at 4 MiB per frame and **defers draws** | Do not copy the draw deferral (it drops submitted draws for a frame, violating the kit invariants); use it as evidence for G7 (decode/upload off the worker, waiting instead of dropping) |

| Scheduling races the 360 never showed | ReXGlue ignores guest thread priorities and affinities by default (`ignore_thread_priorities` / `ignore_thread_affinities` = true, `sdk/src/system/xthread.cpp:40-43`). Game job systems that relied on fixed cores/priorities race on PC: one-frame flicker, wrong save operation, rare loader crashes (AC6). Classify an intermittent bug by re-running with both cvars `false`; fix the race at its submitter (store before submit), not by a guard | AC6 `ac6_effect_mode_fix.cpp`, `ac6_storage_submit_order_fix.cpp` |
| Fixes the developer already shipped | If porting the base revision, diff the TU's code for the same function: timing bugs are often fixed there | AC6 storage fix |

## 5. Testing that scales across games

| Practice | Source | Kit action |
|---|---|---|
| Renderer contract tests on WARP (clears in rects, resolves, scale 1/2/3, alpha modes) running without the game | Darkness `tests/` (64 CTest targets, hardware + WARP + debug layer) | Add a small `port/tests/` target for the generic pieces (texture untiling, rect-list expansion, EDRAM overlap model, HostPx rounding) when porting game #2; they are pure functions today |
| Build fingerprint: stale objects after a rollback crashed a build | Darkness `tools/update_native_stamp.py` | Kit already syncs DLLs (`bench/build.sh`); if incremental builds misbehave after reverting experiments, do a clean rebuild before measuring |
| Deterministic post-codegen patches with an anchor and a hard failure when the anchor is missing | skate3 `cmake/ApplySkate3CodegenPatches.cmake` | This is the kit's "deterministic post-codegen patch" rung (CLAUDE.md): always FATAL_ERROR when the anchor is not found, never silently skip |
| Publish expected input hashes and the exact dump procedure | Darkness README | Release notes of each port |
