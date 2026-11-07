#!/usr/bin/env bash
# Fetch pinned third-party dependencies and apply local patches.
#
# Prefer this over a bare `git submodule update --recursive` so pins in
# third_party/DEPS and patches/ stay in sync.
#
# Usage:
#   scripts/linux/setup_deps.sh       # init/update to pins + apply patches
#   scripts/linux/setup_deps.sh --no-patch
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS_FILE="$REPO_DIR/third_party/DEPS"
APPLY_PATCHES=1

for arg in "$@"; do
    case "$arg" in
        --no-patch) APPLY_PATCHES=0 ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "setup_deps: unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

if [[ ! -f "$DEPS_FILE" ]]; then
    echo "setup_deps: missing $DEPS_FILE" >&2
    exit 1
fi

# Resolve a pin that may be a tag or a full/abbreviated SHA.
resolve_pin() {
    local tree="$1"
    local pin="$2"
    if git -C "$tree" rev-parse --verify "${pin}^{commit}" >/dev/null 2>&1; then
        git -C "$tree" rev-parse "${pin}^{commit}"
        return 0
    fi
    # Fetch tags/commits if the pin is not present yet.
    git -C "$tree" fetch --tags --force origin "+refs/heads/*:refs/remotes/origin/*" >/dev/null 2>&1 || true
    git -C "$tree" fetch origin "$pin" >/dev/null 2>&1 || true
    git -C "$tree" rev-parse --verify "${pin}^{commit}"
}

checkout_pin() {
    local name="$1"
    local path="$2"
    local url="$3"
    local pin="$4"
    local abs="$REPO_DIR/$path"

    if [[ ! -e "$abs/.git" && ! -f "$abs/.git" ]]; then
        echo "setup_deps: cloning $name → $path"
        mkdir -p "$(dirname "$abs")"
        git clone --no-checkout "$url" "$abs"
    fi

    # Ensure origin URL matches DEPS.
    git -C "$abs" remote set-url origin "$url" 2>/dev/null || \
        git -C "$abs" remote add origin "$url"

    local sha
    sha="$(resolve_pin "$abs" "$pin")"
    echo "setup_deps: $name @ ${sha:0:12} (pin $pin)"

    # Discard previous patch dirt so apply_patches can run cleanly.
    git -C "$abs" reset --hard "$sha"
    git -C "$abs" clean -fd
}

echo "setup_deps: reading $DEPS_FILE"

# First pass: top-level deps (not nested under moonlight-common-c/*).
while read -r name path url pin; do
    [[ -z "${name:-}" || "$name" =~ ^# ]] && continue
    case "$path" in
        third_party/moonlight-common-c/*) continue ;;
    esac
    checkout_pin "$name" "$path" "$url" "$pin"
done < <(grep -v '^[[:space:]]*#' "$DEPS_FILE" | grep -v '^[[:space:]]*$')

# Nested pins (enet / nanors) after parent exists.
while read -r name path url pin; do
    [[ -z "${name:-}" || "$name" =~ ^# ]] && continue
    case "$path" in
        third_party/moonlight-common-c/*) ;;
        *) continue ;;
    esac
    checkout_pin "$name" "$path" "$url" "$pin"
done < <(grep -v '^[[:space:]]*#' "$DEPS_FILE" | grep -v '^[[:space:]]*$')

# Sync parent-index gitlinks when this tree is a git checkout (optional).
if git -C "$REPO_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$REPO_DIR" submodule sync --recursive >/dev/null 2>&1 || true
    # Applied Orbis patches leave mcc/enet dirty; do not show that in status.
    git -C "$REPO_DIR" config submodule.third_party/moonlight-common-c.ignore dirty
fi

if [[ "$APPLY_PATCHES" -eq 1 ]]; then
    "$REPO_DIR/scripts/linux/apply_patches.sh"
else
    echo "setup_deps: skipped patches (--no-patch)"
fi

echo "setup_deps: done"
