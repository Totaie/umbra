#!/usr/bin/env bash
#
# Builds the Umbra host on Windows from an MSYS2 UCRT64 shell.
#
# Usage, from "MSYS2 UCRT64":
#     ./scripts/umbra_windows_build.sh [--clean] [--package]
#
# Prerequisites are the packages listed in docs/building.md, plus two the docs omit:
#
#     pacman -S mingw-w64-ucrt-x86_64-nodejs mingw-w64-ucrt-x86_64-nlohmann-json
#
# nodejs builds the web interface. The MSYS2 package is the path of least resistance;
# installing Node on the Windows side also works, but the winget package runs an
# elevated MSI that fails with 1619 in a non-interactive shell.
#
# nlohmann-json matters because when CMake can't find it as a system package it falls
# back to FetchContent, and the sunshine_display_helper target doesn't pick up the
# fetched include directory. That target then fails with
#     src/utility.h:14:10: fatal error: nlohmann/json.hpp: No such file or directory
# roughly 40 targets into the build. Installing it system-wide puts the header on the
# compiler's default include path and sidesteps the problem entirely.
#
# Why this script exists rather than the plain `cmake -B build -G Ninja -S .` from
# docs/building.md:
#
#   cmake/dependencies/ffmpeg.cmake builds the prebuilt-binary asset name from
#   "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}". CMake derives
#   CMAKE_SYSTEM_PROCESSOR on Windows from the PROCESSOR_ARCHITECTURE environment
#   variable, which MSYS2's bash does not export. It therefore comes out empty and
#   the configure step tries to download
#       .../releases/download/<tag>/Windows--ffmpeg.tar.gz
#   instead of
#       .../releases/download/<tag>/Windows-AMD64-ffmpeg.tar.gz
#   and fails with "Failed to download FFmpeg binaries: HTTP response code said
#   error", which gives no hint that the architecture is the problem.
#
#   Passing -DCMAKE_SYSTEM_PROCESSOR does not help: platform detection overwrites it.
#   The variable has to be in the environment before CMake runs.

set -euo pipefail

SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${SOURCE_ROOT}/build"

DO_CLEAN=0
DO_PACKAGE=0
for arg in "$@"; do
    case "$arg" in
        --clean)   DO_CLEAN=1 ;;
        --package) DO_PACKAGE=1 ;;
        *)
            echo "Unknown option: $arg" >&2
            echo "Usage: $0 [--clean] [--package]" >&2
            exit 1
            ;;
    esac
done

if [[ "${MSYSTEM:-}" != "UCRT64" && "${MSYSTEM:-}" != "CLANGARM64" ]]; then
    echo "This must run from an MSYS2 UCRT64 (or CLANGARM64) shell." >&2
    echo "MSYSTEM is currently '${MSYSTEM:-unset}'." >&2
    exit 1
fi

# See the comment at the top: without this the FFmpeg download 404s.
if [[ -z "${PROCESSOR_ARCHITECTURE:-}" ]]; then
    case "$(uname -m)" in
        x86_64)  export PROCESSOR_ARCHITECTURE=AMD64 ;;
        aarch64) export PROCESSOR_ARCHITECTURE=ARM64 ;;
        *)
            echo "Unrecognised machine type $(uname -m); set PROCESSOR_ARCHITECTURE yourself." >&2
            exit 1
            ;;
    esac
    echo "Set PROCESSOR_ARCHITECTURE=${PROCESSOR_ARCHITECTURE} for FFmpeg dependency resolution"
fi

if ! command -v npm >/dev/null 2>&1 && [[ ! -x "/c/Program Files/nodejs/npm.cmd" ]]; then
    echo "ERROR: npm was not found, and the web_ui target is not optional: the build" >&2
    echo "       fails at 'Unable to build the Vibepollo browser interface'." >&2
    echo "       Install it with: pacman -S mingw-w64-ucrt-x86_64-nodejs" >&2
    exit 1
fi

# Checked up front because the failure is ~40 targets in and the message doesn't
# suggest a missing package. See the header comment.
if [[ ! -f "${MINGW_PREFIX:-/ucrt64}/include/nlohmann/json.hpp" ]]; then
    echo "ERROR: nlohmann/json.hpp was not found as a system header." >&2
    echo "       Install it with: pacman -S mingw-w64-ucrt-x86_64-nlohmann-json" >&2
    exit 1
fi

if [[ "$DO_CLEAN" == "1" ]]; then
    echo "Cleaning ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

echo "Configuring..."
# The repo used for update checks is passed explicitly rather than relying on the
# defaults in cmake/prep/build_version.cmake. Those are CACHE variables, so a build
# directory first configured against upstream keeps upstream's values forever and the
# host would quietly check Vibepollo for its updates instead of Umbra.
cmake -B "${BUILD_DIR}" -G Ninja -S "${SOURCE_ROOT}" \
      -DSUNSHINE_REPO_OWNER=Totaie \
      -DSUNSHINE_REPO_NAME=umbra-host

# Cap parallelism. Ninja defaults to CPU count + 2, which is 14 jobs on a 12 thread
# machine, and this codebase compiles Boost and C++23 templates that can take well
# over a gigabyte per cc1plus. That exhausts RAM long before it saturates the CPU,
# and the machine spends the build swapping instead of compiling.
#
# Memory is the binding constraint here, not cores, so the default is deliberately
# well below the core count. Raise it with UMBRA_BUILD_JOBS if you have headroom.
JOBS="${UMBRA_BUILD_JOBS:-}"
if [[ -z "$JOBS" ]]; then
    CORES="$(nproc 2>/dev/null || echo 4)"
    JOBS=$(( CORES / 3 ))
    (( JOBS < 1 )) && JOBS=1
    (( JOBS > 6 )) && JOBS=6
fi

echo "Building with ${JOBS} parallel jobs (set UMBRA_BUILD_JOBS to change)..."

# Run at low priority too, so an interactive session stays responsive even while
# every one of those jobs is busy.
if command -v nice >/dev/null 2>&1; then
    nice -n 19 ninja -C "${BUILD_DIR}" -j "${JOBS}"
else
    ninja -C "${BUILD_DIR}" -j "${JOBS}"
fi

if [[ "$DO_PACKAGE" == "1" ]]; then
    echo "Packaging..."
    # NSIS is the installer the Umbra client bundle chains; see scripts/fetch-host.ps1
    # in the client repo.
    cpack -G NSIS --config "${BUILD_DIR}/CPackConfig.cmake"
fi

echo
echo "Build complete. Binaries are in ${BUILD_DIR}"
