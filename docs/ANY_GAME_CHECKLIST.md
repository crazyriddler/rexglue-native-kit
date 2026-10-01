# Any-game checklist: what varies between titles and the known answer

Conan proved the pipeline on one game. This file lists what **will** differ on another title
and the answer already known for each item, taken from other static-recompilation ports
(evidence, commits and file references: UPSTREAM_RESEARCH.md). It holds game variability
only: optimization gates live in PERFORMANCE_GUIDE (G-numbers referenced here), procedures in
the playbook. Record the answers in the "Game facts" table of `docs/PROJECT_STATE.md`.

Projects the answers come from: UnleashedRecomp (XenonRecomp, XDK-hook renderer on plume),
reblue (ReXGlue 0.10, XDK-hook renderer on plume), skate3recomp (ReXGlue 0.8 fork,
engine-level renderer), The Darkness Recomp (XenonRecomp, engine-level D3D11),
LostOdysseyRecomp (XenonRecomp, PM4-interpreting renderer), AC6_recomp and 8 more ReXGlue
ports (Xenos emulation), dc3-decomp (full XDK symbol map).

## 1. Inputs and recompilation (phases 0-1)

| Item | Check | Answer |
|---|---|---|
| Title update | A TU exists (`default.xexp`, usually inside an STFS `TU_*` package)? Which revision does the user own? | Extract with `tools/stfs_extract.py`; place `default.xexp` **next to** `default.xex` before codegen. The SDK applies a sibling `<xex>p` in `UserModule::LoadFromFile` (`sdk/src/system/user_module.cpp:104`) for codegen and runtime alike; log "Loading XEX patch from". Decide once (switching = new codegen). skate3 does the same via `patched_file_path` |
| Fixes the developer shipped | Porting the base revision while a TU exists | Diff the TU's code of a function that misbehaves: timing bugs are often fixed there (AC6 storage race) |
| Extra modules | `.xex`/`.dll` besides default.xex (skate3: `EAWebkit.xex`, TU-patched too) | `.dll`: `rexglue init --scan-dll`; other `.xex`: `rexglue init module --project-root . --xex-path ... --guest-path ...`; each module = own codegen target, output dir and function config |
| Dump format | Folder / ISO / GOD / XContent | The kit takes an extracted folder; extract ISO/GOD first (extract-xiso or similar). An installer accepting ISOs: skate3 `skate3_iso_installer.cpp`, UnleashedRecomp `install/iso_file_system.cpp` |
| Revision identity | SHA-256 of default.xex (+ .xexp) | Record it; a release should verify it at startup and explain a mismatch (the recompiled code matches one revision only; Darkness publishes its hashes) |
| DLC | Packages the game mounts | `stfs_extract.py` into the content layout; skate3 reads a `dlc/` folder |
| XDK revision | `xdk_sigs.py match`: exact vs fuzzy/missing | exact = Conan-era XDK (~2006-07). Many fuzzy/missing = later XDK -> `tools/re/xdk_layout.py` with the 2012 table (`xdk_2012_dc3_symbols.tsv`); still missing -> semantic discovery (GAME_ADAPTATION_GUIDE §1) |
| setjmp/longjmp, `__savegprlr`/`__restgprlr` | Correct codegen needs `setjmp_address`/`longjmp_address` (11 of 12 surveyed ReXGlue ports set them; ReXGlue does not detect them) | Found by xdk_layout.py (late XDK) or signature/semantics after the first codegen; set in the manifest; regenerate (playbook phase 1) |
| Native CRT `[rexcrt]` candidates | memcpy/memset/str*/wcs*, XMemCpy, Rtl*Heap, file I/O, fibers (SDK implementations in `sdk/src/kernel/crt/`, ~70 functions) | Used by 6 of 12 surveyed ports, not by Conan. Collect addresses in phase 1, verify each (REXGLUE_PORTING_RULES, its Phase 11), apply as G14 in the phase-3 codegen window |
| Codegen register-locality flags | Which of `skip_lr`, `skip_msr`, `*_as_local` | Field data: UnleashedRecomp all on; TiP `skip_lr/skip_msr/ctr/xer/cr/reserved` on, `non_argument/non_volatile` off; The Darkness all off. Apply as G1 in the phase-3 codegen window |
| Jump tables the analyser misses | UnresolvedCall into switch targets | Manifest `switch_tables` (`sdk/src/codegen/config.cpp:257`); UnleashedRecomp/Darkness ship `*_switch_tables.toml` |
| Vtable thunks the analyser misses | `Call to invalid or unregistered function` inside a packed run of `addi r3,r3,-N; b` (MSVC multiple-inheritance adjustor thunks, referenced only from vtables) | `python scripts/port/find_adjustor_thunks.py --toml` lists every unregistered one (exact pattern: register the `high` ones in one go, check the lone ones). VivaPinataRecomp `find_thunk_holes.py`; runtime data-pointer/stub sweeps for vtables in its `debug_tools.h` |
| Many overrides/hooks | Manifest grows to hundreds of entries | Split with `includes = [...]` per subsystem (reblue `config/hooks/*.toml`, reNut/reDAHM `*_func/_crt/_hook.toml`) |
| Translator instruction bugs | Wrong results without a crash in math/bit code | Audited 2026-09-27 against Darkness and LostOdysseyRecomp fixes: vcmp*h masks, fnmadd, vpk* aliasing, BLRL, RLWIMI, BDNZF already correct; **sraw/srad carry, in-place `vpkd3d128` (vD == vB) and `vupkd3d128` overflow-to-NaN were wrong and are fixed** (`scripts/tests/test_codegen_sra.py`, `sdk/tests/ppc/asm/instr_vp*kd3d128.s`; VivaPinataRecomp found the vpkd3d128 one as a white terrain). Suspect, unfixed: `vpkd3d128`/`vupkd3d128` type 6 (4_20_20_20) differs from Xenia. Still open: FP record forms (`fxxx.`, CR1 from FPSCR not modelled in `builders/floating_point.cpp`), integer Rc-form coverage, 64-bit high word of update-form loads. If the disassembly uses them, add a case in `sdk/tests/ppc/asm/` first (runs through the real recompiler, VALIDATION_GUIDE "SDK self-tests"), then fix the builder |

