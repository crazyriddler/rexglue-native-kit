# Strategy review: a "second-generation" DX12 backend proposal vs the measured Conan port

> Written 2026-09-27. Input: an external AI's "second-generation DX12 backend" proposal
> (a temporary `informe.md`, since removed from the repo; the § numbers in the table below
> refer to its sections, and each row restates the proposal so the table stands alone)
> and the whole kit (docs, reference renderer, experiment log, SDK fork).
> Output: what to adopt, what to adopt only behind a measurement gate, what to reject and
> why, plus new items and inconsistencies found in the kit. The adopted items are already
> merged into PERFORMANCE_GUIDE.md §"Where is the frame bound?" / §"Gated optimizations",
> NATIVE_RENDERER_ARCHITECTURE.md §7-8, VALIDATION_GUIDE.md and the playbook.

## 1. The core disagreement: which bottleneck the proposal optimizes

The proposal assumes a "first-generation" renderer that is **API/CPU-submission bound and
GPU bandwidth bound** and designs against it (GPU-driven rendering, ExecuteIndirect,
compute culling, HZB, parallel recording, resource aliasing, residency LRU). The Conan
measurements say otherwise (EXP-030, EXP-040, EXP-047; RTX 4080 / 5800X3D, jungle):

| Quantity | Value | Consequence |
|---|---|---|
| Native unlocked frame | 4.03-4.11 ms | |
| Guest-only ceiling (null draws, no renderer at all) | 3.97 ms | the renderer costs **~0.1 ms** of critical path |
| D3D12 recording worker | ~34% busy, off the critical path (lag 1) | submission is not the limit |
| GPU per frame | 1.3 ms (2.3 ms with every enhancement at 1080p) | GPU is idle >60% even unlocked |
| Guest draws / resolves per frame | 616 / 29 | nowhere near "2,000-5,000 draws" |
| VRAM | 0.36 GB | residency management has nothing to manage |

So for an XDK-era 360 title the frame is **bound by the recompiled guest code** (game logic
+ render thread + XDK D3D library), not by D3D12. Every millisecond left to win is on the
guest CPU side, and the proposal does not address it at all. The most important addition
to the workflow is therefore a **mandatory bound classification** before any optimization
(PERFORMANCE_GUIDE.md §"Where is the frame bound?"), and optimizations gated by the
metric they move.

A second structural misreading: the proposal describes the renderer as a "Xenos command
translator (ring buffer -> IR)". The kit does **not** translate PM4: it hooks ~25 XDK D3D
entry points, reads the guest D3DDevice shadow, and only mirrors PM4 for the constants
that bypass the shadow. Its "record the frame as POD items, then emit" is what `WorkCmd`
batches + the recording worker already do (EXP-030).

## 2. Point-by-point verdict

Legend: **Adopt** = do it in every port; **Gated** = only when the named metric shows the
problem (see PERFORMANCE_GUIDE.md §"Gated optimizations"); **Reject** = conflicts with
correctness or with evidence; **Done** = the kit already does it (sometimes better).

