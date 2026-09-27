#!/usr/bin/env bash
# Re-vendor tools/xenosrecomp/src: pinned reblue-XenosRecomp + tools/xenosrecomp/patches/*.patch,
# stored without .git (the kit ships src/ already patched; run this only after changing the
# pin or a patch). Replaces src/ entirely.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REPO_URL="https://github.com/zolaware/reblue-XenosRecomp"
PIN="339af41df2c23dbe3256c1c377716b81a0e0fe6b"
LOCAL_MIRROR="$ROOT/_research/upstream/reblue-XenosRecomp"
SRC="$HERE/src"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -d "$LOCAL_MIRROR/.git" ] && git -C "$LOCAL_MIRROR" cat-file -e "$PIN^{commit}" 2>/dev/null; then
  git clone -q --no-checkout "$LOCAL_MIRROR" "$TMP/src"
else
  git clone -q --no-checkout "$REPO_URL" "$TMP/src"
fi
git -C "$TMP/src" -c advice.detachedHead=false checkout -q "$PIN"

shopt -s nullglob
for p in "$HERE"/patches/*.patch; do
  echo "[xenosrecomp] applying $(basename "$p")"
  git -C "$TMP/src" apply --whitespace=nowarn "$p"
done
rm -rf "$TMP/src/.git" "$SRC"
mv "$TMP/src" "$SRC"
echo "[xenosrecomp] source vendored at $SRC (${PIN:0:7} + patches)"
