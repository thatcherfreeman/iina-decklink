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
# Signing: set SIGN_IDENTITY to a Developer ID Application identity to produce
# a binary that opens with no Gatekeeper prompt.  Without it the helper is
# ad-hoc signed, which runs locally but will be blocked on other machines.
#
#   SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" scripts/package.sh
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
           | grep -E '^(/opt/homebrew|/usr/local)' || true)

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
           | grep -E '^(/opt/homebrew|/usr/local)' || true)
if [[ -n "$leftover" ]]; then
    echo "error: unbundled dependencies remain:" >&2
    echo "$leftover" >&2
    exit 1
fi

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
    codesign --force --timestamp --options runtime --sign "$SIGN_IDENTITY" \
        "$STAGE/iina-decklink-helper"
    echo "    Notarize before publishing:"
    echo "      ditto -c -k --keepParent $STAGE notarize.zip"
    echo "      xcrun notarytool submit notarize.zip --keychain-profile <profile> --wait"
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
