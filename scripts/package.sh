#!/usr/bin/env bash
#
# Produces the two release artifacts:
#
#   dist/iina-decklink-helper-<arch>.tar.gz   the helper plus its FFmpeg
#                                             libraries, for GitHub Releases
#   dist/iina-decklink.iinaplgz               the plugin itself, for users who
#                                             prefer a file to a repo URL
#
# The plugin is deliberately tiny and carries no binaries: IINA installs
# plugins by downloading the repository's contents, so the helper is fetched
# separately on first run.
#
# Signing: set SIGN_IDENTITY to a Developer ID Application identity, and
# NOTARY_PROFILE to a notarytool keychain-profile name, to produce a binary
# that opens with no Gatekeeper prompt on another machine. Without them the
# helper is ad-hoc signed, which runs locally but nowhere else. See
# RELEASING.md for one-time setup (certificate + notarytool credentials) —
# in normal use, run scripts/release_macos.sh instead of this directly.
#
#   SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
#       NOTARY_PROFILE="your-notary-profile" scripts/package.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/dist"
STAGE="$DIST/stage"
ARCH="$(uname -m)"
SIGN_IDENTITY="${SIGN_IDENTITY:-}"

rm -rf "$DIST"
mkdir -p "$STAGE"

echo "==> Building the helper"
"$ROOT/scripts/build_native.sh" Release >/dev/null
cp "$ROOT/plugin/bin/iina-decklink-helper" "$STAGE/"

# A release must never bundle Homebrew's ffmpeg: it's built --enable-gpl and
# links libx264/libx265, which puts a GPLv3 obligation on the whole tarball
# for encoders this playback-only helper never calls. build_native.sh only
# uses the decode-only LGPL build when scripts/build_ffmpeg_lgpl.sh has been
# run; refuse to package rather than silently ship the wrong one.
if [[ "$(cat "$ROOT/native/build/.ffmpeg_selection" 2>/dev/null)" != "lgpl" ]]; then
    echo "error: the helper wasn't built against the LGPL FFmpeg." >&2
    echo "       Run scripts/build_ffmpeg_lgpl.sh, then re-run this script." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Bundle the FFmpeg libraries.
#
# The helper links whatever FFmpeg it was built against, at an absolute path.
# Copy those libraries alongside the binary and rewrite the install names to
# @loader_path so the bundle is self-contained — it must not depend on
# Homebrew being installed, nor on IINA's own FFmpeg, whose major version
# changes independently of ours.
# ---------------------------------------------------------------------------
echo "==> Bundling libraries"
bundle_deps() {
    # Every one of these must be local: the recursive call below would
    # otherwise clobber the caller's loop variable, and the caller would then
    # rewrite the wrong install name and silently leave its own dependency
    # pointing outside the bundle.
    local target="$1"
    local deps dep base

    deps=$(otool -L "$target" | tail -n +2 | awk '{print $1}' \
           | grep -E "^($ROOT/native/third_party|/opt/homebrew|/usr/local)" || true)

    for dep in $deps; do
        base="$(basename "$dep")"
        # Rewrite first, so this reference is correct regardless of what the
        # recursion below does.
        install_name_tool -change "$dep" "@loader_path/$base" "$target" 2>/dev/null
        if [[ ! -f "$STAGE/$base" ]]; then
            cp -f "$dep" "$STAGE/$base"
            chmod u+w "$STAGE/$base"
            install_name_tool -id "@loader_path/$base" "$STAGE/$base" 2>/dev/null
            # The libraries carry their own absolute references.
            bundle_deps "$STAGE/$base"
        fi
    done
}
bundle_deps "$STAGE/iina-decklink-helper"

echo "==> Verifying nothing outside the bundle is referenced"
leftover=$(otool -L "$STAGE/iina-decklink-helper" | tail -n +2 | awk '{print $1}' \
           | grep -E "^($ROOT/native/third_party|/opt/homebrew|/usr/local)" || true)
if [[ -n "$leftover" ]]; then
    echo "error: unbundled dependencies remain:" >&2
    echo "$leftover" >&2
    exit 1
fi

