#!/usr/bin/env bash
#
# Builds iina-decklink-helper into plugin/bin/.
#
# Usage: scripts/build_native.sh [Debug|Release]   (default Release)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${1:-Release}"
BUILD_DIR="$ROOT/native/build"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")

# Toolchain selection.
#
# A machine can end up with Xcode and Command Line Tools at different
# versions, where xcode-select points at Xcode's older clang while xcrun
# resolves to the CLT's newer SDK.  That combination fails to compile at all:
# recent libc++ headers use builtins (__builtin_clzg and friends) that older
# clangs don't have.  When the active SDK belongs to the CLT, use the CLT's
# clang++ with it so the pair is matched.
SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
CLT_CLANGXX="/Library/Developer/CommandLineTools/usr/bin/clang++"
if [[ "$SDK_PATH" == /Library/Developer/CommandLineTools/* && -x "$CLT_CLANGXX" ]]; then
    echo "==> Using Command Line Tools toolchain (matches the active SDK)"
    CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="$CLT_CLANGXX" -DCMAKE_OSX_SYSROOT="$SDK_PATH")
fi

if [[ ! -f "$ROOT/DeckLinkSDK/Mac/include/DeckLinkAPI.h" ]]; then
    echo "error: DeckLink SDK not found at $ROOT/DeckLinkSDK" >&2
    echo "       Download it from blackmagicdesign.com/support and unpack it there." >&2
    exit 1
fi

# Prefer the decode-only LGPL FFmpeg built by build_ffmpeg_lgpl.sh over
# whatever's on the system: Homebrew's ffmpeg is built --enable-gpl and links
# libx264/libx265, which puts a GPLv3 obligation on the whole release tarball
# for encoders this playback-only helper never calls. Falling back to the
# system ffmpeg keeps local dev working with no extra setup, but a release
# build should not ship it — see README's Releasing section.
FFMPEG_LGPL_PREFIX="$ROOT/native/third_party/ffmpeg-lgpl"
if [[ -f "$FFMPEG_LGPL_PREFIX/lib/pkgconfig/libavcodec.pc" ]]; then
    echo "==> Using decode-only LGPL FFmpeg at $FFMPEG_LGPL_PREFIX"
    export PKG_CONFIG_PATH="$FFMPEG_LGPL_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    FFMPEG_SELECTION="lgpl"
else
    echo "==> No LGPL FFmpeg found at $FFMPEG_LGPL_PREFIX — falling back to the"
    echo "    system FFmpeg via pkg-config. Fine for local dev; run"
    echo "    scripts/build_ffmpeg_lgpl.sh first before scripts/package.sh."
    FFMPEG_SELECTION="system"
fi

# pkg_check_modules' result is cached in CMakeCache.txt, so switching which
# FFmpeg is on PKG_CONFIG_PATH has no effect on an already-configured build
# directory. Wipe it whenever the recorded selection doesn't positively match
# this run's — including no marker at all, since a build directory from
# before this marker existed is exactly as stale as one recorded "system".
SELECTION_MARKER="$BUILD_DIR/.ffmpeg_selection"
if [[ -d "$BUILD_DIR" && "$(cat "$SELECTION_MARKER" 2>/dev/null)" != "$FFMPEG_SELECTION" ]]; then
    echo "==> FFmpeg selection changed ($(cat "$SELECTION_MARKER" 2>/dev/null || echo unknown) -> $FFMPEG_SELECTION) — reconfiguring"
    rm -rf "$BUILD_DIR"
fi

echo "==> Configuring ($BUILD_TYPE)"
cmake -S "$ROOT/native" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
mkdir -p "$BUILD_DIR"
echo "$FFMPEG_SELECTION" > "$SELECTION_MARKER"

echo "==> Building"
cmake --build "$BUILD_DIR" --parallel

echo "==> Built $ROOT/plugin/bin/iina-decklink-helper"
"$ROOT/plugin/bin/iina-decklink-helper" --version
