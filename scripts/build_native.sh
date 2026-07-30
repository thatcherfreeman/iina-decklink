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

echo "==> Configuring ($BUILD_TYPE)"
cmake -S "$ROOT/native" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

echo "==> Building"
cmake --build "$BUILD_DIR" --parallel

echo "==> Built $ROOT/plugin/bin/iina-decklink-helper"
"$ROOT/plugin/bin/iina-decklink-helper" --version
