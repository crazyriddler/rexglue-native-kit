# This SDK: ReXGlue v0.10.0 fork used by the native-port kit

Source: `C:\Users\jrbar\rexglue-sdk native-render`, branch `native-render`, commit
d859ff6 (2026-09-26), on top of ReXGlue v0.10.0 (upstream `main` `c94f5eb` = tag `v0.10.0` `f5337cd` + 2 commits; the original clone at
`C:\Users\jrbar\rexglue-sdk` is untouched and must stay so). Submodules are vendored
(no .git); the libmspack/moltenvk/o1heap symlink placeholders are already fixed.

Record every further SDK change here (what, why, which experiment).

## Changes relative to upstream v0.10.0 (by area)

Runtime / performance
- High-resolution timing: `timeBeginPeriod(1)`, waitable-timer sleeps, vblank worker that
  waits out the interval and delivers at most one vblank per wake (E049).
- TimerQueue uses a blocking wait strategy (was spinning ~18% of a core); the vendored
  disruptorplus `wait_for`/`wait_until` argument order fixed (EXP-040). Kept as
  `sdk/patches/thirdparty/0001-disruptorplus-wait-arg-order.patch` because `thirdparty/*/`
  is not in git; `scripts/restore_sdk_thirdparty.sh` re-applies it on a fresh clone.
- Occlusion queries resolved one frame late, never waiting on the GPU (E050).
- `clear_memory_page_state` default false (E051); `readback_memexport` default false;
  primitive processor cache min 4096 indices; quad lists as triangle lists (E048).
- `vsync` / `unlocked_vblank_rate` cvars live in rexruntime (`graphics_flags.h`) so they
  exist without the GPU plugin (EXP-036).
- Present through a frame-latency waitable swap chain (no queue lock across Present,
  EXP-007); Present(1) without tearing when vsync is on; presents/s log (EXP-037/039).
- Window size no longer changes the guest video mode (EXP-037). Logging "off" creates
  no files.
- Shader pipelines: draws wait for pending pipeline creation (cold-cache black scene,
  EXP-005).

Audio
- `UnregisterClient` waits for the client's in-flight callback (level-load hang E036);
  SubmitFrame drops a frame instead of dereferencing a null driver (E034).
  Debug cvars `audio_debug_callback_delay_ms`, `audio_unregister_wait`.

Graphics interfaces
- `IGraphicsSystem::GetGammaRamp256` (native present applies the DC_LUT ramp).
- GPU swap callback with register file, `bench_screenshot_swaps` (frame-exact A/B).
- Xenos path: NaN sanitize of constants in cached staging (E041), `gpu_null_draws`.

UI / app
- `ReXApp::GetWindowTitle()` virtual (default: name + build stamp).
- `ImGuiDialog::WantsContinuousRepaint()`: idle dialogs no longer force continuous repaint.

Benchmark / diagnostics
- `autoinput_script` (buttons, sticks, **LT/RT** added in the kit), `bench_exit_after_s`,
  `bench_screenshot_times/swaps/dir`, `perf_log_csv`, perf counters in Release, bench exit
  callbacks.
- In-process sampling profiler (`sample_profile_out`, full stacks + module map,
  `sample_profile_all_threads`).
- Thread CPU accounting, WAIT_REG_MEM timing, `gpu_trace_register`.

Codegen correctness
- `sraw` / `srad` (src/codegen/builders/logical.cpp): XER.CA is now set for a negative source
  when the shift count is >= 32 / >= 64 (every bit shifted out). The clamped comparison
  alone cleared CA for 0x80000000 / 0x8000000000000000. Same defect LostOdysseyRecomp fixed
  in XenonRecomp (docs/UPSTREAM_RESEARCH.md, Lost Odyssey). Test:
  `python -m pytest scripts/tests/test_codegen_sra.py` (renders the emitted strings, compares
  with the ISA; fails on the old code). Needs a codegen re-run of a port to take effect;
  expected impact is rare (CA consumed after a variable arithmetic shift of that value).

Tests (SDK self-test, docs/VALIDATION_GUIDE.md "SDK self-tests")
- `tests/ppc/asm/instr_sraw.s`, `instr_srad.s`: sign-bit-only cases (shift 31/32/63, 63/64)
  for the carry fix; they fail on the old builder (verified 2026-09-27).
- `tests/unit/core/timer_queue_test.cpp` (new): one-shot and recurring timers, and idle CPU
  of a waiting queue (< 3 ms per 500 ms; blocking strategy ~0.1 ms, upstream spin ~12.5-14.6 ms
  on Linux; fails on the upstream timer_queue.cpp).
- `tests/unit/CMakeLists.txt`: link `xxHash::xxhash` (hash_test.cpp did not compile upstream).
- `cmake/ppc_test_pipeline.cmake`: falls back to the kit's `tools/binutils` when `sdk/tools`
  does not exist.
- Known upstream failures on Linux (files identical to upstream `c94f5eb`): `chrono_test.cpp`
  FILETIME 1601-epoch cases and `output_stamp_test.cpp` escaping of paths with spaces/`#`.

Kit-only changes (not in the source branch)
- `thirdparty/CMakeLists.txt`: accepts vendored submodule content without `.git`.
- `src/ui/rex_app.cpp`: `--dump_xex_image=<file>` writes the loaded (decrypted,
  decompressed) executable image, any XEX compression.
- `src/input/input_system.cpp`: autoinput `LT` / `RT` triggers.

## Building
- As part of a game project (REXSDK_DIR mode): outputs in `sdk/out/win-amd64/`.
- Standalone CLI: `cmake --preset win-amd64 && cmake --build out/build/win-amd64
  --config Release --target rexglue` -> `out/win-amd64/Release/rexglue.exe`
  (after `source ../scripts/dev_env.sh`).