## 2. Boot and runtime behaviour (phase 2)

| Item | Check | Answer |
|---|---|---|
| Scheduling races the 360 never showed | One-frame glitches, wrong results, rare crashes that depend on host load | ReXGlue ignores guest thread priorities and affinities by default (`ignore_thread_priorities`/`ignore_thread_affinities` = true, `sdk/src/system/xthread.cpp:40-43`). Re-run with both `false` to classify; fix the race at the submitter (store before submit), not with a guard (AC6 `ac6_effect_mode_fix.cpp`, `ac6_storage_submit_order_fix.cpp`) |
| Cutscene A/V drift | In-engine cutscenes tick per rendered frame while audio runs on its own clock | Audio-master catch-up with bounded extra ticks when >= 2 behind (AC6 `ac6_cutscene_resync.cpp`) |

## 3. Renderer strategy and shaders (phases 4-5)

| Option | When | Cost | Examples |
|---|---|---|---|
| **XDK D3D hooks + device shadow + PM4 mirror** (kit default) | The game uses the statically linked XDK D3D (almost every retail title); frame-exact parity wanted | Generic: ~90% of the reference renderer is reusable | Conan, UnleashedRecomp, reblue |
| Engine-level capture (engine draw/material/mesh functions) | The engine exposes clean draw records and ships shader *sources* (Darkness) or has few material families to port by hand (skate3) | Game-specific, large, approximate parity; easiest big enhancements | skate3, The Darkness |
| Hybrid with fallback | Bring-up of an engine-level renderer on a large game | Emulated path kept alive | skate3 (native scene, emulated menus/FMV, F5 toggle) |

For this user's goals keep the XDK-hook design; consider engine-level only if the XDK path
cannot reach 45 dB (e.g. MEMEXPORT-heavy content) and record why. One authoritative output
path only (AC6 abandoned a second "replay" renderer competing with Xenos).

| Item | Check | Answer |
|---|---|---|
| Shaders in compressed packages | `extract_shaders.py` finds few containers (it scans raw bytes; UE3 compresses its packages) | Add a cvar-gated container dump (one file per container_hash) to the CreateShader hooks in `shader_registry.cpp` - it hashes them but has no dump yet - and harvest over the scenario set; or decompress the packages first. `<TITLE_ID>.xsh` (Xenos runtime storage) gives ucode only, for coverage checks. LostOdysseyRecomp: 20,686 shaders in UE3 packages |
| Occlusion queries | Capture shows VIZ_QUERY / occlusion use | Native init fakes "visible" (EXP-013): the game submits more than on the console. Native queries read back one frame late = G3 (reblue `occlusion.cpp`) |
| MEMEXPORT | Shaders with memory export | Not used by Conan: implement as UAV writes or a compute pass |

