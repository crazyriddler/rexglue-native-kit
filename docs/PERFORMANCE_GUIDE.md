# Performance guide

## Measure the right thing

- Scenario: scripted input (`--autoinput_script`, bench/scenario_*.txt), fixed save
  (bench/userdata_template copied fresh per run), `--bench_exit_after_s`, per-frame CSV
  (`--perf_log_csv`), window t=50..90 s. `tools/bench_summary.py <csv> <skip> <t0> <t1>`.
- Unlocked frame time: `--unlocked_vblank_rate=1000 --fps_limit=0 --vsync=false`.
  Guest-bound ceiling: legacy `--gpu_null_draws=true` (CP consumes PM4 but draws nothing).
- At a frame cap (handheld case): process CPU (`tools/power_probe.py`, also GPU power via
  nvidia-smi), per-thread CPU with names (`tools/thread_cpu.py`), RAM (`tools/mem_probe.ps1`),
  VRAM + upload peak (native "vram" log line).
- Profiles: `--sample_profile_out=<file>` (+ `--sample_profile_all_threads`), symbolize with
  `tools/symbolize_profile.py`, callers/callees `tools/profile_stacks.py`, active leaves per
  thread `tools/profile_threads.py`. Use the RelWithDebInfo build for symbols.
- GPU: `native: GPU time` log (timestamps) and `--native_gpu_pass_timing`.
- One-stop: `SCEN_A=... SCEN_B=... bench/opt_baseline.sh <tag>` (Release) -> artifacts/profiles/opt_<tag>.txt.
- Paired runs (A B A B) when the difference is < 5%; watch for path clusters.
- Handheld proxy: repeat the key scenario with the process limited to 6 logical CPUs
  (`cmd //c start "" /affinity 3F <exe> ...` from Git Bash); an optimization must not only win on a
  16-thread desktop (LostOdysseyRecomp "3C6T" method). Worker pools (e.g. PSO precompile)
  should size from the process affinity mask, not `hardware_concurrency()`.
- Judge a local speedup on the whole frame: LostOdysseyRecomp's 9-15x vertex-compare SIMD
  gave +0.6% fps because the GPU dominated.

## What cost what (Conan, RTX 4080 / 5800X3D)

| Stage | Frame (unlocked) |
|---|---|
| Legacy Xenos, before fixes | 23.3 ms (CP busy == frame, GPU 6.4 ms) |
| + present latency waitable (no queue lock across Present) | 9.5 ms |
| + SDK fixes (timer res, occlusion, clear_memory_page_state...) | 4.77 ms |
| Native, first full frame | 146 ms -> 768 (WC readback!) -> 20.8 -> 7.2 ms |
| + resolve texture keys, write watches, SIMD index scan | 5.57 ms |
| + recording worker thread (lag 1) | 4.22 ms |
| + no plugin, idle waits, name-lookup cache, x86-64-v3 | 4.03-4.11 ms (ceiling 3.97) |

At 30 fps: process CPU 141% of a core -> 45%; presents/s 300 -> 30; GPU ~21 W (idle
level). GPU per frame 1.3 ms at full clocks (all enhancements at 1080p: 2.3 ms).

## Checklist (in order of payoff seen)

1. Native renderer on a worker thread; guest threads only capture.
2. No PM4 translation, no Xenos plugin (sync-only consumer).
3. Buffers: persistent host copies, dirty ranges, upload only the bytes a draw reads.
4. Textures: decode once, write-watch revalidation, no per-frame hashing.
5. Constant buffers reused while the mirror's versions are unchanged; NaN scrub in SIMD.
6. Resolve textures keyed by size too (no create/release per resolve).
7. Busy-waits: BlockOnFence poll -> sleep until the fence; WAIT_REG_MEM blocking; SDK
   TimerQueue blocking strategy; guest `db16cyc` loops -> spin 2 ms then 250 us sleeps;
   no ImGui dialogs that force continuous repaint.
