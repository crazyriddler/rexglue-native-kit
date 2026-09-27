# Local workspace contract

## Environment model

The game port is a normal local directory on the user's Windows PC.

Treat the current working directory as the authoritative project root.

A GitHub-hosted copy of the project is NOT required. Never block work because:
- there is no GitHub repository;
- there is no `origin` remote;
- GitHub authentication is unavailable;
- PR/issue APIs are unavailable;
- CI does not exist.

## Local Git

Git is optional but strongly useful for engineering checkpoints.

If `.git/` is absent and `git` is installed, you may initialize a local repository when useful:

```powershell
git init
git add -A
git commit -m "checkpoint: initial functional ReXGlue port"
```

No remote is needed for any of those operations.

Use commits/tags/branches/worktrees locally when they improve experimentation. Do not make remote-hosting workflows a prerequisite.

## External source research

Public source code may be inspected from the Internet when useful: shallow clones in
`_research/upstream/<repository>` (gitignored). The project index and what was already
learned from each project: docs/UPSTREAM_RESEARCH.md. If `git clone` is inconvenient, use
web access or release archives.

## Project autonomy

Inside this working directory you may take any engineering action the goal needs (the rules of
CLAUDE.md and the priorities of docs/DECISION_GUIDE.md still decide *which* action is worth
taking; the reference design is adapted, not replaced, unless measurements show a need):
- edit/delete/rewrite source;
- replace generated files;
- alter ReXGlue integration;
- replace or fork XenosRecomp;
- add a new RHI/backend;
- modify build scripts;
- install/vend tools;
- generate shader caches;
- run executables;
- profile and capture;
- create experimental branches or directories;
- restructure the project.

The backup is outside this workspace, so recoverability of this working tree must not prevent experimentation.
