# Upstream research clones

Disposable shallow clones of other projects live here (gitignored). The inspected commits,
URLs and findings are in docs/UPSTREAM_RESEARCH.md. Clone again when a question needs the
source, e.g.:

```bash
git clone --depth 1 https://github.com/zolaware/reblue _research/upstream/reblue
```

`tools/xenosrecomp/fetch_source.sh` uses `_research/upstream/reblue-XenosRecomp` as an
offline mirror when it exists and contains the pinned commit; otherwise it clones from GitHub.
