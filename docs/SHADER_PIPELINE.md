# Shader pipeline (Xenos microcode -> HLSL -> DXIL, offline)

One command: `bash tools/shaders/build_corpus.sh` (~40 s for ~600 shaders).

1. Fetch DXC (tools/dxc) if missing.
2. `tools/xenosrecomp/fetch_source.sh`: pinned zolaware/reblue-XenosRecomp @339af41
   (local mirror in `_research/upstream/reblue-XenosRecomp`, no network needed) +
   `tools/xenosrecomp/patches/0001-conan-recomp.patch`; build `XenosRecompCorpus`
   (CMake, clang; deps fmt/xxHash from the kit SDK).
3. `extract_shaders.py`: byte-granular scan of every game file + the decoded default.xex
   for shader containers (Conan: all 1800 in `shaders/shaders.stx`, 591 unique; 1659 of
   them start at an offset % 4 == 1 - do not assume alignment).
4. `join_runtime_cache.py`: join with the Xenos backend's runtime shader storage
   (`bench/**/<TITLE_ID>.xsh`) to check coverage (PS match exactly; VS ucode is patched
   by the XDK at declaration bind, so join VS by normalized vfetch).
5. `build_catalog.py`: translate, compile DXIL (vs/ps_6_0; lib_6_3 variants for shaders
   using spec constants) + SPIR-V, reflect constants/samplers/interpolators ->
   `artifacts/shaders/catalog.json`, `artifacts/shaders/dxil/*.dxil`, docs/SHADER_CATALOG.md,
   `tfetch_filters.txt` (per-instruction filter overrides).
6. CMake packs `artifacts/shaders/dxil` into one blob (RCDATA 2) via `pack_shaders.py`;
   the renderer looks shaders up by container hash (entries are packed 20-byte records:
   read with memcpy, never as an aligned struct).

Identity: `container_hash` = XXH3_64 of container bytes [0, virtualSize+physicalSize)
(same key as XenosRecomp/UnleashedRecomp/re:Blue); `ucode_hash` = XXH3_64 of the
microcode (ReXGlue Shader::ucode_data_hash). The native renderer hashes the container at
the XDK CreateShader hook.

## What the patch (CONAN_RECOMP) adds to stock reblue-XenosRecomp

Stock translation compiled 234/591 Conan shaders; the patch reached 591/591. Features:

| Area | Change |
|---|---|
| Int loop constants | `i16`.. loops implemented (count/start/step, aL restore, literal `defi` loops); unstructured `switch(pc)` loops single-level |
| Bool constants | PS bools keyed by unified CF address fixed; `cexec bN` blocks conditional (were unconditional) |
| Relative addressing | whole 256-entry float file as one array; `src_const_is_addressed()` slot rules; const1Relative honored |
| Spec constants at runtime | `g_SpecConstants()` reads shared c28.x: alpha test (bit 1), alpha-to-coverage (bit 3, mip-corrected alpha, UnleashedRecomp branch), soft particles (bits 4/5) |
| Screen-space draws | `g_ScreenXform` (c27) applied to oPos |
| Pixel centers | `g_HalfPixelOffset * oPos.w` driven by the host |
| Upscaled resolves | TEX_SCALE (descriptor bits 16-27) keeps tfetch offsets, getWeights2D and bicubic fetches in guest texels |
| Pixel position input | `g_PixelPosScale` maps host pixels to guest pixels |
| Gamma textures | PWL gamma (descriptor bit 31) after filtering |
| Smooth effects | descriptor bit 28 -> cubic B-spline magnification in tfetch2D |
| Shadow atlas (Conan-specific heuristic) | in PS sampling `g_ShadowMapTextureAtlas`, literal vectors that are multiples of 0.25/4096 within +-2/4096 are scaled by `g_ShadowAtlasTexelScale`; optional per-pixel rotation (`g_ShadowSoftness`) |
| Soft particles | fade by the resolved scene depth |
| Filter overrides | `// conan_tfetch slot= mag= min= mip= aniso=` comments for the catalog |

For a new game: the generic rows apply as is. Re-check the Conan-specific heuristic
(shadow atlas sampler name and kernel literals) against the new catalog; guard new
heuristics by sampler/constant names from the reflection, never by shader hash lists.

## Status tracking

Keep `docs/SHADER_CATALOG.md` generated (translate/compile status per shader) and mark
visually validated ones through the A/B scenarios. Failures seen in stock translation are
a checklist for new games: undeclared loop constants, PS bool indexing, translator asserts
on relative addressing of unnamed registers, silent cexec/const1Relative/CondExecPredCleanEnd
bugs.