| § | Proposal | Verdict | Reason / evidence |
|---|---|---|---|
| 1.1 | Record frame into POD IR, post-process, then emit | **Done** | WorkCmd capture on guest threads, batches of 32, worker records (EXP-030) |
| 1.1 / 4.1.2 | Sort the frame by RT -> PSO -> material | **Reject** | Xenos order is semantic: EDRAM aliasing by base address, resolves read what was drawn *so far*, blending, clears limited to rects, stencil sequences. Reordering breaks frame-exact A/B for a gain that does not exist at 616 draws. See ARCHITECTURE §7 |
| 2 | Zero allocations / arenas in hot path | **Done / partial** | Capture arena (`WorkBatch::bytes`, EXP-047), upload ring, thread_local scratch. Remaining `std::vector` temporaries in rect-list expansion etc. are cold paths |
| 2 | Parallel command-list recording by pass | **Gated** (G5) | Worker is 34% busy and off the critical path. Only if a game's worker becomes the critical path (worker busy > ~80% of the frame) |
| 3 "conan_pipelines.bin is fragile / tied to driver" | - | **Wrong premise** | The file holds pointer-free PSO *descriptions* (inputs), machine-independent; the driver's own cache holds the blobs. That is exactly the proposal's "layer 2" |
| 3.1 L1 | `ID3D12PipelineLibrary` driver blobs on disk | **Gated** (G8) | Warm driver cache already gives 0.05 ms median creation (EXP-044). Worth it only if cold-start precompile time (1.6 s for 121 PSOs) grows large for a game with thousands of PSOs, or driver caches are observed to be evicted |
| 3.1 L3 | Parallel startup precompile | **Done** | 1-4 below-normal threads, embedded base + local file (EXP-044) |
| 3.1 L4 | Placeholder PSO, never block the frame | **Reject** | EXP-005: skipping or substituting a draw once can break the game permanently (exposure init feedback draw -> black scene). A wrong PSO for one frame is also a visible glitch. Correct answer: coverage (save packs and generated saves, PERFORMANCE_GUIDE checklist item 8) + a **release gate "0 PSOs compiled during play"** (VALIDATION_GUIDE) |
| 3.2 | PSO streams, minimize variation, canonical blend set | **Partial** | Streams: cosmetic. "Map to the nearest canonical state" = **Reject** (changes pixels). Removing the input layout from the PSO key is the real variation reducer -> G6 |
| 3.3 | XXH3 PSO hash, reserved map | **Done** | |
| 4.1 | Instancing detection, ExecuteIndirect batches | **Reject for XDK-era titles / Gated** (G5) | Per-draw constants differ, order matters, CPU cost is not in submission. Revisit only for a game with >3k draws and a worker on the critical path |
| 4.3 | Compute frustum/HZB culling | **Reject** | The game already culls; GPU is 1.3 ms. Culling draws the game submitted changes semantics (occlusion queries, resolves, stencil). The claimed "2-5x less rasterization" has no basis for these titles |
| 4.4 | Async compute queue for post/texture work | **Gated** (G7 for texture decode) | Post FX cost 0.2-0.5 ms each; overlap gain < measurement noise. Texture decode is the one candidate (hitches), and it is a CPU problem first |
| 5.1 | Offline shader translation, never at runtime | **Done** | XenosRecomp (patched) 591/591, DXIL embedded |
| 5.1 | `SampleGrad` with Xenos-computed gradients | **Reject as a blanket rule** | The premise "Xenos has no implicit derivatives" is wrong: tfetch computes LOD from quad derivatives by default, and the ucode selects computed LOD / explicit LOD / gradients per instruction. Map each tfetch mode as the ucode says (translator job, SHADER_PIPELINE.md); a blanket SampleGrad would change filtering |
| 5.1 | `precise` where rounding matters | **Done** | precise oPos, `-ffp-contract=off` on the CPU side (EXP-047) |
| 5.1 | Per-shader automated validation | **Adopt** (new tool) | Matches open question Q-S1: numeric harness shader-interpreter vs DXIL on WARP. VALIDATION_GUIDE §"Shader numeric harness" |
| 5.2 | Vertex fetch in shader (no Input Assembler) | **Gated** (G6), most interesting item of the proposal | Would remove CPU byte-swapping of vertex/index data (`SwapBufferRange` on the worker), remove the vertex declaration from the PSO key, and handle exotic Xenos vertex formats in one place. Costs a XenosRecomp change and a re-validation of every VS. Trigger: upload/swap > ~15% of worker time, or PSO count dominated by decl permutations, or a vertex format the IA cannot express |
| 5.3 | One static root signature, bindless, SM 6.6 heap indexing | **Mostly done** | One RS; SRV/sampler tables cover the whole heaps and are bound once per frame (`frame_state_bound_`). SM 6.6 `ResourceDescriptorHeap[]` saves nothing measurable here. Cheap improvement: root signature 1.1 with `DATA_STATIC_WHILE_SET_AT_EXECUTE` on the per-draw CBVs (ARCHITECTURE §8) |
| 5.4 | Enumerated variant table by root constant | **Done** | Spec constants in shared c28 (alpha test, A2C, soft particles) |
| 6.1 | `CREATE_NOT_ZEROED`, placed resources, residency LRU, budget polling | **Adopt** NOT_ZEROED (cheap); **Gated** the rest (G9) | Placed/aliased heaps and LRU only matter if VRAM > ~70% of a 6 GB handheld budget; Conan uses 0.36 GB |
| 6.2 | Upload ring per frame, copy queue for big uploads | **Done / Gated** | Ring + overflow pages exist. Handheld tweak: size the ring from the measured peak (59 MB of 128 MB x 3) -> G9 |
| 6.3 | Offline/disk cache of deswizzled textures | **Gated** (G7) | Textures decode once and stay cached; the remaining hitches (EXP-044: 4 spikes at title/level transitions) have not been attributed. Measure decode ms per frame first; fix with a decode job pool + copy queue before any disk cache |
| 6.4 L0-L1 | Elide resolve copies when the RT can be sampled directly | **Gated** (G4) | Real but small at 1.3 ms GPU. Useful at 4K on handhelds (bandwidth). Needs a counter of MB copied by resolves first |
| 6.4 L2 | Alias eDRAM surfaces in one heap | **Reject** | The EDRAM overlap model already reproduces guest aliasing semantically; physical aliasing adds barriers and bug surface for no measured gain |
| 6.4 L3 | Native tile loop | **Done differently** | Predicated tiling is dropped: each pass renders once at full size, EndTiling resolves keep tiles in place (Q-R7) |
| 6.5 | MEMEXPORT via UAV/compute | **Adopt when a game uses it** | Conan does not; document as the approach for games that do |
| 7.1 | Flip-discard, tearing, waitable swap chain | **Done** (SDK presenter) | EXP-007, E049 |
| 7.1 | HDR output | **Optional enhancement, low priority** | User preference: defaults = original. Only as an off-by-default option, and only if tested |
| 7.2 | 2 frames in flight, 1 fence per frame | **Gated** (G10) | `kFramesInFlight = 3` + worker lag 1. Input latency was never measured. Measure, then try 2 |
| 7.3 | Delta-time clamp at high fps | **Done** | vblank = 2 x fps cap, the game keeps its own timing |
| 8 | FSR/DLSS/CAS/TAA | **Reject by user policy** | CLAUDE.md: no FSR/CAS, no bicubic upscaling |
| 8 | MSAA, aniso, SSAO/FXAA/bloom | **Done** | EXP-045/048 |
| 9 | PIX markers per pass, DRED, GPU-based validation | **Adopt** | Cheap, generic. DRED auto-breadcrumbs replace the costly MARKER_OUT breadcrumbs (opt-in since EXP-040). ARCHITECTURE §8, VALIDATION_GUIDE |
| 9 | Per-pass GPU timestamps, renderer counters to CSV | **Done / extend** | `native_gpu_pass_timing` exists; add resolve MB, PSOs compiled in play, texture decode ms, upload peak to BENCHMARKS.csv |
| 9 | Golden frames in CI | **Adopt as native golden dumps** | A/B vs Xenos stays the oracle, but a stored set of native dumps per scenario catches regressions without loading the plugin |
| 9 | Deterministic replay of the IR without the game | **Adopt (tooling, medium effort)** | WorkCmd batches are self-contained (captured ranges). Serializing a few frames gives an offline renderer profiler/regression test. VALIDATION_GUIDE §"Capture replay" |
| 10 ph.7 | Same backend binary for 2 games, change only data | **Adopt as direction** | GAME_ADAPTATION_GUIDE already isolates the ~10% game-specific facts; collect them into one `game_profile` header/TOML when porting game #2 (GAME_ADAPTATION_GUIDE.md §5) |
| 11 | Bottleneck checklist | **Replaced** | By the measured "Where is the frame bound?" table in PERFORMANCE_GUIDE |
| A.2 | Format table | **Partially wrong** | `k_16_16_16_16_EXPAND` is float16 (EXP-019), `k_2_10_10_10` is not "depth-as-color", Xenos 7e3 needs R16G16B16A16_FLOAT, not R11G11B10 (alpha is used, EXP-040). Use `texture_decode.cpp` / XDK_D3D_NOTES as the source of truth |
| C | "the SDK's declared goal is to replace the GPU backend" | **Unverified** | UPSTREAM_RESEARCH.md: upstream has nothing beyond v0.10.0's detached mode |

