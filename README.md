# IINA → DeckLink

Sends a clean feed of whatever IINA is playing to a Blackmagic DeckLink or
UltraStudio device, for viewing on a reference monitor.

No OSD, no subtitles, and no colour management: the picture is rescaled and
repacked into the output pixel format using the source's own matrix
coefficients, but primaries and transfer function are never touched — so HDR
material reaches the card carrying its original code values.

## How it works, and why

An IINA plugin is JavaScript in a sandbox. It has no access to decoded frames,
IINA's bundled libmpv has no DeckLink output, and its FFmpeg has no decklink
device compiled in. So the plugin cannot feed the card itself, however it's
written.

Instead the plugin drives a small companion process:

```
IINA ──▶ its own window and audio, untouched
  │
  │  decklink.iinaplugin (JavaScript)
  │    ├─ menu, settings window, preferences
  │    ├─ WebSocket server on 127.0.0.1
  │    └─ launches the helper
  ▼
iina-decklink-helper (C++)
  ├─ FFmpeg demux + decode, VideoToolbox accelerated
  ├─ swscale → uyvy / v210 / argb / r210
  └─ DeckLink scheduled playback ──▶ SDI ──▶ reference monitor
```

The helper decodes the same file independently and is slaved to IINA's
playhead. The plugin reports position ten times a second; between reports the
helper extrapolates off its own monotonic clock, and for every output frame
slot it picks the source frame nearest where IINA will be when that slot
reaches the wire.

Those reports are *filtered*, not obeyed. mpv's `time-pos` is quantised to the
frame it is displaying, so each report is up to half a source frame away from
the truth. Anchoring the clock to every one of them feeds that quantisation
into the feed loop, which corrects it by holding and dropping frames — a
23.976p source on a 23.976p output, which should sit at hold=1 forever, instead
accumulates repeats for as long as it plays. A routine report therefore only
nudges the clock; seeks, pauses and rate changes snap it.

That one mechanism also does frame-rate conversion: holding each source frame
for `mode_fps / src_fps` output frames *is* pulldown, so 23.976p on a 60p
output comes out as a clean 3:2 with no special case. Drift is corrected by
nudging the same accumulator, gently enough that the cadence isn't disturbed.

### Choosing the output size

A DeckLink is normally wired to one monitor of a known, fixed resolution, so
the output size is the user's decision and the *frame rate* is the thing worth
negotiating. Pick the resolution your monitor expects; the picture is scaled to
fit it, and the helper then finds the best rate the device offers for whatever
is playing. A 1080p film on a UHD monitor is upscaled rather than switching the
monitor's mode mid-playlist.

The list in the settings window is built from the display modes the device
actually reports, largest first — not a fixed menu. "Auto (follow the source)"
is still there, last, for the cases where you do want the output to track the
file.

Rate negotiation, once the size is fixed: exact match, then a whole-number
multiple, then the rate whose ratio to the source is nearest a clean pulldown
cadence — halves included, since 2.5 is 3:2 and the feed loop produces it
exactly. That last tier matters more than it sounds. A device with only
720p50/59.94/60 has no 23.976 at all, and picking the first mode listed handed
film to 720p50, a ratio of 2.086 and visible judder, when 720p59.94 was
available at exactly 2.5.

There is no "exact display mode" setting. It is available as `--mode` on the
helper's command line and nowhere else: it overrides the resolution outright
and has no fallback, so a value left behind in preferences silently pins the
output to one mode — which presents as the resolution setting appearing to do
nothing, and as a black screen if the monitor won't lock to it. Any such value
written by an earlier version is cleared on load.

### Measured sync

From `--play --null`, which reconstructs what was actually on the wire at each
instant and compares it against the master clock (20-second clips, 8-second
measurement window after warm-up):

| source → output | mean error | cadence |
| --- | --- | --- |
| 23.976p → 23.976p | +0.8 ms | all 1 |
| 23.976p → 59.94p | −7.2 ms | 96 × 3, 96 × 2 |
| 24p → 48p | −9.2 ms | all 2 |
| 24p → 60p | −7.4 ms | 96 × 3, 96 × 2 |
| 29.97p → 23.976p | +13 ms | all 1 |

All within about half a source frame.

