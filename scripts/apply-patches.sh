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
    #
    # Sorting the two roots together was a bug: "integration" sorts before
    # "upstream", so the hooks became the base layer and every upstream bug
    # report was generated as a diff against a tree that already carried PgrOS
    # code. Those patches have to apply to STOCK upstream to be worth filing.
    find "$ROOT/patches/upstream" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort
    find "$ROOT/patches/integration" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort
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
    # Regenerating a patch is only trivial while no two patches touch the same
    # file. The moment two do, a naive re-diff against pristine upstream hands
    # each one the other's hunks: adding the emoji gesture to
    # TLoraPagerKeyboard.cpp silently rewrote upstream bug report 0001 to contain
    # PgrOS feature code. These patches exist to be filed upstream, so that is a
    # correctness problem rather than a cosmetic one.
    #
    # Patch N is, by definition, the difference between
    #
    #   baseline   pristine upstream plus patches 1..N-1
    #   target     the working tree minus patches N+1..end
    #
    # Both sides are built from the patch files AS THEY ARE ON DISK, never from
    # anything this run has just written. That matters: it is what keeps the
    # result independent of the order patches are visited, and it is the reason
    # refresh can only ever preserve a split, never invent one. A brand new patch
    # picks up whatever is left unattributed, which is exactly right -- but a
    # split that has already been destroyed has to be restored by hand, because
    # nothing in the tree records which hunk belonged to which patch.
    local pristine
    pristine="$(mktemp -d)"
    # Only the source tree is needed as a baseline, and extracting the whole repo
    # trips over symlinks that Windows checkouts cannot create.
    git -C "$VENDOR" archive HEAD src platformio.ini | tar -x -C "$pristine"

    local pending results
    pending="$(mktemp -d)"
    results=""
    # shellcheck disable=SC2064
    trap "rm -rf '$pristine' '$pending'" RETURN

    local dirs
    dirs="$(patch_dirs)"

    local d
    for d in $dirs; do
        local name target files
        name="$(basename "$d")"
        target="$(cd "$d" && pwd)/$name.patch"
        files="$(patch_files "$d")"

        if [ -z "$files" ]; then
            echo "skip $name (no FILES manifest and no existing patch)"
            continue
        fi

        local base work f
        base="$(mktemp -d)"
        work="$(mktemp -d)"

        for f in $files; do
            if [ ! -e "$VENDOR/$f" ]; then
                echo "warning: $name lists $f, which does not exist" >&2
                continue
            fi
            mkdir -p "$base/$(dirname "$f")" "$work/$(dirname "$f")"
            cp "$pristine/$f" "$base/$f" 2>/dev/null || cp "$VENDOR/$f" "$base/$f"
            cp "$VENDOR/$f" "$work/$f"
        done

        # Walk the stack with an explicit position flag. Doing this with sed
        # address ranges was wrong in a way that is easy to miss: "1,/re/p"
        # starts looking for the end address at line TWO, so for the first patch
        # the range ran on past itself and the patch got applied to its own
        # baseline, cancelling out to an empty diff.
        local other seen laters
        seen=0
        laters=""
        for other in $dirs; do
            if [ "$other" = "$d" ]; then
                seen=1
                continue
            fi
            if [ "$seen" = "0" ]; then
                # baseline: pristine plus every EARLIER patch, our files only
                stack_apply "$base" "$other" "$files" "" "$name"
            else
                laters="$other
$laters"
            fi
        done

        # target: the working tree minus every LATER patch, newest first
        for other in $laters; do
            stack_apply "$work" "$other" "$files" "-R" "$name"
        done

        # Staged, not written. Writing as we go would let a patch computed
        # earlier in this run become an input to one computed later, which is
        # exactly the coupling the baseline/target split exists to avoid.
        : > "$pending/$name.patch"
        for f in $files; do
            [ -e "$work/$f" ] || continue
            diff_files "$base/$f" "$work/$f" "$f" >> "$pending/$name.patch"
        done
        rm -rf "$base" "$work"

        results="$results$name|$target
"
    done

    # Nothing has touched patches/ up to this point.
    local line
    while IFS='|' read -r name target; do
        [ -n "$name" ] || continue
        cp "$pending/$name.patch" "$target"
        if [ ! -s "$target" ]; then
            echo "refreshed $name (empty -- no changes in its files)"
        else
            echo "refreshed $name ($(wc -l < "$target") lines)"
        fi
    done <<EOF
$results
EOF
}

# Apply (or reverse, with $4 = -R) one patch into a scratch tree, limited to the
# files the patch being refreshed owns. Hunks for any other file are not ours to
# touch and are not present in the tree anyway.
stack_apply() {
    local tree="$1" other="$2" files="$3" rev="$4" owner="$5"
    local oname opatch inc f
    oname="$(basename "$other")"
    opatch="$(cd "$other" && pwd)/$oname.patch"
    [ -f "$opatch" ] || return 0

    inc=""
    for f in $files; do
        case "$(patch_files "$other")" in
        *"$f"*) inc="$inc --include=$f" ;;
        esac
    done
    [ -n "$inc" ] || return 0

    # shellcheck disable=SC2086
    (cd "$tree" && git apply $rev -p1 $inc "$opatch") ||
        echo "warning: could not stage $oname while refreshing $owner; check the result" >&2
}

# The files a patch owns. An explicit FILES manifest is preferred so a brand new
# patch can be generated the same way as an existing one; falling back to the
# patch itself keeps older patch directories working.
patch_files() {
    local d="$1"
    if [ -f "$d/FILES" ]; then
        grep -v '^[[:space:]]*$' "$d/FILES" | grep -v '^#'
    elif [ -f "$d/$(basename "$d").patch" ]; then
        grep '^--- a/' "$d/$(basename "$d").patch" | sed 's|^--- a/||' | sort -u
    fi
}

# One file's diff, with the a/ and b/ prefixes `git apply -p1` expects. Both
# sides live in scratch directories, so git's own prefixes are rewritten back to
# the repo-relative path.
diff_files() {
    local old="$1" new="$2" rel="$3"
    git --no-pager diff --no-index -- "$old" "$new" 2>/dev/null |
        sed -e "s|^--- a/.*|--- a/$rel|" \
            -e "s|^+++ b/.*|+++ b/$rel|" \
            -e "s|^diff --git .*|diff --git a/$rel b/$rel|" ||
        true
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