8. PSO precompilation (record + embedded base + startup workers). Coverage: record over many
   levels - community save packs are ideal (extract with tools/stfs_extract.py, one user data
   root per save as slot 1, load + walk/attack ~60 s each, merge).
   Better still, decode the game's save format (often plain: level name + spawn position +
   world-state blob) and generate a save per level start / savepoint (on Conan the
   savepoint positions came from the level world files). These two coverage runs were done
   after the reference EXPERIMENT_LOG was frozen at EXP-048: log them as new EXP entries. Walk forward AND backward
   (checkpoints can face walls) and avoid menu-navigating inputs after a possible death.
9. Memoize pure guest functions only if a profile shows them (per-thread cache, bounded).
10. `-march=x86-64-v3 -ffp-contract=off` + CPU check.
11. Remove per-draw debug (clock queries, logs, scans) from hot paths.

Traps: never read write-combined upload memory; sleeping thresholds below ~2 ms break
intra-frame handoffs; breadcrumbs (MARKER_OUT per draw) off by default (cost on AMD);
never leave write watches on hot guest pages.

## Where is the frame bound? (classify before optimizing)

Do this at the end of phase 3 (legacy) and again after phase 7 (native). Record the numbers
in BENCHMARKS.csv (`guest_ceiling_ms`, `worker_busy_pct`, `gpu_ms`).

| Measurement | How |
|---|---|
| Frame (unlocked) | `bench/run.sh ... --unlocked_vblank_rate=1000 --fps_limit=0 --vsync=false` + bench_summary |
| Guest ceiling | legacy `--gpu_null_draws=true` (same flags) |
| Worker busy | `tools/thread_cpu.py` (native_draw_thread) or profile slot 4 |
| GPU | `native: GPU time` log, `--native_gpu_pass_timing` |
| Render-thread split | `--sample_profile_out` + `tools/profile_threads.py`: game code vs XDK D3D range vs capture hooks |

| Result | Bound by | Optimize (in this order) |
|---|---|---|
| frame within ~5% of the guest ceiling | **guest code** (Conan: 4.03 vs 3.97 ms) | checklist items 7, 9, 10; then G14 native CRT, G1 codegen flags, G2 XDK D3D cost, G11 PGO, G3 occlusion |
| frame >> ceiling, worker busy ~ frame | **recording worker** | profile the worker; G6 vertex fetch in shader if SwapBufferRange/upload dominate; G7 texture decode off the worker; G5 parallel recording last |
| gpu_ms ~ frame | **GPU** | per-pass timing; G4 resolve elision; the pass's own cost (MSAA, shadow size, enhancement) |
| hitches only | **one-off work** | PSO (compiled-in-play counter -> G13), texture decode ms (G7), upload overflow pages, guest loads; Present blocks during loads (G12 present-thread priority) |
| CPU at a frame cap too high | **busy-waits** | checklist item 7, `tools/profile_threads.py` active leaves per thread |

Renderer-side optimizations on a guest-bound game do not move the frame: do not start them.

## Gated optimizations (second generation)

Each item names the metric that must show the problem first, the expected gain and the
exactness risk. Measure before/after with paired runs; A/B at scale 1 must stay unchanged.
Evaluation of where these come from: docs/STRATEGY_REVIEW.md.

