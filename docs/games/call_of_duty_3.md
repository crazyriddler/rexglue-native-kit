# Call of Duty 3 (Xbox 360): notes for a port

Read this before phase 0 if `game/` holds Call of Duty 3. Sources: the kit author's handover of
an earlier attempt with ReXGlue **0.8.1** (`v0.8.1.68-dev.g8dadea6`, clang 19, plugin xenos) -
marked **[0.8.1]** - and the kit SDK fork (0.10) behaviour checked in `sdk/` on 2026-10-01 -
marked **[kit]**. Record confirmed answers in `docs/PROJECT_STATE.md` "Game facts".

## 1. Facts

| Fact | Value |
|---|---|
| Title ID / Media ID (every XEX) | `415607E1` / `720B9353` [0.8.1] |
| Executables | `default.xex` (single player, base `0x82000000`), `codmp_xenonf.xex` (**multiplayer executable**: `module_flags = 0x01`, base `0x82000000`; NOT a module of the campaign) |
| Level code | One guest DLL per campaign level, `sp\<level>\<level>.dll`, loaded when the mission starts through `XexLoadImage("game:\sp\<level>\<level>.dll")` |
| Levels (15) | `blkbrn, chambois, credits, crssrds, falaise, forest, fuelplnt, hostage, island, laison, mace2, mayenne, nightd, saint_lo, stbert` |
| `saint_lo.dll` header | `module_flags = 0x09` (TITLE + DLL), image `0x89000000-0x89260000`, code `0x89030000-0x8924287C`, entry `0x892347A8`, compression BASIC, encrypted; 6071 functions |
| Dump sanity | The game runs fully in Xenia (the files are good) |
| Main XEX functions analysis missed [0.8.1] | `0x822D2140, 0x822D0498, 0x822C27F8, 0x822CAB78, 0x8216D720, 0x822D2508` (`[entrypoint.functions] 0xADDR = {}`; re-check with the kit SDK, its analysis differs) |

## 2. State reached [0.8.1]

Boots, menus, new game, intro video (full graphics) and the loading screen work. **Crash ~1 s
after `saint_lo.dll` registers its functions**, before any level graphics or sound. Before the
module was declared, the crash was `Execute(892347A8): function not in function table` /
`[FATAL] Call to invalid or unregistered function at guest address 0x890985E0`: the level code
did not exist in the recompiled main XEX.

## 3. Level DLLs as `[[modules]]`

Working configuration [0.8.1], with the kit differences:

```toml
[[modules]]
guest_path = "sp/saint_lo/saint_lo.dll"          # normalized form of game:\sp\saint_lo\saint_lo.dll
file_path = "../cod3/sp/saint_lo/saint_lo.dll"
out_directory_path = "generated/saint_lo"
includes = []
is_dll = true                                    # needed on 0.8.1; default for modules on the kit SDK
```

- **[kit]** Phase 0 `rexglue init ... --game-root game --scan-dll` scans the game root
  recursively and writes one such entry per `.dll` (guest path = path under the game root,
  i.e. `sp/<level>/<level>.dll`): all 15 levels at once. Modules default to `is_dll = true`
  (`project_recompiler.cpp`), so no manual edit.
- `rexglue init module` needs the parent `init` options too (both versions):
  `rexglue init --project-name cod3 --xex-path game/default.xex module --project-root . --xex-path game/sp/<level>/<level>.dll --guest-path "game:\sp\<level>\<level>.dll"`.
- One joint `rexglue codegen` writes `generated/default/` and one `generated/<level>/` per
  module, the `<project>_<level>` SHARED targets (`dll_targets.cmake`, included by
  `generated/rexglue.cmake`) and the module registry. No manual registration in the app,
  no hand-written dll_targets, no forced `LoadUserModule`: the 0.8.1 attempt tried all of those
  and they were unnecessary (same crash either way).
- Expected startup log when the module is wired:
  `Registered recompiled module: pe='saint_lo.dll' guest='sp/saint_lo/saint_lo.dll' lib='cod3_saint_lo'`,
  then at mission start `Function table initialized for module: code=89030000-8924287C, image=89000000-89260000`
  and `Module 'sp/saint_lo/saint_lo.dll' registered 6071 functions`.
- Bring one level up first (saint_lo); the other 14 only after it plays.
- Release: the module DLLs `<GAME_NAME>_<level>.dll` ship next to the exe
  (`tools/make_release.sh` copies `<GAME_NAME>_*.dll`).

**[kit] How loading works and what each failure looks like** (`sdk/src/system/kernel_state.cpp`
`LoadUserModule`, `xboxkrnl_modules.cpp` `XexLoadImage`): the requested name is resolved
(bare names next to the executable), the XEX is loaded from the VFS, its normalized guest path
is compared with each registered `guest_path`, the host DLL is loaded and its image range
checked, functions are registered, then guest `DllMain(DLL_PROCESS_ATTACH)` runs.

| Log / symptom | Meaning | Fix |
|---|---|---|
| `Call to invalid or unregistered function` at an address inside the DLL image, no load error | No `[[modules]]` entry, or its `guest_path` differs from the requested path: loaded as a plain XEX without recompiled code | Set `guest_path` to the requested path (strings in the image, `LoadUserModule` log) |
| `Recompiled module '...' layout does not match loaded XEX` | The DLL on disk differs from the one codegen used (revision, TU) | Codegen from the file the game loads |
| `Failed to load shared library` / `ReXModule_Register not found` | Host module DLL missing next to the exe, or stale | Build all targets; release copies `<GAME_NAME>_*.dll` |
| `RegisterRecompiledModule: duplicate guest_path` | Two entries normalize to the same path | One entry per DLL |
| `DllMain(DLL_PROCESS_ATTACH) skipped ...` / `returned FALSE ...; rolling back load` | Load from a host thread / the DLL's own init failed | Read the preceding log lines |
| `XexGetProcedureAddress ... not found` | An import between modules is unresolved; the game stores a null pointer | Check the export table in the module dump |
| Codegen: `Module '...' [..) overlaps '...'` | Two images share an address range (e.g. `codmp_xenonf.xex` + `default.xex`, both at `0x82000000`) | Only DLLs that load into the same process are modules; another executable is a separate port |

