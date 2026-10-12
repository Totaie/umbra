#!/usr/bin/env bash
#
# Fetches the submodules a build needs. Use this on a fresh clone instead of
#
#     git submodule update --init --recursive
#
# which fails on this tree: umbra-v2 pins moonlight-common-c at a commit that was
# made in one working copy and never pushed, so there is nowhere to fetch it from.
# Until the pin names something published, this falls back to the last commit that
# is, plus the one change the missing commit made (packaging/patches). Once the pin
# is fixed the ordinary update succeeds and none of the fallback runs.
#
# Safe to run again; it leaves an already prepared tree alone.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

git submodule update --init app/SDL_GameControllerDB qmdnsengine/qmdnsengine

# Only the Windows build compiles AMF, and it is not a small download.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) git submodule update --init third-party/AMF ;;
esac

common=moonlight-common-c/moonlight-common-c
published=7bfa0c1e79d55871c519287c441a629b9eeed045
patch=$PWD/packaging/patches/moonlight-common-c-optional-mic.patch

if ! git submodule update --init "$common"; then
    echo >&2
    echo "moonlight-common-c: the pinned commit is not published (the error above)." >&2
    echo "Using ${published:0:8} with $(basename "$patch") instead." >&2

    git -C "$common" checkout --quiet "$published"

    # Applied already if it comes back out cleanly.
    if ! git -C "$common" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$common" apply "$patch"
    fi
fi

git -C "$common" submodule update --init --recursive