## 3. Items the proposal missed (higher value for these games)

Ranked by expected gain on a guest-bound port. All are measurement-gated.

1. **Guest codegen register-locality flags** (G1). ReXGlue 0.10.0 supports XenonRecomp's
   `skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`, `reserved_as_local`,
   `cr_as_local`, `non_argument_as_local`, `non_volatile_as_local`
   (`sdk/src/codegen/config.cpp:103-116`). Conan's manifest uses **none** of them, so every
   guest register access goes through the `PPCContext` in memory. These let clang keep
   registers in host registers across the whole recompiled game, i.e. they speed up the
   part of the frame that is actually the bottleneck. Constraints: `setjmp`/`longjmp` and
   functions that share registers are handled by the codegen (`context.cpp:48, 179-205`);
   `skip_lr` is incompatible with hooks that read `ctx.lr` (E026) - enable it last and
   only if no hook needs LR. Validate with the A/B scenarios + a long session, paired runs.
   Field evidence (2026-09-27, ANY_GAME_CHECKLIST.md): UnleashedRecomp ships all of them on;
   The Darkness Recomp ships all off as a "correctness profile". No before/after number was
   found in either repository, so the gain must be measured here.
2. **Guest XDK D3D cost** (G2). The hooks call the original XDK functions so their PM4 lands
   in the command segment for the mirror. That guest work (state flush, packet building)
   runs on the critical render thread. skate3recomp found guest packet building to be the
   dominant guest cost. Measure the share of render-thread samples inside the XDK D3D
   address range; if large, candidates are the `SetPending_*` dirty-mask split (skate3,
   UPSTREAM_RESEARCH §6) or replacing selected XDK functions whose PM4 the mirror does not
   need (open question Q-R5 lists the fence paths that must then be fed).
