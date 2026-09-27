# SDK patches

## 0001-crash-diagnostics.patch

**What:** two small additive diagnostic patches to `rexglue-sdk` (applied directly to the checkout at `C:\Users\jrbar\rexglue-sdk`, not yet upstreamed anywhere):

1. `src/system/xmemory.cpp` (`Memory::AccessViolationCallback`) — on an unhandled guest access violation, dumps the full `PPCContext` (every GPR, LR, CTR, `last_indirect_target`) via `rex::runtime::ThreadState::Get()`. Safe because the vectored exception handler runs on the faulting thread itself, so its thread-local `PPCContext` is still valid.
2. `src/core/exception_handler_win.cpp` (`ExceptionHandlerCallback`) — when no registered handler manages an exception, resolves the host RIP (already captured, previously unused after that point) to a symbol name + source line via `dbghelp.h` (`SymFromAddr`/`SymGetLineFromAddr64`), reading the PDB the `RelWithDebInfo` build already produces.

**Why an SDK patch and not a project-level fix:** there is no debugger installed on this machine (checked for `cdb`/`windbg`, both absent — see `docs/error_log.md` E010's investigation) and no other way to get more than a target-address-only `[FATAL]`/access-violation log line. This is exactly the kind of "runtime diagnosis" CLAUDE.md Phase 12/13 calls for; without it, `error_log.md` E016 would have been unresolvable without guessing.

**Why it's safe to keep:** purely additive logging on paths that are *already* about to report an error and continue toward `EXCEPTION_CONTINUE_SEARCH`/`return false` — no behavior changes on any success path, no perf cost outside an actual crash. `SymInitialize` is guarded by a `static bool` so it only runs once per process.

**Verification:** used to root-cause `docs/error_log.md` E016 — the symbol/line resolution pointed to `generated/default/conan_recomp.41.cpp:17013` inside `sub_82599B40`, which led directly to the actual fix (see E016's writeup). Re-running after the E016 fix should still print this diagnostic if any *future* unhandled access violation occurs, which is the point of keeping it.

**Reproduce:** `git apply patches/sdk/0001-crash-diagnostics.patch` from `C:\Users\jrbar\rexglue-sdk` on a clean `v0.10.0` checkout, or just read the diff and apply by hand — it's two small, self-contained hunks.
