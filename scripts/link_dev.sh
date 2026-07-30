#!/usr/bin/env bash
#
# Installs the plugin into IINA as a development plugin: builds the helper,
# drops it where the plugin expects to find it, and symlinks the plugin folder
# into IINA's plugin directory.
#
# IINA loads any symlink in its plugin folder whose name ends in
# .iinaplugin-dev, so edits here take effect on the next IINA launch with no
# packaging step.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDENTIFIER="com.thatcherfreeman.iina-decklink"
PLUGIN_DIR="$HOME/Library/Application Support/com.colliderli.iina/plugins"
LINK="$PLUGIN_DIR/iina-decklink.iinaplugin-dev"
# @data/ resolves here, which is where the plugin looks for the helper.
DATA_DIR="$PLUGIN_DIR/.data/$IDENTIFIER"

if [[ ! -d "$PLUGIN_DIR" ]]; then
    echo "error: IINA's plugin folder not found at $PLUGIN_DIR" >&2
    echo "       Launch IINA at least once first." >&2
    exit 1
fi

echo "==> Building the helper"
"$ROOT/scripts/build_native.sh" "${1:-Release}" >/dev/null

echo "==> Installing the helper into the plugin's data folder"
mkdir -p "$DATA_DIR/bin"
# Copy rather than symlink: the helper is re-signed in place by the packaging
# script, and a symlink would quietly re-sign the build output instead.
cp -f "$ROOT/plugin/bin/iina-decklink-helper" "$DATA_DIR/bin/iina-decklink-helper"
chmod +x "$DATA_DIR/bin/iina-decklink-helper"

# An unsigned binary won't execute on Apple Silicon at all, so ad-hoc sign it.
codesign --force --sign - "$DATA_DIR/bin/iina-decklink-helper" 2>/dev/null || true
xattr -cr "$DATA_DIR/bin/iina-decklink-helper" 2>/dev/null || true

echo "==> Linking the plugin"
if [[ -L "$LINK" ]]; then
    rm "$LINK"
elif [[ -e "$LINK" ]]; then
    echo "error: $LINK exists and is not a symlink; remove it first" >&2
    exit 1
fi
ln -s "$ROOT/plugin" "$LINK"

echo
echo "Installed:"
echo "  plugin  $LINK -> $ROOT/plugin"
echo "  helper  $DATA_DIR/bin/iina-decklink-helper"
echo
echo "Restart IINA, then enable it from Plugin → Send to DeckLink."
echo "Logs: Window → Log Viewer, subsystem \"DeckLink Output\"."
