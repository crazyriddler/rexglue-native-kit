---
name: boot-and-crash-triage
description: Phase 2: build, boot and fix runtime crashes/hangs until menus, gameplay, save/load and FMV work on the legacy path.
---
# Boot and crash triage

1. CMakeLists/app setup like reference/conan/port (GPU_PLUGINS xenos, imgui link, data discovery, gpu_plugin default, window title). Configure nr + nr-rel (docs/TOOLCHAIN_SETUP.md); build with bench/build.sh.
2. Run with --mnk_mode (+ --autoinput_script for automation); read port/out/build/nr/logs.
3. Classify each failure with docs/LESSONS_LEARNED.md section C:
   - `[FATAL] Call to invalid or unregistered function` -> verify code in the disassembly, add a boundary override (register a whole visible thunk family at once);
   - guest AV -> symbolize, read generated code, supply missing assets or guard to the function's own bail-out (never to a loop head);
   - SDK native crash -> real fix in sdk/, documented;
   - hang -> hang watchdog / ProcDump + windbg-tool host stacks; intermittent -> bench/repro_freeze.sh;
   - timing-dependent crash/glitch -> first classify with `--ignore_thread_priorities=false --ignore_thread_affinities=false` (ReXGlue ignores guest priorities/affinities by default); a submit-order race gets a fix at its submitter, not only a guard (LESSONS_LEARNED C).
4. One fix per run; record in port/docs/error_log.md.
5. Build scripted scenarios (bench/scenario_*.txt) and the bench save in bench/userdata_template.
