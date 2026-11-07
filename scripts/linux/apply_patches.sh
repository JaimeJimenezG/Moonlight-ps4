#!/usr/bin/env bash
# Apply moonlight-ps4 patches onto pinned third_party checkouts.
# Safe to re-run: skips a patch if it is already applied.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PATCH_DIR="$REPO_DIR/patches"

apply_one() {
    local tree="$1"
    local patch="$2"
    local label="$3"

    if [[ ! -d "$tree" ]]; then
        echo "apply_patches: missing $tree (run scripts/linux/setup_deps.sh first)" >&2
        return 1
    fi
    if [[ ! -f "$patch" ]]; then
        echo "apply_patches: missing patch $patch" >&2
        return 1
    fi

    if git -C "$tree" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$tree" apply "$patch"
        echo "apply_patches: applied $label"
    elif git -C "$tree" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "apply_patches: already applied $label"
    else
        echo "apply_patches: FAILED $label (tree dirty or pin mismatch?)" >&2
        echo "  tree=$tree" >&2
        echo "  patch=$patch" >&2
        return 1
    fi
}

apply_one \
    "$REPO_DIR/third_party/moonlight-common-c" \
    "$PATCH_DIR/moonlight-common-c-orbis.patch" \
    "moonlight-common-c Orbis"

apply_one \
    "$REPO_DIR/third_party/moonlight-common-c/enet" \
    "$PATCH_DIR/moonlight-common-c-enet-orbis.patch" \
    "enet Orbis"

echo "apply_patches: done"
