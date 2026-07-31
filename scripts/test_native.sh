#!/usr/bin/env bash
#
# Smoke-tests plugin/bin/iina-decklink-helper against a small committed test
# clip, using --null so it needs no DeckLink hardware — this is what CI runs.
# It's a floor, not a substitute for the manual hardware/hardware-adjacent
# testing described in README's "Testing without hardware" section; it exists
# to catch outright breakage (crashes, decode failures, a pixel format the
# trimmed LGPL FFmpeg build turns out not to carry) on every push.
#
# Usage: scripts/test_native.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HELPER="$ROOT/plugin/bin/iina-decklink-helper"
CLIP="$ROOT/native/testdata/sample.mp4"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$HELPER" ]]; then
    echo "error: $HELPER not found — run scripts/build_native.sh first" >&2
    exit 1
fi
if [[ ! -f "$CLIP" ]]; then
    echo "error: test clip missing at $CLIP" >&2
    exit 1
fi

fail=0
check() {
    local name="$1"; shift
    echo "==> $name"
    if "$@"; then
        echo "    ok"
    else
        echo "    FAILED: $name" >&2
        fail=1
    fi
}

# --play --null: decode + convert + simulated-output feed loop, exercising
# hardware decode, swscale, and the pulldown/cadence machinery end to end.
# Checked with Python rather than grep so a malformed/missing field is a
# clear assertion failure, not a silent pass.
test_play() {
    local pixfmt="$1" out
    out="$("$HELPER" --play "$CLIP" --null-fps 24 --duration 2 --pixfmt "$pixfmt" 2>"$TMP/play.err")" || return 1
    echo "$out" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert d['played'] >= 1.5, f\"played too little: {d['played']}\"
assert d['reseeks'] == 0, f\"unexpected reseeks: {d['reseeks']}\"
"
}
check "play (v210)"  test_play v210
check "play (uyvy)"  test_play uyvy
check "play (rgb10)" test_play rgb10
check "play (argb)"  test_play argb

# --still: the tiff-encoder + image2-muxer path Grab Still depends on, which
# is exactly the pair scripts/build_ffmpeg_lgpl.sh explicitly re-enables in an
# otherwise encoders-disabled build — the thing most likely to silently break
# if that build config ever drifts.
test_still() {
    local out="$TMP/still.tiff"
    "$HELPER" --still "$CLIP" --at 1.0 --out "$out" >/dev/null 2>&1 || return 1
    [[ -s "$out" ]] || return 1
    file "$out" | grep -qi tiff
}
check "grab still" test_still

# --dump: the canvas/padding path (broadcast-black fill, framing/scale),
# independent of any DeckLink or FFmpeg output plumbing.
test_dump() {
    local out="$TMP/canvas.ppm"
    "$HELPER" --dump "$CLIP" --size 640x360 --pixfmt uyvy --out "$out" >/dev/null 2>&1 || return 1
    [[ -s "$out" ]]
}
check "dump canvas" test_dump

if [[ $fail -ne 0 ]]; then
    echo
    echo "native smoke tests: FAILED"
    exit 1
fi
echo
echo "native smoke tests: all passed"