# Belt and suspenders on top of the .ffmpeg_selection check above: even
# having built against the right FFmpeg, make sure no GPL-only library
# somehow ended up in the bundle.
gpl_libs=$(ls "$STAGE"/*.dylib 2>/dev/null | grep -iE 'x264|x265|mp3lame' || true)
if [[ -n "$gpl_libs" ]]; then
    echo "error: GPL-licensed libraries ended up in the release bundle:" >&2
    echo "$gpl_libs" >&2
    exit 1
fi

# LGPL requires the license text travel with the binary, and (since these
# libraries aren't modified from upstream) a pointer to that exact source is
# enough to satisfy the source-availability obligation.
FFMPEG_VERSION="$(sed -n 's/.*FFMPEG_VERSION "\(.*\)"/\1/p' \
    "$ROOT/native/third_party/ffmpeg-lgpl/include/libavutil/ffversion.h" 2>/dev/null || true)"
cat > "$STAGE/THIRD_PARTY_NOTICES.txt" <<EOF
This build bundles FFmpeg shared libraries (libavformat, libavcodec,
libavfilter, libavutil, libswscale, libswresample), built decode-only with
no GPL or nonfree components (see scripts/build_ffmpeg_lgpl.sh in the
iina-decklink source repository for the exact build configuration used).
They are unmodified upstream FFmpeg and are licensed under the GNU Lesser
General Public License version 2.1 or later (LGPL-2.1+); see LGPL-2.1.txt
in this directory for the full license text.

Corresponding source: https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION:-<version>}.tar.xz
FFmpeg project: https://ffmpeg.org/

These libraries are dynamically linked (see the .dylib files alongside the
iina-decklink-helper binary) so they can be replaced with a compatible build
of your own.
EOF
curl -sL "https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt" \
    -o "$STAGE/LGPL-2.1.txt" || echo "warning: could not fetch LGPL text for bundling" >&2

# ---------------------------------------------------------------------------
# Sign.  Libraries first, then the executable, which is the order codesign
# requires for a valid bundle.
# ---------------------------------------------------------------------------
if [[ -n "$SIGN_IDENTITY" ]]; then
    echo "==> Signing with Developer ID"
    for lib in "$STAGE"/*.dylib; do
        [[ -e "$lib" ]] || continue
        codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" "$lib"
    done
    # Only the executable needs the library-validation exception: it's the
    # one that dlopens Blackmagic's DeckLinkAPI.framework (a different Team
    # ID) at runtime. Without it, a hardened-runtime binary passes
    # notarization fine and then finds zero devices on every machine it runs
    # on — the framework load is silently refused.
    codesign --force --timestamp --options runtime \
        --entitlements "$ROOT/scripts/release.entitlements" \
        --sign "$SIGN_IDENTITY" "$STAGE/iina-decklink-helper"

    echo "==> Verifying the signature"
    codesign --verify --deep --strict --verbose=2 "$STAGE/iina-decklink-helper"

    if [[ -n "${NOTARY_PROFILE:-}" ]]; then
        echo "==> Notarizing (this can take a few minutes)"
        # notarytool takes a zip/dmg/pkg, never a bare binary or a .tar.gz —
        # this zip exists only to satisfy that; the release tarball built
        # below is unaffected by notarization (it doesn't modify the signed
        # files) so there's nothing to redo afterward.
        NOTARIZE_ZIP="$DIST/notarize.zip"
        ditto -c -k --keepParent "$STAGE" "$NOTARIZE_ZIP"
        xcrun notarytool submit "$NOTARIZE_ZIP" \
            --keychain-profile "$NOTARY_PROFILE" --wait
        rm -f "$NOTARIZE_ZIP"
        # Not stapled: stapler only attaches to an .app/.pkg/.dmg container,
        # and this ships as a bare binary + dylibs. Gatekeeper instead
        # confirms the notarization ticket online on first launch — the
        # normal, Apple-documented path for a notarized command-line tool
        # that isn't bundled as a .app. That first launch already needs
        # network (it was just downloaded from GitHub), so this costs
        # nothing in practice.
    else
        echo "==> Skipping notarization (set NOTARY_PROFILE to notarize —"
        echo "    see RELEASING.md). Signed but unnotarized; Gatekeeper will"
        echo "    still block this build on another machine."
    fi
else
    echo "==> Ad-hoc signing (set SIGN_IDENTITY for a distributable build)"
    for lib in "$STAGE"/*.dylib; do
        [[ -e "$lib" ]] || continue
        codesign --force --sign - "$lib"
    done
    codesign --force --sign - "$STAGE/iina-decklink-helper"
fi

echo "==> Checking the packaged helper runs"
"$STAGE/iina-decklink-helper" --version >/dev/null

if [[ -n "$SIGN_IDENTITY" ]]; then
    echo "==> Gatekeeper check"
    spctl -a -vvv -t exec "$STAGE/iina-decklink-helper" || true
fi

TARBALL="$DIST/iina-decklink-helper-$ARCH.tar.gz"
tar -czf "$TARBALL" -C "$STAGE" .
rm -rf "$STAGE"

# ---------------------------------------------------------------------------
# The plugin package: everything except the local build output.
# ---------------------------------------------------------------------------
echo "==> Packing the plugin"
PLGZ="$DIST/iina-decklink.iinaplgz"
( cd "$ROOT/plugin" && zip -q -r "$PLGZ" . -x "bin/*" ".DS_Store" )

echo
echo "Artifacts:"
ls -lh "$DIST" | tail -n +2 | awk '{printf "  %-40s %s\n", $9, $5}'
echo
echo "Publish the tarball as a GitHub release asset; the plugin downloads it"
echo "by that exact filename on first run."
