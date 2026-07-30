// Reading and normalising the plugin's preferences.
//
// Everything the settings window and the helper protocol need comes through
// here, so the shape of a settings object is defined in exactly one place.

const { preferences } = iina;

const PIXEL_FORMATS = [
  { value: "uyvy", label: "8-bit YUV 4:2:2" },
  { value: "v210", label: "10-bit YUV 4:2:2" },
  { value: "argb", label: "8-bit RGB 4:4:4" },
  { value: "rgb10", label: "10-bit RGB 4:4:4" },
];

const LEVELS = [
  { value: "video", label: "Video (SMPTE legal)" },
  { value: "full", label: "Full" },
];

const LINK_MODES = [
  { value: "single", label: "Single link" },
  { value: "dual", label: "Dual link" },
  { value: "quad", label: "Quad link" },
];

const FRAMING = [
  { value: "fit", label: "Fit (letterbox)" },
  { value: "1:1", label: "1:1 centre (no resampling)" },
];

const HWDEC = [
  { value: "auto", label: "Auto" },
  { value: "vt", label: "VideoToolbox only" },
  { value: "software", label: "Software only" },
];

// What happens to the card when IINA isn't the frontmost app. A single choice
// rather than two independent checkboxes, since "release" and "blackout" are
// two different answers to the same question and enabling both at once would
// just mean the blackout is immediately undone by the release that follows it.
//
// "blackout" is the youtube-decklink behaviour: the card stays open (so
// nothing else can take it, and it comes back the instant focus returns) but
// shows black instead of a frozen frame, so a reference monitor left running
// unattended doesn't sit there burning in a still image. "release" is the
// older behaviour this plugin already had: hand the card to another
// application entirely, e.g. switching to Resolve.
const ON_BLUR = [
  { value: "nothing", label: "Do nothing" },
  { value: "blackout", label: "Blank to black" },
  { value: "release", label: "Release the device" },
];

// Accepts "" (follow the source) or a "WxH" pair; anything else is a stale or
// corrupt value and falls back to HD, which every SDI monitor takes.
function parseResolution(value) {
  if (value === "" || value === null || value === undefined) return "";
  return /^\d{3,5}x\d{3,5}$/.test(String(value)) ? String(value) : "1920x1080";
}

function oneOf(value, options, fallback) {
  return options.some((o) => o.value === value) ? value : fallback;
}

// Preserves whichever choice an installation already made under the old
// boolean release_on_blur, the first time this reads with no on_blur value of
// its own yet. Every other case — including a plugin that predates
// release_on_blur entirely — falls through to "nothing", the default both
// settings have always shared.
function defaultOnBlur() {
  return preferences.get("release_on_blur") ? "release" : "nothing";
}

function clampInt(value, min, max, fallback) {
  const n = parseInt(value, 10);
  if (isNaN(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

function read() {
  return {
    enabled: !!preferences.get("enabled"),
    device: preferences.get("device") || "",
    // An exact display mode is an escape hatch for the command line, not a
    // setting: it overrides the resolution outright and has no fallback, so a
    // stale value silently pins the output to something the monitor may not
    // lock to at all. It is deliberately never loaded from preferences.
    mode: "",
    // Not validated against a list of known sizes: the only authority on what
    // is valid is the device, and the settings window builds its menu from
    // what the hardware reports.
    lockResolution: parseResolution(preferences.get("lock_resolution")),
    pixfmt: oneOf(preferences.get("pixfmt"), PIXEL_FORMATS, "v210"),
    levels: oneOf(preferences.get("levels"), LEVELS, "video"),
    link: oneOf(preferences.get("link"), LINK_MODES, "single"),
    framing: oneOf(preferences.get("framing"), FRAMING, "fit"),
    hwdec: oneOf(preferences.get("hwdec"), HWDEC, "auto"),
    preroll: clampInt(preferences.get("preroll"), 1, 16, 3),
    offsetMs: clampInt(preferences.get("offset_ms"), -2000, 2000, 0),
    audio: !!preferences.get("audio"),
    audioChannels: clampInt(preferences.get("audio_channels"), 2, 16, 2),
    onBlur: oneOf(preferences.get("on_blur"), ON_BLUR, defaultOnBlur()),
    helperPath: preferences.get("helper_path") || "",
    // Kept as the user's raw choice — "~/Desktop", or whatever chooseFile
    // returned — and resolved to an absolute path only where it is actually
    // used, not here, so the settings window can keep showing "~/Desktop"
    // rather than a resolved-at-read-time absolute path that changes meaning
    // if the account's home directory ever does.
    stillDir: preferences.get("still_dir") || "~/Desktop",
  };
}

function write(partial) {
  const map = {
    enabled: "enabled",
    device: "device",
    lockResolution: "lock_resolution",
    pixfmt: "pixfmt",
    levels: "levels",
    link: "link",
    framing: "framing",
    hwdec: "hwdec",
    preroll: "preroll",
    offsetMs: "offset_ms",
    audio: "audio",
    audioChannels: "audio_channels",
    onBlur: "on_blur",
    helperPath: "helper_path",
    stillDir: "still_dir",
  };
  for (const key of Object.keys(partial)) {
    if (map[key] !== undefined) preferences.set(map[key], partial[key]);
  }
  preferences.sync();
}

// Settings in the shape the helper's "configure" command expects.
function toConfigureMessage(s) {
  const [w, h] = s.lockResolution ? s.lockResolution.split("x") : ["0", "0"];
  return {
    cmd: "configure",
    device: s.device,
    mode: s.mode,
    pixfmt: s.pixfmt,
    levels: s.levels,
    link: s.link,
    framing: s.framing,
    hwdec: s.hwdec,
    preroll: s.preroll,
    offset_ms: s.offsetMs,
    lock_w: parseInt(w, 10) || 0,
    lock_h: parseInt(h, 10) || 0,
    audio: s.audio,
    audio_channels: s.audioChannels,
  };
}

// Clears the exact-mode preference written by earlier versions, which exposed
// it in the settings window. Left in place it keeps overriding the resolution.
function clearStaleMode() {
  if (preferences.get("mode")) {
    preferences.set("mode", "");
    preferences.sync();
    return true;
  }
  return false;
}

module.exports = {
  PIXEL_FORMATS,
  LEVELS,
  LINK_MODES,
  FRAMING,
  HWDEC,
  ON_BLUR,
  clearStaleMode,
  read,
  write,
  toConfigureMessage,
};