## 4. The open crash: `r30 == 0` in `sub_89132580` [0.8.1]

Verified with WinDbg on 0.8.1:
- `0xC0000005` in `cod3_saint_lo.dll` at offset `0x553987`, deterministic (same with the module
  isolated or integrated). WinDbg names it `cod3_saint_lo!ReXModule_Register+0x552987` because the
  DLL exports no other symbol: use the PDB of a RelWithDebInfo build, not that name.
- Host instruction `mov eax, dword ptr [rcx+rax]`, `rcx = 0`, `rax = 0x100005104`, membase
  `0x100000000` -> guest address `0x5104`: `ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 20740)`
  in `sub_89132580` (`generated/saint_lo/cod3_recomp.7.cpp`).
- The prologue sets `lis r30,-30427` (`r30 = 0x89250000`, an address in the module's data);
  `r30` is already 0 at the call to `sub_891BE760` (`+0x55296a`). Something between the
  prologue and that call zeroes it: one of `sub_8916C370, sub_890C0408, sub_890CBA28,
  sub_890C22E0, sub_8917E6D8, sub_89099818` or an indirect `bctrl`.
- Checked and correct: `__savegprlr_24` / `__restgprlr_24` are symmetric (r30 at `[r1-24]`);
  `sub_891BE760` saves r30/r31 before `stwu r1,-112(r1)`; the recompiled code matches the
  original; the module image `0x89000000-0x89260000` is mapped.

Hypotheses, unverified (rank by what is cheapest to test):
1. **Re-run on the kit SDK first.** 0.10 differs from 0.8.1 in analysis and codegen (the kit
   fork also fixes `vpkd3d128`, `sraw/srad` and more); the crash may change or vanish.
2. **setjmp/longjmp not mapped** [kit]: the 0.8.1 manifest has no `setjmp_address` /
   `longjmp_address` for either image. The id Tech-derived CoD engine uses setjmp/longjmp for
   error recovery, and a mis-emulated `longjmp` returns with non-volatile registers (r14-r31)
   from a wrong or empty buffer. Find both in each image (ANY_GAME_CHECKLIST §1), set them per
   module, regenerate.
3. A callee restores r30 from a stack slot that was never written (wrong function boundary in
   the module, or a callee entered past its prologue).
4. An indirect call crossing modules lands in a trap or another module's code with a stale
   per-thread context.

Method: a hardware write watchpoint on the context slot of the register. `PPCContext`
(`sdk/include/rex/ppc/context.h`) is `r3, r0, r1, r2, r4 ... r31` at 8 bytes each, then `lr`:
**`r30` = context + 0xF0**, `r1` = +0x10. Under WinDbg: break at the prologue of
`sub_89132580` (after `lis r30`, the slot holds `0x89250000`), find the register that holds the
context pointer there (it was `rdi`), `ba w8 @rdi+0xf0`, continue, and at each hit read `k`/`r`
and the generated code of the writer. Use `sxi av` (first-chance AVs from the SDK GPU
write-watch otherwise stop the debugger). Also check `r1` (+0x10) and the stack slots at the
crash. Fix the producer of the 0 (CLAUDE.md rule 3), then record it as an E-number.

## 5. SDK patches tried on 0.8.1: none belongs in the kit

| Patch | Result |
|---|---|
| `xex_module.cpp` `ReadImageBasicCompressed`: grow `total_size` to `image_size` (+ a log) | No effect (log showed `total_size == image_size == 0x260000`): the module's .bss was already mapped. Reverted |
| `function_dispatcher.cpp` `Execute`: remove the `64 + 112` stack padding | Never verified; the padding exists on purpose (games overwrite the caller frame by 16-32 bytes) and the kit SDK keeps it. Do not repeat |

## 6. False leads (do not repeat)

- `codmp_xenonf.xex` is the multiplayer executable, not the campaign module (overlapping base).
- "`thread_local` disassembler bug": the garbage addresses came from the codmp base overlap.
- Isolated module + manual registration/forced loading: unnecessary, same crash.
- Unmapped module .bss: disproved at run time (above).
- `0x890985E0` / `0x892347A8` as embedded pointers in the main XEX: they are the module's own
  code / entry point.

## 7. Expected log noise [0.8.1]

`NtCreateFile FAILED path='d:\_english\'` (language folders), `d:\config\*.cfg`,
`D:\movies\*.wma`, `d:\hunkusage.dat`; many `Completed delivery of APC to 82345508`;
first-chance AVs writing to `0xA8xxxxxx` (SDK GPU write-watch). None of them is the problem.

## 8. Tooling notes

- Log level: cvar `--log_level=trace` (underscore; `--log-level` is not a cvar) or the
  environment variable `REX_LOG_LEVEL=trace`. Trace logs are mostly GPU lines: filter out
  `gpu]`, `Resolve:`, `APC to`.
- `llvm-objdump -d --start-address=... --stop-address=... cod3_saint_lo.dll` disassembles the
  host code around a crash offset (host image base `0x180000000`).
- The 0.8.1 attempt consumed a prebuilt SDK install; the kit builds the SDK as a subdirectory
  with `-march=x86-64-v3` (LESSONS A covers the SSSE3 error that appears without it).
