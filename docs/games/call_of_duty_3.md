# Call of Duty 3 (Xbox 360): notes for a port

Read this before phase 0 if `game/` holds Call of Duty 3. Source: notes the kit author kept
from an earlier ReXGlue attempt (reconstructed summary, no code, manifests or logs kept) plus
the SDK behaviour verified in the kit fork (2026-10-01). Facts from the old attempt are marked
**[old notes]**; everything else was checked in `sdk/`. Replace guesses with evidence as the
port progresses and record the answers in `docs/PROJECT_STATE.md` "Game facts".

## State reached by the old attempt [old notes]

- Boots to the main menu.
- **Crashes when a campaign level loads.** Unsolved: the last open lead was a guest access
  violation with `r30 == 0` right before a dereference (cause never identified).
- Level code is not in `default.xex`: the game loads a level DLL at run time through the kernel
  call `XexLoadImage` (`saint_lo.dll` for the Saint-Lô level; check `game/` for the other
  levels' `.dll` files - every one needs the same treatment).
- The old attempt added "`[[modules]]` support" and fixed "XexLoadImage registration issues"
  (exact syntax, hook and root cause not kept).

## Level DLLs: what the kit SDK already does (no SDK extension needed)

ReXGlue 0.10 (the kit fork) supports runtime-loaded modules natively; the old notes called it
unofficial, it is not:

1. **Manifest.** Each DLL is a `[[modules]]` entry (`guest_path`, `file_path`,
   `out_directory_path`) with its own codegen output and function config. Create them in phase 0:
   `rexglue init ... --scan-dll` adds every `.dll` under the game root (guest path derived from
   its location), or per file:
   `rexglue.exe init module --project-root . --xex-path game/<dir>/saint_lo.dll --guest-path "<path the game passes>"`.
2. **Build.** Every module becomes a host DLL `<GAME_NAME>_<module>.dll` next to the exe,
   exporting `ReXModule_Register` / `ReXModule_GetImageInfo`; the generated module registry
   calls `KernelState::RegisterRecompiledModule(pe, guest_path, lib)` at startup (debug log:
   `Registered recompiled module: pe=... guest=... lib=...`).
3. **Run time.** `XexLoadImage(name)` -> `KernelState::LoadUserModule`
   (`sdk/src/system/kernel_state.cpp`): a bare file name is resolved next to the executable's
   guest path; the XEX is loaded from the VFS; `FindRecompiledModule(path)` compares the
   **normalized guest path** with the registered `guest_path`; on a match the host DLL is
   loaded, its image range checked against the XEX and its functions registered; then guest
   `DllMain(DLL_PROCESS_ATTACH)` runs.

The "registration issues" class is therefore almost certainly one of these, each with its own
log line:

| Log / symptom | Meaning | Fix |
|---|---|---|
| No error at load, then `Call to invalid or unregistered function` at an address inside the DLL's image | `guest_path` in the manifest does not match the path the game passes (normalized): the DLL was loaded as a plain XEX without recompiled code | Find the exact string (`strings` on `port/logs/default_image.bin` for `.dll`, or the `LoadUserModule` / module dump log lines) and set `guest_path` to it |
| `Recompiled module '...' layout does not match loaded XEX` | The DLL on disk is not the one codegen used (other revision, TU-patched copy) | Codegen from the same file the game loads; a TU may patch DLLs too (ANY_GAME_CHECKLIST §1) |
| `Failed to load shared library` / `ReXModule_Register not found` | The host module DLL is not next to the exe (or a stale one) | Build all targets; the release copies `<GAME_NAME>_*.dll` (`tools/make_release.sh`) |
| `RegisterRecompiledModule: duplicate guest_path` | Two manifest entries normalize to the same path | One entry per DLL |
| `DllMain(DLL_PROCESS_ATTACH) skipped ...: not on a guest thread` | Load requested from a host thread | Load must come from guest code; investigate who calls |
| `DllMain(DLL_PROCESS_ATTACH) returned FALSE ...; rolling back load` | The DLL's own init failed (often a missing import/export or file) | Read the log lines before it |
| `XexGetProcedureAddress ... not found` | The main XEX asks the DLL for an export it cannot resolve; the game stores a null pointer | Check the DLL's export table in the module dump; ordinal vs name |

Each module is analysed on its own: run the phase-1 loop (codegen errors, switch tables,
`find_cross_file_gotos.py`) per module, and the unregistered-function scanners on the DLL's
disassembly too (`scripts/port/find_adjustor_thunks.py <dll.dis> <module register.cpp>`;
`find_unregistered_after_bctr.py` has its two paths at the top).

## The open `r30 == 0` crash: how to attack it

Hypotheses (unverified, ranked by how often they explain this symptom in recompiled ports):

1. The DLL's initialization did not run or failed (DllMain / CRT static init of the DLL):
   globals the level code reads stay 0. Check the `DllMain` log lines above first.
2. A null pointer returned earlier: an unresolved `XexGetProcedureAddress`, a missing level
   asset (LESSONS C: loaders leave tables uninitialized), or a module function the analyser
   missed (its caller got garbage). Check the log for WARN lines before the crash.
3. A wrong function boundary in the DLL's codegen, so the code that loads `r30` (typically
   `mr r30,r3` in the prologue, saving `this`) is not where the crashing code expects it.
4. A hook/override that does not preserve non-volatile registers (`r14`-`r31`).

Method (LESSONS_LEARNED C, playbook phase 2): take the faulting guest address from the log,
find its function in the module's generated code, find where `r30` is set (prologue copy of an
argument, or a load from a global), and walk back to the producer of the zero with a midasm
log hook or the debugger (`windbg-tool`; ignore first-chance AVs from the SDK write-watch).
Fix the producer (missing file, export, boundary, init), never guard the dereference unless
the function has its own bail-out (CLAUDE.md rule 3).

## Record as soon as known

Title ID, SHA-256 of `default.xex` and of every level DLL (+ TU), the `[[modules]]` block, each
DLL's guest path as the game requests it, per-module manifest overrides, and the root cause of
the `r30` crash (E-number in `port/docs/error_log.md`, summary line here).
