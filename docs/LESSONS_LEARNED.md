# Lessons learned (symptom -> cause -> fix)

Distilled from the Conan (2007) port: 51 port errors (reference/conan/port/docs/error_log.md)
and 48 native-renderer experiments (reference/conan/docs/EXPERIMENT_LOG.md). Read the
section for the phase you are in **before** debugging. Each entry names the reference
entry (E0xx / EXP-0xx) that has the full evidence chain.

Conventions: "guest" = the Xbox 360 code/addresses; "host" = PC; `sub_XXXXXXXX` =
recompiled function for guest address 0xXXXXXXXX.

---

## A. Toolchain and SDK build (phase 0)

| Symptom | Cause | Fix | Ref |
|---|---|---|---|
| No clang/ninja/Windows SDK, no admin rights | Fresh PC, VS Installer never completed | Portable LLVM tarball + `xwin` (CRT/SDK MSI payloads unpacked without admin) + portable Ninja/CMake/Python, all in `tools/toolchain/` by `scripts/setup_toolchain.sh`, wired with INCLUDE/LIB and `-fuse-ld=lld` -> `scripts/dev_env.sh` | E001 |
| `lzxd.c:1:1: expected identifier` | Git without symlink privilege writes symlinks as text files | `scripts/port/fix_broken_symlinks.py <submodule dir>` (the kit's vendored sdk/thirdparty is already fixed) | E002 |
| Debug `rexglued.exe`: `ucrtbased.dll` missing | Debug CRT only ships with a full VS install | Use **RelWithDebInfo** for anything that must run; Debug only compiles | E003 |
| `_mm_shuffle_epi8 requires ssse3` in SDK sources | The game project (outer CMake) did not pass `-march` to the SDK subdirectory | Configure the game with `-march=x86-64-v3 -ffp-contract=off` (release) or at least v2 | E007, EXP-047 |
| `imgui.h not found` compiling rex_app.cpp | REXSDK_DIR (add_subdirectory) mode does not export imgui includes | `target_link_libraries(<game> PRIVATE imgui::imgui)` | E008 |
| Stale behaviour after an SDK change | CMake copies SDK DLLs only when the exe relinks | Always build with `bench/build.sh` (syncs rexruntime.dll) | EXP-005 |
| CMake cannot find fmt/xxHash/imgui... in `sdk/thirdparty` | An entry was deleted or emptied (the vendored code is in git) | `git checkout -- sdk/thirdparty/<name>` or `bash scripts/restore_sdk_thirdparty.sh <name>` (SDK base pin + kit patch) | kit |
| `Git submodule 'x' is not initialized` with the kit SDK | Vendored copy has no `.git` | Already patched in the kit (`sdk/thirdparty/CMakeLists.txt` accepts vendored content) | kit |

## B. Codegen (phase 1): "0 errors" is not "complete"

| Symptom | Cause | Fix | Ref |
|---|---|---|---|
| `UnresolvedCall ... target not in any function` | Real code reached only by a tail `b` (no `bl`, vtable, PDATA) | `[entrypoint.functions.0xADDR] end = 0x...` with the end read from the disassembly (padding / next function) | E004, E006 |
| Unresolved target is a block of another function | Mis-seeded function start | `parent = 0xREALSTART` chunk; a shared epilogue block needs **its own** chunk entry | E005 |
| `use of undeclared label loc_X` at build | Two functions fall through into each other and landed in different .cpp files (goto cannot cross TUs) | Override the **first** function's `end` to swallow the second; `parent` does NOT merge output files. Scan all: `scripts/port/find_cross_file_gotos.py` | E009 |
| Top-level `[[midasm_hook]]` silently ignored | Must be `[[entrypoint.midasm_hook]]` | Nest it; codegen emits the extern automatically | E016 |
| Midasm hook signature | `bool name(PPCRegister& r)` (plain C++ linkage). With `return`/`jump_address_on_*` the call is wrapped in `if (hook(...))` | `jump_address_on_false` may target ANY instruction boundary in the same function (codegen synthesizes the label) | E016, E027 |
| Need LR inside a hook | The manifest register list has no `lr` | `rex::runtime::ThreadState::Get()->context()->lr` inside the hook | E026 |

## C. Boot and runtime crashes (phase 2)

| Symptom | Cause | Fix | Ref |
|---|---|---|---|
| Exe seems hung, low CPU, no log | Modal MessageBox `--game_data_root was not provided` (even with --headless) | `OnConfigurePaths`: auto-discover `data/` (release) or `game/` next to exe/cwd | E010, E042 |
| Window black, log: `VdInitializeRingBuffer: no GPU emulation loaded` | No GPU plugin selected | `rexglue_setup_target(<game> GPU_PLUGINS xenos)` + default `config.gpu_plugin = "xenos"` in OnPreSetup (legacy phase) | E011 |
| `[FATAL] Call to invalid or unregistered function at 0x...` although codegen had 0 errors | Code reached only through data-driven indirect calls; gap-fill does not seed a function after an **unpadded `bctr`** or an **unconditional tail `b`** | Verify the address in the disassembly, add `[entrypoint.functions.0xADDR] end=...`. When the crash is inside a packed family of vtable thunks, register the whole visible family at once. `scripts/port/find_unregistered_after_bctr.py` lists candidates (Conan: 725, only ~15 ever hit) - fix reactively, never batch-guess boundaries | E012-E015, E031-E046 |
| Asset path with a doubled folder (`d:\Shaders\Shaders\x`) | The guest itself builds that path | Directory **junction** inside game/ (no privilege needed) | E012 |
| Guest AV reading a small offset (`0x1A4`) after failed asset lookups | Missing optional asset makes a loader leave tables uninitialized | Find the real asset elsewhere in the dump; supply it (hard link / convert TGA->DDS with `scripts/port/tga_to_dds.py`), do not invent data | E016, E030 |
| Timing-dependent null/garbage pointer crashes in a loader | Race exposed by host timing (first classify with the thread priority/affinity cvars, next row) | Midasm guard hooks (`nonnull`, `plausible >= 0x10000`, `valid code ptr`) that jump to the function's **own** existing bail-out. Verify the bail target is an exit, not a loop head (E024 created an infinite loop) | E016-E029 |
| Intermittent one-frame glitch, wrong result or rare crash that depends on host load | Guest job system relied on the 360's fixed cores / thread priorities; ReXGlue ignores both by default | Re-run with `--ignore_thread_priorities=false --ignore_thread_affinities=false` to classify; fix the submit-order race in a hook (AC6 survey, UPSTREAM_RESEARCH) instead of guarding its symptom | AC6 |
| Process "hangs" with 2 threads burning CPU, frozen PPC registers | A guard jumped to a loop re-entry point -> host-level infinite loop | ProcDump full dump + `windbg-tool` (DbgEng from NuGet, no admin) to get a host stack; fix the jump target | E025 |
| AV inside SDK native code (`AudioSystem::SubmitFrame`) | `assert_true` compiles out in RelWithDebInfo | Real runtime check + drop frame | E034 |
| Intermittent freeze at level load (1 in 3-6 runs), last log line an audio drop | Audio client unregistered while its callback was running; the guest mixer thread then waited forever | SDK fix already in the kit (UnregisterClient waits for in-flight callback). Use `bench/repro_freeze.sh` + the hang watchdog for any "sometimes freezes" | E036, EXP-034 |
| Game "stuck" on a menu with scripted/keyboard input | Launched without `--mnk_mode`, or `SendKeys` used | `--mnk_mode` + `--autoinput_script` (or `keybd_event`); log at the input handler before blaming input | E044, M9 notes |
| Attract-mode video mistaken for gameplay | Title screens loop demo videos | Check draw counts / input response | M9 notes |
| Diagnosing crashes without a debugger | - | `patches/sdk`-style diagnostics: symbolize host RIP with dbghelp in the exception handler, dump PPCContext in the AV callback (reference/conan/port/patches/sdk/0001-crash-diagnostics.patch) | E016 |

## D. Legacy (Xenos) path problems worth knowing (phase 2-3)

| Symptom | Cause | Fix | Ref |
|---|---|---|---|
| 3D scene black on a **cold** shader cache only | Async pipeline creation dropped a one-time draw (exposure init) | Draws wait for pending pipelines (kit SDK default) | EXP-005 |
| Frame time quantized to the monitor refresh | Present blocked the D3D12 queue lock | Frame-latency waitable swap chain (kit SDK default) | EXP-007 |
| Character nearly black | NaN in unused constant lanes (0*NaN=0 on Xenos, NaN in HLSL) | Sanitize constants NaN->0 at upload (both paths) | E041, EXP-021 |
| Foliage stretched over the screen | Guest vertex count exceeds the buffer capacity for quad lists | Clamp to the fetch-constant capacity (legacy path) | E047 |
| Low FPS after long debugging | Investigation instrumentation left in hot paths (write watches on hot pages, probe midasm hooks, per-draw scans) | Remove all of it; measure with guest swap counter, not RTSS | E048 |
| 30 fps with vsync / hitchy without | Windows timer at 15.6 ms | `timeBeginPeriod(1)`, waitable-timer sleeps, vblank catches up without bursts (kit SDK) | E049 |
| Drops near god rays | Occlusion queries waited for the GPU | One-frame-late query results (kit SDK) | E050 |
| Whole frame re-uploaded | `clear_memory_page_state=true` | Default false (kit SDK) | E051 |

## E. Native renderer bring-up (phase 5-7)

| Symptom | Cause | Fix | Ref |
|---|---|---|---|
| Only a few % of draws reach the renderer | Occlusion queries return 0 when Xenos draws nothing -> game culls everything | Native init: fake visible counts (`occlusion_query_enable=false`) | EXP-013 |
| GPU fence never completes (no TDR) | A resolve destination recreated with another format while in flight | Key resolve textures by (address, format, size); fence-tagged deferred release (`Retire()`) | EXP-012 |
| Intermittent GPU stall, breadcrumbs stop at trivial draws | Native renderer shared the presenter's D3D12 queue | Own direct queue; copy into presenter output on the presenter queue with a cross-queue wait | EXP-026 |
| Depth pre-pass vs opaque disagree, bloom haze accumulates | Guest surfaces alias by EDRAM base, not by object | Host RT key = EDRAM base + format + size (+samples); EDRAM overlap model clears/reinterprets on rebind | EXP-013, EXP-024, EXP-027 |
| Giant shard triangles | Missing strip restart | IBStripCutValue 0xFFFF/0xFFFFFFFF | EXP-013 |
| Viewport wrong size | Device viewport struct holds floats | Read as float | EXP-013 |
| Index/texture lookups never match | Objects hold **virtual** addresses (0xE0000000 view, +0x1000), fetch constants hold **physical** | Normalize with a `GuestPhysical()` helper everywhere | EXP-013, EXP-016 |
| Random flat "planes", constants stale | Constants bypass the device shadow: LOAD_ALU_CONSTANT literals at shader bind, GpuBeginShaderConstantF4 ring writes, inline SET_CONSTANT, inline type-0 blocks | **PM4 mirror**: parse the XDK command segment between draws (type-0/1, SET_CONSTANT, LOAD_ALU_CONSTANT, INDIRECT_BUFFER); matches Xenia's register file exactly | EXP-018, EXP-021, EXP-023 |
| Menus accumulate / pause background missing | `D3DDevice_Clear` uses an internal draw, not the hooked draw entry points | Hook Clear; XDK flags: TARGET0-3 = bits 0-3, ZBUFFER 0x10, STENCIL 0x20 (NOT the D3D9 values) | EXP-035 |
| Post-process quads cover only a triangle | Rect-list 4th vertex built wrongly | Xenia rule: the longest edge is the diagonal, v3 = d1 + d2 - corner | EXP-031 |
| Upscale/full-screen passes land in a corner | Screen-space draws (PA_CL_VTE_CNTL without viewport scale) | `g_ScreenXform` pixel->NDC; D3D renders the intersection of bound RT/DS sizes -> larger depth surface when RT0 is bigger | EXP-017 |
| Blue grass / cyan leaves | `RB_COPY_DEST_INFO` swap bit is reset right after each copy | Capture copy state at copy-mode draws (`RB_MODECONTROL == 6`); compose R<->B in the SRV mapping | EXP-024 |
| Sand/terrain white, image too bright | Texture fetch sign = GAMMA | PWL gamma in the shader (not sRGB); descriptor bit 31 | EXP-027, EXP-031 |
| Fog/“planes” everywhere | `k_16_16_16_16_EXPAND` decoded as null | EXPAND formats are 16-bit float | EXP-019 |
| Any texture shows another texture's data after a video | SRV slot freed twice (per-frame blit SRV with `--next` on a recycled slot) | Allocate fixed slots once; never "give back" by decrementing | EXP-033 |
| One-sided bright outlines on every edge (A/B ~34 dB) | Xenos D3D9 pixel centers | Half-pixel offset (1/w, -1/h) through `g_HalfPixelOffset` -> A/B 50 dB | EXP-032 |
| Black foliage cards | Alpha test not applied | Spec constant bit from RB_COLORCONTROL + threshold from RB_ALPHA_REF | EXP-022 |
| Menu/pause textures stale | Dynamic textures rewritten in place | Content hash + guest physical write watches (per 4 KB page), arm -> hash -> decode order | EXP-019, EXP-029, EXP-033 |
| First frames black after a load spike | Per-frame upload ring exhausted -> all later draws dropped | Overflow upload pages retired with the frame fence | EXP-025 |
| Fence landed byte-reversed, main thread spins in BlockUntilIdle | Native GPU store applied GpuSwap AND a byteswap | Store `GpuSwap(value)` in host order; read-pointer/scratch write-back explicitly big-endian | EXP-036 |
| Black screen in release only | Resource table entries read as an aligned struct (20-byte entries read as 24) | memcpy each packed entry | EXP-037 |
| Black image after raising resolution | Reduction targets (1x576, 1x1 luminance) scaled | Targets < 16 px stay 1:1 | EXP-037 |
| Deferred lighting only in the top-left quadrant at 2x | Pixel-position input in host pixels | `g_PixelPosScale` = 1/scale | EXP-037 |
| Doubled shadow edges at higher shadow quality | Shader offsets/weights computed on the host texel grid, tap spacing in guest texels | TEX_SCALE bits keep blur kernels in guest texels; for the shadow atlas instead scale the baked PCF literals (`g_ShadowAtlasTexelScale`) | EXP-038, EXP-041 |
| Square/striped glows and particles above 720p | Finest mips of small effect textures magnified | Smooth effects: cubic magnification + LOD bias log2(scale) for small textures in blended draws; linear sampling of upscaled color resolves (always on) | EXP-046, EXP-048 |

## F. Performance (phase 8)

| Finding | Detail | Ref |
|---|---|---|
| Measure first | Legacy frame: guest waiting on the CP; CP CPU in UpdateBindings; null-draw mode gives the guest-bound ceiling | EXP-006, EXP-008 |
| Never read write-combined upload memory | Byte-swapping in the upload heap: 768 ms/frame -> 20.8 ms after swapping in a cached scratch buffer | EXP-014 |
| Re-upload only what a draw reads | Dynamic VBs appended with NOOVERWRITE: dirty ranges + per-draw vertex range (48 GB -> 1.3 GB per run) | EXP-025 |
| Resolve textures churn | Same memory resolved at different sizes -> key includes size | EXP-029 |
| Move D3D12 recording off the game's render thread | Capture WorkCmds on guest threads, record on a worker (render thread native share 27% -> 8%); lag-1 swap | EXP-030 |
| Busy-waits burn a core at a frame cap | XDK BlockOnFence poll, PM4 WAIT_REG_MEM spin, SDK TimerQueue spin, guest `db16cyc` loops, ImGui continuous repaint | EXP-039, EXP-040 |
| Idle-wait thresholds | Sleeping after 100-500 us made unlocked frames 2-6x slower (intra-frame handoffs): spin 2 ms, then 250 us sleeps | EXP-040 |
| Memoize pure guest functions on hot paths | Per-draw parameter-name lookup cached per thread (-4%) | EXP-047 |
| Shader stutter is driver PSO compilation | Offline DXIL is not enough: record PSOs, precompile at startup on low-priority threads, embed a base list | EXP-044 |
| x86-64-v3 | ~2-5% on the recompiled code; needs `-ffp-contract=off` to keep PPC float rounding and a baseline-ISA CPU check | EXP-047 |
| Remove debug from hot paths | Clock queries / logs per draw cost ~2% CPU even when never printing | EXP-047 |
| Renderer-side optimization proposals (GPU-driven, ExecuteIndirect, culling, aliasing) | On an XDK-era title the frame is guest-bound (Conan 4.03 ms vs 3.97 ms guest ceiling, GPU 1.3 ms): they cannot move the frame. Classify the bound first; see docs/STRATEGY_REVIEW.md | STRATEGY_REVIEW |

## G. Settings, launcher, release (phase 9-10)

| Lesson | Ref |
|---|---|
| Defaults = the original game. Fixes that make higher resolutions look like the original (full-resolution scene, smooth effects) are always on, not options | EXP-048 |
| Every option must work; test each (the MSAA option was removed and re-added only after testing Off/4x/8x) | EXP-042, EXP-048 |
| Dependent options grey out (foliage AA needs MSAA) and are forced off in Apply | EXP-048 |
| The launcher must fit a 1280x800 handheld at 150%: two columns | EXP-045, EXP-048 |
| FPS cap: the game presents every second vblank -> native vblank = 2 x fps; VSync = Present(1) without tearing | EXP-037 |
| Window size must not change the guest video mode | EXP-037 |
| SDK overlays/hotkeys/achievement toast removed (they also forced continuous repaint) | EXP-039 |
| Window title: game name only (`GetWindowTitle()` override); VERSIONINFO in the .rc | kit |
| Release folder: exe, rexruntime.dll, app-local VC++ DLLs, cfg, data/ - nothing else; logging off in Release | EXP-037 |

## H. Tooling traps (all phases)

- **A flag given twice** on the command line makes the SDK parser drop **all** flags (runs then waited in the launcher). `bench/run.sh` adds defaults only when absent.
- The repo path may contain a space: pass dump dirs **relative to the exe dir** (`../../../../artifacts/...`).
- Never kill processes by image name or grab "the first window of class X": the user may be playing the release. `bench/run_safe.sh` and `tools/capture_window.py` only touch their own process.
- Windows Python does not see Git-Bash `/tmp`: use the session scratchpad or `cygpath -w`.
- Heredocs in the Bash tool can eat backslashes: write Python helper scripts to files for anything with `\`.
- `cp_busy_us == frame time` is misleading (includes blocking); use per-thread CPU (`tools/thread_cpu.py`).
- RTSS/overlay FPS counts host presents, not guest frames; use the guest swap counter / perf CSV.
- Frame-exact comparisons: compare by **guest swap number**, not time (the CP lags the CPU).
- Two path clusters can exist for the same scripted walk (different draw counts); compare within a cluster or use paired runs.
- A replaced (instead of merged) pipeline base dropped 175 -> 31 records: merge, never overwrite.
- Release builds force logging off unless `--log_level` differs from the default: pass `--log_level=debug` (not `info`) plus `--log_file=<name>` to get a log from a Release exe.
- `--dump_xex_image` output differs from `xex_decode.py` output only in the import table (the loader fills kernel variable imports, 39 bytes on Conan): code bytes are identical.
