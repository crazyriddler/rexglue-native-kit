#!/usr/bin/env bash
# Re-vendor sdk/thirdparty/ entries.
#
# The kit vendors the SDK's third-party code in git (~400 MB, no submodules, kit patches
# applied), so a clone needs nothing. Use this only to rebuild a deleted or empty entry,
# exactly as the kit expects:
#   - every entry of upstream rexglue-sdk thirdparty/ at the kit's base commit (c94f5eb):
#     plain directories are copied from that commit, submodules are fetched at the commit
#     that base pins (sdk/.gitmodules gives the URLs), without .git (vendored layout);
#   - then the kit's own patches to third-party code (sdk/patches/thirdparty/*.patch).
# An entry that already exists and is non-empty is never touched (your local copy wins).
#
# usage: scripts/restore_sdk_thirdparty.sh [name,name,...]   (default: every entry)
# Needs git and network access to github.com. Windows without symlink privilege: symlink
# placeholders are materialized with scripts/port/fix_broken_symlinks.py.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$ROOT/sdk"
BASE=c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
URL=https://github.com/rexglue/rexglue-sdk.git
ONLY=",${1:-},"
PY=$(command -v python3 || command -v python || true)
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone -q --filter=blob:none --no-checkout "$URL" "$TMP/sdk"
restored=()
while read -r _mode type sha path; do
  name="${path#thirdparty/}"
  [ "$name" = "CMakeLists.txt" ] && continue
  [ "$ONLY" != ",," ] && [[ "$ONLY" != *",$name,"* ]] && continue
  dest="$SDK/thirdparty/$name"
  if [ -d "$dest" ] && [ -n "$(ls -A "$dest")" ]; then
    echo "[thirdparty] keep   $name (already present)"
    continue
  fi
  rm -rf "$dest"
  if [ "$type" = tree ]; then
    git -C "$TMP/sdk" checkout -q "$BASE" -- "thirdparty/$name"
    cp -r "$TMP/sdk/thirdparty/$name" "$dest"
  else
    sub_url=$(git config -f "$SDK/.gitmodules" --get "submodule.thirdparty/$name.url")
    git init -q "$dest"
    git -C "$dest" fetch -q --depth 1 "$sub_url" "$sha"
    git -C "$dest" -c advice.detachedHead=false checkout -q FETCH_HEAD
    case "$(uname -s)" in
      MINGW*|MSYS*|CYGWIN*) [ -n "$PY" ] && "$PY" "$ROOT/scripts/port/fix_broken_symlinks.py" "$dest" ;;
    esac
    rm -rf "$dest/.git"
  fi
  echo "[thirdparty] restored $name ($type ${sha:0:10})"
  restored+=("$name")
done < <(git -C "$TMP/sdk" ls-tree "$BASE" thirdparty/)

# Kit patches: applied only to entries restored now (a kept local copy already has them).
shopt -s nullglob
for p in "$SDK"/patches/thirdparty/*.patch; do
  target=$(sed -n 's|^+++ b/thirdparty/\([^/]*\)/.*|\1|p' "$p" | head -1)
  if [[ " ${restored[*]:-} " == *" $target "* ]]; then
    git -C "$ROOT" apply --directory=sdk --whitespace=nowarn "$p"
    echo "[thirdparty] patched $target ($(basename "$p"))"
  fi
done
echo "[thirdparty] done: ${#restored[@]} entries restored"
