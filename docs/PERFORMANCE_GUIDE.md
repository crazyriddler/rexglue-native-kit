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
   root per save as slot 1, load + walk/attack ~60 s each, merge; see reference EXP-049).
   Better still, decode the game's save format (often plain: level name + spawn position +
   world-state blob) and generate a save per level start / savepoint (reference EXP-050:
   savepoint positions came from the level world files). Walk forward AND backward
   (checkpoints can face walls) and avoid menu-navigating inputs after a possible death.
9. Memoize pure guest functions only if a profile shows them (per-thread cache, bounded).
10. `-march=x86-64-v3 -ffp-contract=off` + CPU check.
11. Remove per-draw debug (clock queries, logs, scans) from hot paths.

Traps: never read write-combined upload memory; sleeping thresholds below ~2 ms break
intra-frame handoffs; breadcrumbs (MARKER_OUT per draw) off by default (cost on AMD);
never leave write watches on hot guest pages.

## Memory targets (handheld: 6 GB RAM / 6 GB VRAM)

Conan: RAM 1.6 GB, VRAM 0.36 GB + 0.44 GB upload heaps (128 MB x 3 frames); upload peak
59 MB; no texture eviction needed. Ultra shadows (4096 -> 16384x8192 R32F atlas) cost
512 MB: keep it an option, default original.