| ID | Optimization | Gate (start only if...) | Risk / validation |
|---|---|---|---|
| G1 | Codegen register locality (UnleashedRecomp ships all on, The Darkness all off as a correctness profile): manifest `ctr_as_local`, `xer_as_local`, `reserved_as_local`, `cr_as_local`, `non_argument_as_local`, `non_volatile_as_local`, `skip_msr`; `skip_lr` last (`sdk/src/codegen/config.cpp`) | guest-bound (the usual case); preferred window: end of phase 3 (playbook) | Enable one flag per codegen+build; A/B scenarios, long session, repro_freeze. `skip_lr` breaks hooks that read `ctx.lr` (E026). setjmp/longjmp handled by codegen, still test save/load and FMV |
| G2 | Cut guest XDK D3D work (the originals the hooks call) | XDK D3D range > ~15% of render-thread samples | Dirty-mask constant path (skate3 `SetPending_*`), or replace XDK functions whose PM4 the mirror does not need; feed fences per Q-R5. Full A/B |
| G3 | Native occlusion queries, results one frame late | capture shows occlusion queries AND native draws/frame > legacy draws/frame | Game-visible (it changes what the game submits); compare draw counts and A/B |
| G4 | Resolve elision (sample the host RT instead of copying) | GPU-bound or `resolve_copy_mb` large at 4K / handheld | Only full-surface, same-format, non-MSAA resolves whose source is not re-rendered before the read; keep the copy path as fallback cvar |
| G5 | Parallel recording by pass / ExecuteIndirect batches | worker busy > ~80% of the frame and on the critical path | Keep submission order; never reorder draws |
| G6 | Vertex fetch in shader from raw big-endian buffers (no IA, no CPU byte swap, decl out of the PSO key) | SwapBufferRange + uploads > ~15% of worker time, or PSO count dominated by decl permutations, or a vertex format the IA cannot express | XenosRecomp change for every VS; re-run the shader harness + all A/B scenarios |
| G7 | Texture decode off the worker (job pool + copy queue, or compute untile) | `tex_decode_ms` spikes coincide with hitches | Keep content-hash/write-watch semantics; a texture not ready yet must wait, never be skipped |
| G8 | `ID3D12PipelineLibrary` on top of the PSO records | cold precompile time > ~10 s or driver cache evictions observed | Invalidate on driver/adapter change; records stay the source of truth |
| G9 | Memory for handhelds: upload ring sized from the measured peak, placed heaps, residency budget | VRAM or sysmem > ~70% of a 6 GB budget | Overflow pages already exist, so a smaller ring is safe; measure `upload_peak_mb` over all scenarios |
| G10 | 2 frames in flight instead of 3 (latency) | measured input latency is a complaint | Check unlocked frame time and hitches do not regress |
| G11 | clang PGO on the recompiled code | guest-bound, after G1 | Keep `-ffp-contract=off`; profile on several scenarios, not one; paired runs. Darkness found ThinLTO gave nothing: measure, do not assume |
| G12 | Host thread placement: guest hardware threads on distinct physical cores (CPU Sets), present thread ABOVE_NORMAL | Present blocks / hitches at loads with cores saturated, or a guest worker sharing an SMT sibling with the engine thread | Scheduling only, guest semantics unchanged; Darkness: priority fixed 1.3 s Present stalls, core mapping gave no gain |
| G13 | Load-time PSO precompile: at the CreateShader hook, enqueue recorded PSOs using that shader at high priority; optionally hold the game's loading flag until they finish (UnleashedRecomp) | cold precompile at startup too long (thousands of PSOs) or PSOs compiled in play after a level load | Never skip a draw; holding a loading flag needs the engine's flag found per game |
| G14 | Native CRT: `[rexcrt]` for memcpy/memset/str*/wcs*/XMemCpy (then Rtl*Heap group + `rexcrt_heap_enable`, file I/O) | guest-bound and CRT functions visible in the render/game thread profile (expected: recompiled PPC memcpy/memset loops are much slower than host ones; measure the share first); preferred window: end of phase 3 (playbook) | Codegen change; verify each address (xdk_layout.py or semantics), A/B + long session + save/load; heap group last (allocator change: watch RAM) |

Rejected outright (see STRATEGY_REVIEW.md): reordering/sorting draws, GPU culling of draws
the game submitted, placeholder PSOs, mapping states to "nearest canonical" PSOs, physical
EDRAM aliasing, FSR/CAS/TAA-style upscalers (user policy).

## Memory targets (handheld: 6 GB RAM / 6 GB VRAM)

Conan: RAM 1.6 GB, VRAM 0.36 GB + 0.44 GB upload heaps (128 MB x 3 frames); upload peak
59 MB; no texture eviction needed. Ultra shadows (4096 -> 16384x8192 R32F atlas) cost
512 MB: keep it an option, default original.