## 4. Game-visible options (phase 9)

All off by default and tested before being exposed (CLAUDE.md user preferences).

| Feature | Known technique |
|---|---|
| Frame rate above the console cap | (a) Delta-time fixes: find the engine's dt consumers, clamp/scale them with mid-asm hooks, keep the original behaviour at the original rate (UnleashedRecomp `patches/fps_patches.cpp`; AC6 `ac6_fps_physics_fix.cpp` for fixed per-frame accumulators). (b) When logic cannot run faster: keep logic at 30 Hz and interpolate cameras and bone palettes between ticks (reblue `engine/frame_interp.cpp`, much more work). Verify physics/animation at 30/60/120 |
| Ultrawide / other aspect ratios | (a) Report a wider guest video mode so the engine builds Hor+ projection **and** CPU culling, guest allocations kept within console limits (Darkness: guest <= 2560x720, host scale 2-3x); (b) widen the engine's cull frustum if it ignores the mode (skate3 `skate3_ultrawide_guest.h`, deterministic post-codegen patch); (c) centre HUD/menus per UI layer (UnleashedRecomp `aspect_ratio_patches.cpp`); (d) patch the backbuffer dims, per-frame 1280 forcing and every projection build (reblue `output_resolution.toml`) |
| Effects at full resolution | Rewrite the game's render-buffer registration at init so every downstream size follows (AC6 `ac6_fullres_effects.cpp`); Conan did it host-side (`full_scene_resolution`, always on) |
| FOV | Hook the camera FOV input before the projection store, preserve zoom/scripted ratios (Darkness `fov_camera.cpp`, skate3 `skate3_fov.cpp`) |
| Mouse look / keyboard, controllers | SDK `--mnk_mode`; mouse look needs a camera hook (Darkness `mouse_look.cpp`); SDL backend for non-XInput pads; prompts per device (Darkness, LostOdysseyRecomp) |
| Draw distance / LOD | Engine constants (skate3 `skate3_draw_distance.cpp`) |
| Game text language | The console picks it from the dashboard: SDK cvars `user_language` (XLanguage: 1 en, 2 ja, 3 de, 4 fr, 5 es, 6 it, 7 ko, 8 zh-Hant, 9 pt, 10 zh-Hans, 11 pl, 12 ru; default 1) and `user_country` (default 103 = US; selects regional variants such as `englishus`). Default from the Windows language when the disc has it (what a console in that language does), launcher option to override; verify the file the game opens in the VFS log (VivaPinataRecomp: `user_country` 103 -> `englishus.bnl`) |
| In-game settings page | Darkness rebuilt the game's own menu files: much more work than the kit's launcher; only on request |
| Mods / texture packs | SetTexture hook dump + replace (reNut, TiP, AC6); not a current requirement |

## 5. Runtime facts already handled or tracked elsewhere

| Item | Status |
|---|---|
| Precise guest sleeps (waitable timers) | kit SDK (E049); Darkness did the same |
| VRR: `ALLOW_TEARING` on creation and sync-interval-0 presents | kit SDK presenter |
| Present thread starvation at loads | G12 (Darkness: ABOVE_NORMAL fixed 1.3 s Present blocks) |
| Logging cost | Release logging off; unbuffered dev logs cause periodic 45-73 ms hitches (Darkness): never judge hitches on a dev build that logs periodically |
| PSOs at load time | G13 (UnleashedRecomp, reblue gate the loading screen on pipeline compilation) |
| Upload bursts at first sight | G7; Darkness's per-frame upload cap **defers draws** (violates the kit invariants), use it only as evidence |

## 6. Testing that scales across games

| Practice | Kit action |
|---|---|
| Renderer contract tests on WARP without the game (Darkness: 64 CTest targets) | When porting game #2, add `port/tests/` for the pure pieces (texture untiling, rect-list expansion, EDRAM overlap model, HostPx rounding) |
| Codegen semantic tests (LostOdysseyRecomp: 3,258 instruction checks) | `scripts/tests/test_codegen_sra.py` is the pattern: render the builder's emitted code, compare with the ISA; add a case per instruction bug |
| Deterministic post-codegen patches with anchor + hard failure (skate3 `ApplySkate3CodegenPatches.cmake`) | The kit's post-codegen rung (CLAUDE.md rule 2) |
| Stale objects after reverting experiments (Darkness build fingerprint) | Clean rebuild before measuring when an incremental build behaves oddly; `bench/build.sh` already syncs SDK DLLs |
