#!/usr/bin/env bash
#
# Apply the PgrOS patch set to vendor/firmware.
#
# Patches live in two directories and are treated very differently:
#
#   patches/upstream/    Genuine Meshtastic bugs. Each has a README.md with a
#                        root-cause writeup suitable for filing as an issue and
#                        opening a PR. These are intended to go away as they
#                        land upstream.
#
#   patches/integration/ Build hooks that let PgrOS compile as an overlay on
#                        this tree. Not upstreamable, not bugs.
#
# The vendored tree is a pinned git checkout, so "have the patches been applied"
# is answered by git itself rather than a marker file.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/vendor/firmware"

if [ ! -d "$VENDOR/.git" ]; then
    echo "error: $VENDOR is not a git checkout." >&2
    echo "       Run: git submodule update --init --recursive" >&2
    exit 1
fi

usage() {
    cat <<'USAGE'
usage: apply-patches.sh [--check|--reset|--refresh]

  (no args)   Apply every patch to vendor/firmware.
  --check     Report whether the tree is clean/patched. Exit 1 if unexpected.
  --reset     Discard ALL local changes in vendor/firmware and return it to the
              pinned upstream commit.
  --refresh   Regenerate each .patch from the current state of vendor/firmware.
              Use after hand-editing vendored code.
USAGE
}

patch_dirs() {
    # Deterministic order: upstream fixes first, then integration hooks.
    find "$ROOT/patches/upstream" "$ROOT/patches/integration" \
        -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort
}

cmd_reset() {
    echo "Resetting vendor/firmware to pinned upstream commit..."
    git -C "$VENDOR" reset --hard HEAD
    git -C "$VENDOR" clean -fd -e .pio
    echo "Clean. vendor/firmware is now stock upstream:"
    git -C "$VENDOR" log -1 --oneline
}

cmd_check() {
    if git -C "$VENDOR" diff --quiet; then
        echo "vendor/firmware is STOCK (no patches applied)."
        return 0
    fi
    echo "vendor/firmware has local modifications:"
    git -C "$VENDOR" diff --stat
    return 0
}

cmd_refresh() {
    local d name target
    for d in $(patch_dirs); do
        name="$(basename "$d")"
        target="$d/$name.patch"
        if [ ! -f "$target" ]; then
            echo "skip $name (no existing patch file to refresh)"
            continue
        fi
        # Recover the file list this patch touches, then re-diff exactly those.
        local files
        files="$(grep '^--- a/' "$target" | sed 's|^--- a/||' | sort -u)"
        if [ -z "$files" ]; then
            echo "skip $name (could not determine file list)"
            continue
        fi
        # shellcheck disable=SC2086
        git -C "$VENDOR" diff -- $files > "$target"
        echo "refreshed $name ($(wc -l < "$target") lines)"
    done
}

cmd_apply() {
    local d name target applied=0 skipped=0
    for d in $(patch_dirs); do
        name="$(basename "$d")"
        target="$d/$name.patch"
        [ -f "$target" ] || { echo "skip $name (no .patch file)"; continue; }

        if git -C "$VENDOR" apply --reverse --check "$target" 2>/dev/null; then
            echo "already applied: $name"
            skipped=$((skipped + 1))
            continue
        fi

        if ! git -C "$VENDOR" apply --check "$target" 2>/dev/null; then
            echo "ERROR: $name does not apply cleanly to the pinned tree." >&2
            echo "       The pin may have moved, or the patch needs a refresh." >&2
            exit 1
        fi

        git -C "$VENDOR" apply "$target"
        echo "applied: $name"
        applied=$((applied + 1))
    done
    echo
    echo "$applied applied, $skipped already present."
}

case "${1:-}" in
    "")         cmd_apply ;;
    --check)    cmd_check ;;
    --reset)    cmd_reset ;;
    --refresh)  cmd_refresh ;;
    -h|--help)  usage ;;
    *)          usage; exit 2 ;;
esac
