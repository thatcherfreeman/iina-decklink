#!/usr/bin/env bash
# Build, sign, and notarize the helper for a GitHub release.
#
# Everything runs locally on this machine: your Developer ID identity and
# notarization credentials come from a gitignored release.env plus your local
# keychain, and are never sent to GitHub. CI (.github/workflows/ci.yml) still
# builds macOS on every push as a compile check, but does not sign, notarize,
# or publish that build — see the comment at the top of that file.
#
# Usage:
#   ./scripts/release_macos.sh
#
# Builds whatever version is currently in plugin/Info.json's "version" field
# — bump that yourself and commit it before running this (see RELEASING.md);
# this script doesn't touch it, so the tag gh_release_macos.sh creates later
# always matches a real commit, not a version that only ever existed in an
# uncommitted working tree.
set -euo pipefail
cd "$(dirname "$0")/.."

ENV_FILE="release.env"
if [[ ! -f "$ENV_FILE" ]]; then
    echo "error: $ENV_FILE not found." >&2
    echo "  cp release.env.example release.env" >&2
    echo "  # then edit it — see RELEASING.md for one-time setup" >&2
    exit 1
fi
# release.env uses "KEY = value" (Makefile-style, values may contain spaces,
# colons, parens — e.g. "Developer ID Application: Name (TEAMID)"), not bash
# syntax, so it's parsed rather than sourced. Comments and blank lines skipped.
read_env_key() {
    awk -F' *= *' -v key="$1" '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        $1 == key { sub(/^[^=]*= */, ""); print; exit }
    ' "$ENV_FILE"
}
CODESIGN_IDENTITY="$(read_env_key CODESIGN_IDENTITY)"
NOTARY_PROFILE="$(read_env_key NOTARY_PROFILE)"
: "${CODESIGN_IDENTITY:?CODESIGN_IDENTITY not set in $ENV_FILE}"
: "${NOTARY_PROFILE:?NOTARY_PROFILE not set in $ENV_FILE}"

if [[ "$(uname)" != "Darwin" ]]; then
    echo "error: macOS release signing must run on macOS." >&2
    exit 1
fi

VERSION="$(python3 -c 'import json; print(json.load(open("plugin/Info.json"))["version"])')"
echo "Building version $VERSION"

echo
echo "== build, sign, notarize =="
SIGN_IDENTITY="$CODESIGN_IDENTITY" NOTARY_PROFILE="$NOTARY_PROFILE" ./scripts/package.sh

echo
echo "Done: dist/iina-decklink-helper-$(uname -m).tar.gz, dist/iina-decklink.iinaplgz"
echo "Next: ./scripts/gh_release_macos.sh"
