# Knowledge base index

| Document | Read when |
|---|---|
| NATIVE_PORT_PLAYBOOK.md | always first: phases, exit criteria, commands |
| LESSONS_LEARNED.md | before debugging anything: symptom -> cause -> fix |
| GAME_ADAPTATION_GUIDE.md | porting the reference renderer; finding hook addresses |
| NATIVE_RENDERER_ARCHITECTURE.md | how the native renderer and NativeGraphicsSystem work |
| XDK_D3D_NOTES.md | XDK D3D device/resources/command stream and Xenos semantics |
| SHADER_PIPELINE.md | shader corpus, XenosRecomp patch features |
| PERFORMANCE_GUIDE.md | measuring and optimizing; what cost what |
| VALIDATION_GUIDE.md | frame-exact A/B, diagnosing differences, stability runs |
| RELEASE_AND_SETTINGS.md | user preferences, cfg, launcher, app hooks, release |
| TOOLCHAIN_SETUP.md | compilers, SDK build, configure commands |
| TOOLS_REFERENCE.md | every script/tool and the SDK cvars |
| REXGLUE_PORTING_RULES.md | ReXGlue 0.10.0 codegen/manifest/hook rules (recompilation phase) |
| UPSTREAM_RESEARCH.md, REFERENCE_SOURCES.md, LOCAL_WORKSPACE.md | upstream projects, sources, workspace contract |
| templates/ | per-game state documents to copy in phase 0 |

Per-game documents (created in phase 0 from templates/): PROJECT_STATE.md, EXPERIMENT_LOG.md,
OPEN_QUESTIONS.md, BENCHMARKS.csv, RENDERER_ANALYSIS.md, SHADER_CATALOG.md,
PIPELINE_CATALOG.md, and port/docs/error_log.md.
