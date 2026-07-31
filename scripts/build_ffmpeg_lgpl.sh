#!/usr/bin/env bash
#
# Builds a decode-only, LGPL-only FFmpeg into native/third_party/ffmpeg-lgpl,
# for scripts/build_native.sh and scripts/package.sh to link against instead
# of Homebrew's FFmpeg.
#
# Homebrew's ffmpeg formula is built with --enable-gpl --enable-version3 and
# links libx264/libx265/libmp3lame. Those are GPL, and none of them are
# encoders this helper (a playback-only, decode-and-output tool) ever calls —
# bundling them just puts a GPLv3 obligation on the whole release tarball for
# no reason. This build carries none of that: the only non-decoder pieces
# enabled are the "tiff" encoder and "image2" muxer that Grab Still needs
# (both native to libavcodec/libavformat, both LGPL, no external library).
#
# --disable-autodetect matters here specifically because this machine also
# has that GPL Homebrew ffmpeg installed, which means libx264/libx265's
# pkg-config files are sitting right there on the system. Without
# --disable-autodetect, FFmpeg's configure silently enables anything it
# finds — quietly reintroducing the exact problem this script exists to
# avoid. With it, only libraries an explicit --enable-* names get used.
#
# Usage: scripts/build_ffmpeg_lgpl.sh [version]   (default 8.1.2)
set -euo pipefail

VERSION="${1:-8.1.2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
PREFIX="$ROOT/native/third_party/ffmpeg-lgpl"
JOBS="$(sysctl -n hw.ncpu)"
OPENSSL_PREFIX="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl)"

trap 'rm -rf "$WORK"' EXIT

rm -rf "$PREFIX"
cd "$WORK"

echo "==> Downloading FFmpeg $VERSION source"
curl -sL "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o ffmpeg.tar.xz
tar -xf ffmpeg.tar.xz
cd "ffmpeg-$VERSION"

echo "==> Configuring (decode-only, LGPL, no autodetected extras)"
./configure \
    --prefix="$PREFIX" \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-autodetect \
    --disable-gpl \
    --disable-nonfree \
    --disable-encoders \
    --enable-encoder=tiff \
    --disable-muxers \
    --enable-muxer=image2 \
    --disable-filters \
    --enable-filter=abuffer,abuffersink,atempo \
    --enable-videotoolbox \
    --enable-audiotoolbox \
    --enable-openssl \
    --extra-cflags="-I$OPENSSL_PREFIX/include" \
    --extra-ldflags="-L$OPENSSL_PREFIX/lib"

echo "==> Building ($JOBS jobs)"
make -j"$JOBS"

echo "==> Installing to $PREFIX"
make install

echo "==> Sanity-checking no GPL/nonfree component slipped in"
if strings "$PREFIX"/lib/libavcodec.*.dylib | grep -qE -- "--enable-(gpl|nonfree|libx264|libx265|libmp3lame)"; then
    echo "error: a GPL/nonfree flag ended up in the build config" >&2
    exit 1
fi

echo "==> Built $PREFIX"
echo "    scripts/build_native.sh will pick this up automatically."