Driven by a real IINA rather than the harness, the offset is larger: on an
UltraStudio 4K Mini playing 1080p23.976 HEVC to a 23.976p output, ninety
seconds of playback sat at **+35 ms, drifting by less than 3 ms end to end**,
with no repeated frames, no re-seeks, and every dropped frame accounted for in
the first second. The extra offset is IINA's own display pipeline, which sits
between the position it reports and the picture in its window.

It is a *standing* offset, not jitter, which is what the **Output offset**
setting exists to trim — set it against your monitor and it stays put.

### Embedded audio

The source's audio track is decoded alongside the video, resampled to the 48 kHz
signed 32-bit interleaved PCM the card takes, and mixed to 2, 8 or 16 channels.
IINA's own audio output is untouched — this is a separate feed for the monitor.

The card timestamps audio against the same stream-time axis as video, deriving
it from the video frame counter. So the invariant the feed has to hold is that
exactly one output frame's worth of samples goes out per scheduled output
frame; send too few or too many and the two axes separate permanently. That is
why audio is fed from inside `send_canvas()` rather than at each of the four
places a frame gets scheduled, and why the samples-per-frame remainder is
carried rather than rounded — 48000/29.97 is 1601.6, and rounding it away would
drift by 36 samples a second. `--play --null --audio` reports `audio_skew`, the
difference in samples between what was sent and what the displayed frames call
for; it is 0 across 23.976, 29.97, 48 and 59.94 output modes.

Within that constraint the audio cursor runs free at 1× and deliberately does
*not* follow the video servo's sub-frame trims: a correction invisible in the
picture would be an audible click. It is re-anchored to the picture only on a
seek, or if the two somehow drift a quarter-second apart.

One trap, if you touch this code: a decoder's output is contiguous by
construction, but container timestamps are not sample-accurate. Matroska's
timebase is a millisecond while a 512-sample AAC frame is 10.667 ms, so every
PTS is rounded by up to 24 samples. Deriving each chunk's position from its own
PTS made consecutive chunks overlap or leave a hole, and the feed filled those
holes with silence — 2.8% of every sample sent, which is continuous crackle.
Chunk times now come from a running sample count, with the PTS used only to
anchor the run and to spot a real discontinuity.

**Show DeckLink Sync Status** reports the two counters that matter, and each
means exactly one thing: `dropped` is source frames that never reached the
wire, `repeated` is output slots that re-showed the last frame because nothing
newer had been decoded in time. Pulldown holds are by design and are not
counted as repeats — `--play --null` prints the cadence histogram for those.

## Installing

**From IINA:** Preferences → Plugins → Install from GitHub, and enter
`thatcherfreeman/iina-decklink`. On first use the plugin offers to download the
helper for your architecture; the plugin itself carries no binaries.