3. **Real occlusion queries in the native path** (G3). Native init fakes "visible" for every
   query (EXP-013), so a game that relies on occlusion culling submits **more** guest work
   than on the console. Implement native queries resolved one frame late (the SDK already
   does this for Xenos, E050) and compare draws/frame native vs legacy. Only relevant for
   games whose capture shows VIZ_QUERY / occlusion use.
4. **Profile-guided optimization of the recompiled code** (G11). clang PGO
   (`-fprofile-generate` on a scenario, `-fprofile-use` in Release) on generated code is a
   generic, semantics-preserving CPU win for branch-heavy recompiled code. Cost: a second
   build, an instrumented run per scenario set. Measure with paired runs; keep
   `-ffp-contract=off`.
5. **Release gates** instead of targets: 0 PSOs compiled during play on the scenario set,
   A/B >= 45 dB, 0 hitches > 50 ms outside loads, CPU at the cap within X% of the
   previous release. Scriptable from the existing logs.

## 4. Inconsistencies found in the kit (fixed or tracked)

1. `LESSONS_LEARNED.md` section headings used an older phase numbering (codegen = phase 2,
   performance = phase 7...) while the playbook and skills use codegen = 1,
   performance = 8. **Fixed** to the playbook numbering.
2. `PERFORMANCE_GUIDE.md` cites "reference EXP-049/EXP-050" (save-pack PSO coverage,
   generated saves per savepoint) but `reference/conan/docs/EXPERIMENT_LOG.md` ends at
   EXP-048. **Fixed**: the guide now says these runs are not in the reference log and
   describes the technique inline.
3. `reference/conan/docs/OPEN_QUESTIONS.md` keeps Q-R7 (drop predicated tiling) and Q-S1
   (shader semantic correctness) "open" although the native renderer answered Q-R7 and A/B
   50-58 dB answers most of Q-S1 in practice. Left as a historical record (reference is
   frozen); the numeric harness is the remaining part of Q-S1.
