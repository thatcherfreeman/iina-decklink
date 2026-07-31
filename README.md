# IINA → DeckLink

An IINA plugin that sends whatever you're playing out through a Blackmagic
DeckLink or UltraStudio card, so you can watch it on a reference monitor over
SDI — the kind of setup colorists, editors, and anyone doing broadcast-monitor
QC need.

It's a clean feed: no on-screen controls, no subtitles, nothing overlaid. The
picture isn't color-managed either — it's rescaled to fit your monitor's
resolution, but the actual color values are passed through untouched, so HDR
material reaches the monitor exactly as encoded. IINA's own window and audio
keep working normally; this just adds a second, independent output.

## Installing

1. Install [Blackmagic Desktop Video](https://www.blackmagicdesign.com/support/)
   — this plugin needs it to talk to your card.
2. In IINA: **Preferences → Plugins → Install from GitHub**, and enter:

   ```
   thatcherfreeman/iina-decklink
   ```

3. The first time you turn the plugin on, it downloads a small helper program
   automatically.

## Using it

- **Plugin → Send to DeckLink** turns the output on and off.
- **Plugin → DeckLink Settings…** opens the settings window, where you pick
  your device and configure the output:

  | Setting | What it does |
  | --- | --- |
  | Device | Which card to use, if you have more than one |
  | Resolution | What your monitor expects — the picture is scaled to fit it |
  | SDI link | Single / dual / quad — higher-bandwidth signals may need more than one cable |
  | Pixel format | 8 or 10-bit YUV (4:2:2), or 8 or 10-bit RGB (4:4:4) |
  | Levels | Video (SMPTE legal range) or Full |
  | Framing | Fit (letterboxed) or 1:1 (centered, no scaling) |
  | Embedded audio | Off, or 2/8/16 channels sent down the same SDI cable |
  | Output offset | Nudge the output earlier/later (ms) to line it up with IINA's own audio |
  | Preroll | How many frames get buffered on the card |
  | Hardware decoding | Auto, VideoToolbox only, or software only |
  | Release when not frontmost | Give the card back to another app when you switch away from IINA |

- **Plugin → Grab Still…** (or press <kbd>I</kbd>) saves the current frame as
  a 16-bit TIFF. There's no save dialog — pick a destination folder once under
  **Choose…** in the settings window (defaults to `~/Desktop`), and each grab
  is saved there automatically, named after the clip and timestamp.

A few things worth knowing:

- Playback is closely synced to IINA but not frame-locked — the picture on
  your monitor can trail IINA's window by a source frame or two. The
  **Output offset** setting is there to compensate for any consistent lag.
- Audio over SDI goes silent while paused or playing at any speed other than
  1×, rather than trying to scrub or pitch-shift it.
- Only one program can use a given card at a time. If another app has it
  open, turning the plugin on will fail with a clear error rather than a
  silent black screen.
- This works with local files. Network streams are passed straight to the
  decoder, so anything that can't be opened twice at once won't work here.