Requires [Blackmagic Desktop Video](https://www.blackmagicdesign.com/support/).
Tested against Desktop Video 16.0.1, and the helper carries a compatibility
ladder for the 14.2.1 and 15.3.1 SDK eras so older driver installs work too.

## Using it

**Plugin → Send to DeckLink** turns the output on and off.
**Plugin → DeckLink Settings…** opens the settings window, which lists the
devices and display modes your hardware actually reports.
**Plugin → Grab Still…** (or press <kbd>I</kbd>) saves the frame currently on
the DeckLink output as a 16-bit RGB TIFF — ported from youtube-decklink's own
Grab Still. Playback pauses for the moment it takes to write the file and
resumes automatically. The still is the helper's own frame, not a fresh decode
of IINA's — it comes from the same independent decode that feeds the card, so
it can be a source frame or two off from what IINA's window is showing at that
instant (see Measured sync above), the same as the picture on your monitor.

There is no save panel — IINA plugins can only open an existing file or
directory, not save-as a new one — so the destination folder is chosen once,
in **Choose…** under Still capture in the settings window (default
`~/Desktop`), and each capture is named `<title> <timestamp>.tiff`,
incrementing a `(2)`, `(3)`, ... suffix on a collision rather than overwriting,
the same convention macOS's own screenshot tool uses.

| Setting | |
| --- | --- |
| Device | which card, when more than one is connected |
| Resolution | what your monitor expects; the picture is scaled to it and only the frame rate negotiates |
| SDI link | single / dual / quad — more links, more bandwidth |
| Pixel format | 8/10-bit YUV 4:2:2, 8/10-bit RGB 4:4:4 |
| Levels | Video (SMPTE legal) or Full |
| Framing | Fit letterboxes; 1:1 centres at native size with no resampling |
| Embedded audio | off, or 2/8/16 channels down the SDI, resampled to 48 kHz |
| Output offset | milliseconds, to line the monitor up against IINA's audio |
| Preroll | frames buffered on the card |
| Hardware decoding | Auto, VideoToolbox only, or software only |
| Release when not frontmost | hand the card back to another application by switching windows |

Hardware decode covers h264, hevc, vp9, av1, prores and mpeg2. VideoToolbox
can't do 4:4:4 or 12-bit, and has no path for prores_raw, VC-1 or DNxHD; those
fall back to software automatically.

## Building from source

Needs cmake, pkg-config, FFmpeg development headers (`brew install ffmpeg` is
fine for local dev — see "FFmpeg licensing" under Releasing for why a release
build uses a different one), and the
[DeckLink SDK](https://www.blackmagicdesign.com/support/) unpacked at
`DeckLinkSDK/` in the repo root. The SDK is behind a registration wall, so it
isn't committed here.

```sh
scripts/build_native.sh      # builds plugin/bin/iina-decklink-helper
scripts/link_dev.sh          # installs it into IINA as a dev plugin
```

Then restart IINA. `link_dev.sh` symlinks `plugin/` into IINA's plugin folder
with an `.iinaplugin-dev` suffix, so edits take effect on the next launch with
no packaging step.

Logs appear in Window → Log Viewer, and are also written to

```
~/Library/Application Support/com.colliderli.iina/plugins/
    .data/com.thatcherfreeman.iina-decklink/decklink.log
```

which is the copy worth attaching to a bug report: the Log Viewer is in-memory
and per-session, so it is empty by the time anyone thinks to look. Everything
the helper writes to stderr is relayed there too.

One trap worth knowing if you extend the settings window: IINA creates the
standalone window, and the table its `onMessage` listeners live in, lazily.
Registering a listener before the window exists is neither an error nor a
warning — the call succeeds, the listener is dropped, and the window then opens
and never answers. `index.js` registers all of them inside
`registerWindowHandlers()`, called after `standaloneWindow.open()`.

If Xcode and the Command Line Tools are at different versions, the build script
picks a matched compiler and SDK pair automatically — a mismatch otherwise
fails to compile at all, because recent libc++ headers use builtins that older
clangs don't have.

### Testing without hardware

The helper can simulate a card, which is how the feed loop is tested:

```sh
# geometry, padding colour and levels, written out as a PPM
plugin/bin/iina-decklink-helper --dump video.mkv --size 1920x1080 \
    --pixfmt v210 --framing fit --out canvas.ppm --raw canvas.raw

# the real feed loop against a simulated card, reporting cadence and sync
plugin/bin/iina-decklink-helper --play video.mkv --null-fps 59.94 --duration 8
```

`--raw` writes the canvas in its native layout, which is how the padding is
verified to be true broadcast black (Y=64, Cb=Cr=512 for 10-bit YUV) rather
than zeros — zero-filling a YUV canvas puts chroma at 0 and the bars come out
green.

## Releasing

```sh
SIGN_IDENTITY="Developer ID Application: … (TEAMID)" scripts/package.sh
```

Produces `dist/iina-decklink-helper-<arch>.tar.gz` — the helper plus its
FFmpeg libraries, install names rewritten to `@loader_path` so it depends on
nothing but system frameworks — and `dist/iina-decklink.iinaplgz`. Publish the
tarball as a release asset under exactly that filename; the plugin fetches it
by name. Notarize it, or users get a Gatekeeper prompt.

**FFmpeg licensing:** run `scripts/build_ffmpeg_lgpl.sh` once before
packaging. It builds a decode-only FFmpeg from source with no `--enable-gpl`
and no `--enable-nonfree`, into `native/third_party/ffmpeg-lgpl/`, and
`scripts/build_native.sh` links against it automatically whenever it's
present. Homebrew's ffmpeg, by contrast, is built `--enable-gpl
--enable-version3` and links x264/x265/lame — a GPLv3 obligation on the whole
bundle for encoders this playback-only helper never calls.
`scripts/package.sh` refuses to produce a tarball unless the helper was built
against the LGPL FFmpeg, and separately scans the bundle for x264/x265/lame
as a second check. The resulting tarball is about 9.4 MB (vs ~15 MB for the
GPL one) and carries `THIRD_PARTY_NOTICES.txt` plus the LGPL-2.1 license text,
since those are dynamically-linked LGPL libraries being redistributed.
HEVC, H.264, ProRes and DNxHD *decoding* is all native to libavcodec and
needs none of this — x264/x265/lame are encoders only, and Grab Still's own
encoding need (a TIFF) is native and LGPL too.

## Limitations

- **The file is decoded twice**, once by IINA and once by the helper.
  Hardware decode keeps that affordable on Apple Silicon; it will be tight on
  older Intel Macs.
- **Sync is servoed, not frame-locked.** See the measurements above. If you
  need genuine frame accuracy, the answer is a patched IINA, not a plugin.
- **Local files are the solid path.** Network URLs are passed to FFmpeg
  directly; streams that can't be opened twice won't work.
- **One card, one process.** Nothing in the plugin arbitrates this; the
  helper's own device-open failure does, and it surfaces as an OSD message. An
  earlier version had a global entry keep a registry of which window owned the
  card, but IINA only assigns an addressable label to player instances the
  global entry created itself — `getLabel()` returns null in a window the user
  opened — so the controller could never reply to the window that asked. The
  helper's error is clearer anyway, and it covers the case the registry never
  could: another application holding the card.
- **Embedded audio is muted away from 1× playback.** Paused, or at any speed
  but normal, the feed carries silence rather than a stretched or repeating
  fragment; matching mpv's pitch-corrected scrubbing is not something a clean
  reference feed should attempt. A seek costs about 0.1 s of silence while the
  decoder refills.
- **Grab Still has no save panel.** A plugin can open an existing file or
  directory, not save-as a new filename, so the destination is a folder chosen
  once rather than a name typed per capture. See "Using it" above.

## Credits

The DeckLink output — scheduled playback, the in-flight buffer FIFO, the v210
packer, the SDK version-compatibility ladder and the display-mode selection
ladder — is ported from the author's youtube-decklink project, which in turn
ports the DeckLink output written for mpv. Grab Still is also ported from
youtube-decklink: same rescale-only, source-matrix-only, 16-bit rgb48le TIFF
approach, adapted for the lack of a save panel in the plugin sandbox.

### Driver compatibility

The SDK version-compatibility ladder means the helper runs against Desktop
Video installs going back to the 14.2.1 SDK era, not just the current one: a
union'd wrapper holds whichever versioned `IDeckLinkOutput` `QueryInterface`
actually returns (current SDK and 15.3.1 share one vtable; 14.2.1 has its own,
missing `CreateVideoFrameWithBuffer`), and every method call dispatches through
whichever pointer is live. This is carried over unchanged from youtube-decklink
— same headers, same fallback chain — plus quad-link support and a fps-ladder
tie-break that favours a clean pulldown ratio over whichever mode is listed
first (see "Choosing the output size" above). Verified this session against
the current SDK on a real UltraStudio 4K Mini; the 14.2.1/15.3.1 code paths are
unchanged from the reference but weren't independently re-tested against an
old driver install, since none was available to test against.

## License

MIT — see `LICENSE`. Running as an IINA plugin doesn't change that: IINA
(GPLv3) treats plugins built against its documented plugin API as separate,
independent works under GPLv3 §5(d)'s aggregate clause, and this one goes
further than most by doing its actual work in a wholly separate helper
process with no linkage into IINA at all.

Bundled third-party components keep their own licenses and aren't covered by
the MIT grant above: the Blackmagic DeckLink SDK (permissive, see the license
header in `DeckLinkSDK/Mac/include/DeckLinkAPI.h` — not redistributed in this
repo, see `.gitignore`) and, in release builds, FFmpeg (LGPL-2.1+, built
decode-only with no GPL components — see "FFmpeg licensing" under Releasing).