4. `reference/conan/docs/PROJECT_STATE.md` "Next actions" is from before EXP-037..048
   (still mentions validating `resolution_scale`, which was renamed `render_scale`).
   Historical; new ports use `docs/templates/PROJECT_STATE.md`.
5. The proposal file sat at the kit root, where a fresh agent could take it as
   instructions. **Resolved**: removed; this review is the only record.
6. `docs/templates/BENCHMARKS.csv` has no columns for the metrics that gate optimizations.
   **Fixed**: added `guest_ceiling_ms, worker_busy_pct, pso_compiled_in_play,
   resolve_copy_mb, upload_peak_mb, tex_decode_ms` (`scripts/check_kit.ps1` only checks the
   file exists).
7. CLAUDE.md lists a `/release` skill that did not exist, and the unanchored `release/`
   pattern in `.gitignore` would have ignored `.claude/skills/release/` too. **Fixed**:
   skill added, pattern anchored to `/release/` (the kit's release output folder).
8. The reference renderer has no DRED, no PIX pass markers, no NOT_ZEROED heaps and uses
   root signature 1.0 with one barrier call per transition. Not changed in `reference/`
   (it is the frozen, built-and-validated example); listed as phase-6 hardening in
   NATIVE_RENDERER_ARCHITECTURE.md §8 for the next port.

## 5. Resulting backlog for the next port (in order)

1. Phases 0-7 keep the Conan method (47-58 dB native renderer in ~3 weeks), with these
   additions from the later research: phase 0 answers ANY_GAME_CHECKLIST §1 (title update,
   modules, revision hashes); phase 1 sets setjmp/longjmp; phase 4 records the renderer
   strategy decision; phase 5 handles shaders in compressed packages.
2. Phase 3 baseline now ends with the **bound classification** (guest ceiling in
   BENCHMARKS.csv) and the **codegen optimization window**.
3. Phase 6 adds the cheap hardening: DRED, PIX pass markers, NOT_ZEROED, RS 1.1.
4. Phase 8 order: the Conan checklist -> any G14/G1 left -> G2 XDK D3D cost -> G11 PGO ->
   renderer-side gated items only if the classification says renderer/GPU bound.
5. Tooling investments that pay across games: shader numeric harness, capture replay,
   native golden dumps, release gates, `game_profile` consolidation at game #2.

## 6. Proven vs untested (read before trusting a doc line)

| Status | What |
|---|---|
| Proven on one game (Conan, XDK 2.0.5632) | Phases 0-10 of the playbook, the reference renderer, NativeGraphicsSystem, PSO precompile, launcher, release tooling, every LESSONS_LEARNED row |
| Proven elsewhere, not yet in this kit | `[rexcrt]` (6 surveyed ReXGlue ports), setjmp/longjmp addresses (11 of 12), codegen register-locality flags (UnleashedRecomp, TiP), load-time PSO gating (reblue, UnleashedRecomp), native occlusion queries (reblue), clear-rect coalescing (LostOdysseyRecomp) |
| Unit-tested only | `tools/re/xdk_layout.py` (synthetic layout, `scripts/tests/test_xdk_layout.py`); SDK sraw/srad carry fix (`scripts/tests/test_codegen_sra.py`, no game run with it yet) |
| Documented, never run | G1-G14 as a set on a kit port, §8 hardening of NATIVE_RENDERER_ARCHITECTURE, shader numeric harness, native golden dumps, capture replay, scripted release gates, `game_profile`, handheld affinity proxy runs |
| Expected to differ on the next game | XDK revision (xdk_sigs.py `fuzzy`/`missing` -> semantic discovery), engine pass structure, formats/primitive/packet types Conan never used (log once, implement), MEMEXPORT, 3D texture mips, multiple XEX/DLL modules, 60 fps titles (vblank per frame), shader heuristics keyed by Conan sampler names, shaders stored in compressed packages (UE3), late-XDK function names (xdk_layout.py) |

The first time an untested item is used, record it as an EXP entry with numbers and move it
to "proven" (or record why it was dropped).
