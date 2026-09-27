#!/usr/bin/env bash
# Fetch the pinned reblue-XenosRecomp source into tools/xenosrecomp/src and
# apply tools/xenosrecomp/patches/*.patch. Idempotent: re-running resets src/.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REPO_URL="https://github.com/zolaware/reblue-XenosRecomp"
PIN="339af41df2c23dbe3256c1c377716b81a0e0fe6b"
LOCAL_MIRROR="$ROOT/_research/upstream/reblue-XenosRecomp"
SRC="$HERE/src"

if [ -d "$SRC/.git" ] && [ "$(git -C "$SRC" rev-parse HEAD)" = "$PIN" ]; then
  git -C "$SRC" reset -q --hard "$PIN"
  git -C "$SRC" clean -qfdx
else
  rm -rf "$SRC"
  if [ -d "$LOCAL_MIRROR/.git" ] && git -C "$LOCAL_MIRROR" cat-file -e "$PIN^{commit}" 2>/dev/null; then
    git clone -q --no-checkout "$LOCAL_MIRROR" "$SRC"
  else
    git clone -q --no-checkout "$REPO_URL" "$SRC"
  fi
  git -C "$SRC" -c advice.detachedHead=false checkout -q "$PIN"
fi

shopt -s nullglob
for p in "$HERE"/patches/*.patch; do
  echo "[xenosrecomp] applying $(basename "$p")"
  git -C "$SRC" apply --whitespace=nowarn "$p"
done
echo "[xenosrecomp] source ready at $SRC ($(git -C "$SRC" rev-parse --short HEAD) + patches)"
