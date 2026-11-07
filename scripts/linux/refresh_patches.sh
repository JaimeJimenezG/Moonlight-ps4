#!/usr/bin/env bash
# Regenerate patches/ from the current (dirty) third_party working trees
# against the pins in third_party/DEPS.
#
# Workflow:
#   1. Edit files under third_party/moonlight-common-c (or enet)
#   2. scripts/linux/refresh_patches.sh
#   3. Commit the updated files under patches/ (not the submodule dirt)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS_FILE="$REPO_DIR/third_party/DEPS"
PATCH_DIR="$REPO_DIR/patches"

pin_for() {
    local want_path="$1"
    while read -r name path url pin; do
        [[ -z "${name:-}" || "$name" =~ ^# ]] && continue
        if [[ "$path" == "$want_path" ]]; then
            echo "$pin"
            return 0
        fi
    done < <(grep -v '^[[:space:]]*#' "$DEPS_FILE" | grep -v '^[[:space:]]*$')
    return 1
}

diff_against_pin() {
    local tree="$1"
    local pin="$2"
    local out="$3"
    shift 3
    local sha
    sha="$(git -C "$tree" rev-parse "${pin}^{commit}")"
    git -C "$tree" -c diff.noprefix=false diff \
        --ignore-submodules=all \
        --src-prefix=a/ --dst-prefix=b/ \
        "$sha" -- "$@" > "$out"
    echo "refresh_patches: wrote $out ($(wc -l < "$out") lines)"
}

MCC_PIN="$(pin_for third_party/moonlight-common-c)"
ENET_PIN="$(pin_for third_party/moonlight-common-c/enet)"

diff_against_pin \
    "$REPO_DIR/third_party/moonlight-common-c" \
    "$MCC_PIN" \
    "$PATCH_DIR/moonlight-common-c-orbis.patch" \
    src/

diff_against_pin \
    "$REPO_DIR/third_party/moonlight-common-c/enet" \
    "$ENET_PIN" \
    "$PATCH_DIR/moonlight-common-c-enet-orbis.patch" \
    unix.c

echo "refresh_patches: done — review patches/ then commit those files only"
