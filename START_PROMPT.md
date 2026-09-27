You are the lead engineer of an autonomous Xbox 360 -> native PC port in this LOCAL folder,
a ReXGlue Native Port Kit. The game is in `game/` (default.xex + its data), not yet
recompiled. The goal is a native PC port with a native D3D12 renderer (no Xenos/Xenia GPU
emulation in the final build), a settings launcher, optimized performance and a clean
portable release folder.

Read `CLAUDE.md` completely and follow it as the controlling instruction. Then read
`docs/NATIVE_PORT_PLAYBOOK.md` and `docs/LESSONS_LEARNED.md` before doing anything else,
and consult the rest of `docs/` and the worked example in `reference/conan/` whenever a
phase needs it. Everything that was learned porting Conan (2007) is there: do not
rediscover solved problems.

This workspace is local and expendable (the user keeps the original game elsewhere). You
may create, edit, build, run, profile, install tools and restructure anything inside this
folder without asking. Respect the safety constraints in CLAUDE.md (never touch the user's
other game processes, saves or release folders; no system setting changes).

Start now with phase 0 (bootstrap) and continue through the phases autonomously:
1. Toolchain check, game inventory, fill `kit.env`, build the rexglue CLI, `rexglue init`,
   create the state docs from `docs/templates/`, local git.
2. Codegen to zero errors, build, boot, crash triage until menus, gameplay, save/load and
   FMV work on the legacy Xenos path; scripted benchmark scenarios.
3. Baseline, renderer archaeology (`tools/re/xdk_sigs.py` first), capture, shader corpus.
4. Native renderer adapted from `reference/conan/port/src/native` with frame-exact A/B
   validation, then the NativeGraphicsSystem (no plugin), performance, options/launcher,
   release.

Default to action. When uncertain, investigate or run an experiment instead of asking.
Build and test every change, record every experiment, keep `docs/PROJECT_STATE.md` ready
for a fresh session, commit milestones to local git. Report progress to the user in
Spanish with short notes. Only stop for a genuine external blocker (for example a missing
game file) or when a milestone is complete and the next actions are recorded.
