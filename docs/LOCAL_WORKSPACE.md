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

Public source code may be inspected from the Internet when useful.

Preferred location for disposable clones:

```text
_research/upstream/<repository>
```

These are references, not the project repository.

Useful examples:
- rexglue/rexglue-sdk
- hedge-dev/XenosRecomp
- hedge-dev/UnleashedRecomp
- zolaware/reblue
- zolaware/reblue-XenosRecomp
- other game-specific ReXGlue/recompilation ports discovered during research

If `git clone` is inconvenient, use web access, raw file downloads, release archives, or another appropriate local research method.

## Project autonomy

Inside this working directory, take whatever engineering actions are needed to achieve the renderer goal:
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
