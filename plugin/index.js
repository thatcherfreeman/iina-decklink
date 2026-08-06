// DeckLink Output — main entry, one instance per player window.
//
// The plugin doesn't touch video at all; it can't. It configures the helper
// process, tells it what to play, and keeps it informed of where IINA's
// playhead is. Everything else happens on the other side of the WebSocket.

const { core, mpv, event, menu, utils, standaloneWindow, file } = iina;

// Load the logger before anything else, so a failure in the modules below has
// somewhere durable to be recorded. If even this fails there is nothing to be
// done but fall back to the Log Viewer.
let console;
try {
  console = require("./log-lib.js");
} catch (e) {
  console = iina.console;
  iina.console.error(`could not load the log module: ${e}`);
}

let Settings, HelperLink, listDevices, listModes, findHelper, downloadHelper,
  helperLogPath;
try {
  Settings = require("./settings-lib.js");
  ({ HelperLink, listDevices, listModes, findHelper, downloadHelper,
     helperLogPath } = require("./helper-lib.js"));
} catch (e) {
  console.error(`could not load the plugin's modules: ${e}`);
  throw e;
}

// Earlier versions offered an "Exact mode" picker and persisted the choice.
// That override wins over the resolution setting and has no fallback, so a
// value left behind from then keeps pinning the output to one display mode —
// which looks exactly like the resolution setting doing nothing.
if (Settings.clearStaleMode())
  console.log("cleared a stale exact-mode preference; resolution now decides");

// How often IINA's position is reported to the helper. The helper extrapolates
// between reports off its own monotonic clock, so this only has to be often
// enough to correct accumulated error, not to drive playback.
const POSITION_INTERVAL_MS = 100;

const link = new HelperLink();
let settings = Settings.read();
let outputActive = false;   // the helper has the card open
let tickerId = null;
let lastStatus = null;

let toggleItem = null;

// ---------------------------------------------------------------------------
// Playback state
// ---------------------------------------------------------------------------

// The file the helper should open. mpv's `path` can be relative, so it is
// resolved against the working directory; URLs are passed through untouched
// for FFmpeg to open itself.
function mediaPath() {
  const path = mpv.getString("path");
  if (!path) return null;
  if (/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(path)) return path;
  if (path.startsWith("/")) return path;
  const cwd = mpv.getString("working-directory");
  return cwd ? `${cwd}/${path}` : path;
}

function safeNumber(name, fallback) {
  const value = mpv.getNumber(name);
  return typeof value === "number" && isFinite(value) ? value : fallback;
}

function playbackState() {
  return {
    position: safeNumber("time-pos", 0),
    speed: safeNumber("speed", 1),
    paused: mpv.getFlag("pause"),
  };
}

function sendPosition() {
  if (!link.isConnected() || !outputActive) return;
  const state = playbackState();
  link.send({
    cmd: "position",
    position: state.position,
    speed: state.speed,
    paused: state.paused,
  });
}

function startTicker() {
  if (tickerId) return;
  tickerId = setInterval(sendPosition, POSITION_INTERVAL_MS);
}

function stopTicker() {
  if (!tickerId) return;
  clearInterval(tickerId);
  tickerId = null;
}

// ---------------------------------------------------------------------------
// Output lifecycle
// ---------------------------------------------------------------------------
// What the helper was last told to play, so the same file isn't loaded twice.
// It matters because at startup two triggers coincide: window-loaded starts the
// helper, whose "ready" carries the first load, and file-loaded then arrives
// for the same file. A second load reopens the card, which shows on the wire as
// the output dropping out and coming back.
let loadedPath = null;

function loadCurrentMedia() {
  const path = mediaPath();
  if (!path || path === loadedPath) return;
  loadedPath = path;
  const state = playbackState();
  link.send({
    cmd: "load",
    path,
    position: state.position,
    speed: state.speed,
    paused: state.paused,
  });
}

// The helper reports the card's state on every status message; this turns the
// transitions into log lines and one OSD, so a dropout leaves a mark on IINA's
// side with a timestamp the user can match against what they saw. The running
// totals stay out of the log — @data/helper.log has those, once a second — and
// only changes are recorded here.
//
// A stall does not stop playback in IINA, and there is nothing this plugin can
// do about it, so the OSD is a notification and not a prompt: the user is the
// one who has to decide whether to toggle the output off and on again.
function noteHealthChange(before, now) {
  if (!now) return;
  const was = before || {};

  if (now.stalled && !was.stalled) {
    console.error(
      "the DeckLink stopped accepting frames — see helper.log for the card's " +
        "own counters at the moment it happened",
    );
    core.osd("DeckLink: the card stopped accepting frames");
  } else if (!now.stalled && was.stalled) {
    console.log("the DeckLink started accepting frames again");
  }

  if (now.playback_stopped && !was.playback_stopped)
    console.error("the DeckLink ended scheduled playback; it has to be reopened");

  for (const [key, what] of [
    ["card_errors", "scheduling calls the card refused"],
    ["card_flushed", "frames the driver flushed"],
    ["send_failures", "frames the output refused"],
  ]) {
    const delta = (now[key] || 0) - (was[key] || 0);
    if (delta > 0) console.warn(`${delta} ${what} in the last second`);
  }
}

function handleHelperEvent(message) {
  switch (message.event) {
    case "ready":
      console.log(`helper ready (version ${message.version})`);
      link.send(Settings.toConfigureMessage(settings));
      loadCurrentMedia();
      startTicker();
      break;

    case "started":
      outputActive = true;
      core.osd(
        `DeckLink: ${message.width}×${message.height} @ ${Number(message.fps).toFixed(3)}` +
          ` ${message.pixfmt}${message.hardware ? "" : " (software decode)"}`,
      );
      console.log(
        `output started: mode ${message.mode} ${message.width}x${message.height} ` +
          `@ ${message.fps} ${message.pixfmt}`,
      );
      break;

    case "stopped":
      outputActive = false;
      break;

    case "status":
      noteHealthChange(lastStatus, message);
      lastStatus = message;
      break;

    case "error":
      core.osd(`DeckLink: ${message.message}`);
      console.error(message.message);
      break;

    default:
      break;
  }
}

// Only one process can hold the card, and nothing here arbitrates that: the
// helper's own device-open failure is the guard. An earlier version had the
// global entry keep a registry of which window owned the device, but IINA only
// assigns an addressable label to player instances the global entry created
// itself — getLabel() returns null in a window the user opened — so the
// controller cannot reply to the window that asked, and the claim never
// resolved. The helper's error is clearer anyway, and it covers the case the
// registry never could: another application holding the card.
function startOutput() {
  // window-loaded and file-loaded both land here at launch. HelperLink.start
  // is idempotent, but returning here keeps the log honest about how many
  // times output was actually started.
  if (link.running) return;
  console.log("starting output");
  settings = Settings.read();

  if (!findHelper(settings)) {
    const wanted = utils.ask(
      "The DeckLink helper hasn't been installed yet.\n\n" +
        "Download it now? It's a small signed binary that owns the DeckLink " +
        "device and decodes alongside IINA.",
    );
    setEnabled(false);
    if (wanted) {
      core.osd("DeckLink: downloading helper…");
      downloadHelper().then((error) => {
        if (error) {
          core.osd(`DeckLink: ${error}`);
        } else {
          core.osd("DeckLink: helper installed");
          setEnabled(true);
        }
      });
    }
    return;
  }

  const ok = link.start(settings, {
    onEvent: handleHelperEvent,
    onLog: (line) => console.log(`helper: ${line}`),
    onError: (message) => {
      core.osd(`DeckLink: ${message}`);
      setEnabled(false);
    },
    onExit: (status, wasRunning) => {
      outputActive = false;
      loadedPath = null;   // the next helper starts knowing nothing
      stopTicker();
      if (wasRunning) {
        // The helper only exits on its own if it crashed or its control
        // connection went away, and either explains a picture that vanished —
        // so record what the card was last reported to be doing alongside it.
        console.warn(
          `helper exited unexpectedly (status ${status})` +
            (lastStatus
              ? `; last status: position ${lastStatus.position}, ` +
                `stalled ${!!lastStatus.stalled}, ` +
                `card errors ${lastStatus.card_errors || 0}`
              : "; it never reported a status"),
        );
        core.osd("DeckLink: output stopped");
        setEnabled(false);
      } else {
        console.log(`helper exited (status ${status})`);
      }
      lastStatus = null;
    },
  });

  if (!ok) setEnabled(false);
}

function stopOutput() {
  stopTicker();
  outputActive = false;
  lastStatus = null;
  loadedPath = null;
  link.stop();
}

function setEnabled(on) {
  settings.enabled = on;
  Settings.write({ enabled: on });
  if (toggleItem) {
    toggleItem.selected = on;
    menu.forceUpdate();
  }
  if (on) startOutput();
  else stopOutput();
}

// Pushes changed settings to a running helper. Mode, pixel format and link
// configuration can't be changed on an open device, so the helper reopens it
// and resumes at the current position.
function applySettings() {
  settings = Settings.read();
  if (link.isConnected()) link.send(Settings.toConfigureMessage(settings));
}

// ---------------------------------------------------------------------------
// Still capture — ported from youtube-decklink's "Grab Still"
// ---------------------------------------------------------------------------
// Windows forbids <>:"/\|?* and control characters in filenames; macOS and
// Linux allow more, but writing the strictest-common-denominator name keeps a
// capture portable if the still folder is ever synced or moved across
// platforms. Ported verbatim from youtube-decklink's _sanitize_filename.
const INVALID_FILENAME_CHARS = /[<>:"/\\|?*\x00-\x1f]/g;

function sanitizeFilename(name, maxLen = 120) {
  let s = String(name).replace(INVALID_FILENAME_CHARS, "");
  s = s.replace(/\s+/g, " ").trim();
  s = s.replace(/^[. ]+|[. ]+$/g, "");   // Windows disallows leading/trailing dots and spaces
  if (s.length > maxLen) s = s.slice(0, maxLen).replace(/[. ]+$/g, "");
  return s || "still";
}

// H:MM:SS once there's an hour on the clock, M:SS below that — ported
// verbatim from youtube-decklink's _fmt_time.
function formatTimestamp(seconds) {
  seconds = Math.max(0, Math.floor(seconds));
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  const pad = (n) => String(n).padStart(2, "0");
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
}

// youtube-decklink prompts a save panel on every capture, which is also how
// it lets a name collide with an existing file on purpose (the panel asks).
// IINA plugins have no save panel — chooseFile only opens existing files or
// directories — so instead the destination folder is chosen once (in the
// settings window) and each capture gets a name that can't collide: an
// incrementing " (2)", " (3)", ... suffix, the same convention macOS's own
// screenshot tool uses for exactly this reason.
function uniqueStillPath(dir, stem, ext) {
  let path = `${dir}/${stem}${ext}`;
  for (let n = 2; file.exists(path) && n < 1000; n++)
    path = `${dir}/${stem} (${n})${ext}`;
  return path;
}

// Grabs whatever frame the helper most recently put on the wire — not a fresh
// decode of IINA's own current frame, since the plugin has no access to that;
// this is the helper's independent decode, generally within a source frame or
// two of what IINA is showing (see the sync measurements in the README).
// Playback is paused for the duration, matching youtube-decklink's own
// _grab_still, so the picture on the monitor stays still while the capture is
// in flight rather than continuing to move under the user.
function grabStill() {
  if (!outputActive) {
    core.osd("DeckLink: turn on Send to DeckLink first");
    return;
  }

  const wasPaused = mpv.getFlag("pause");
  if (!wasPaused) core.pause();

  const dir = utils.resolvePath(settings.stillDir || "~/Desktop");
  const title = mpv.getString("media-title") || "";
  const firstWords = title.split(/\s+/).filter(Boolean).slice(0, 5).join(" ");
  const stem =
    `${sanitizeFilename(firstWords || "still")} ` +
    formatTimestamp(safeNumber("time-pos", 0)).replace(/:/g, "-");
  const path = uniqueStillPath(dir, stem, ".tiff");

  link.query("grab_still", "still", { path }).then((reply) => {
    if (!wasPaused) core.resume();
    if (reply && reply.ok) {
      core.osd(`DeckLink: saved ${path.split("/").pop()}`);
      console.log(`still saved: ${path}`);
    } else {
      const msg = reply ? reply.message : "the helper did not answer";
      core.osd(`DeckLink: could not save still (${msg})`);
      console.error(`still capture failed: ${msg}`);
    }
  });
}

// ---------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------
//
// IINA creates the plugin's standalone window — and with it the table its
// onMessage listeners live in — lazily.  A listener registered before the
// window exists is not an error and not a warning: the call succeeds and the
// listener is simply dropped.  The window then opens, the page loads, its
// postMessage calls return cleanly, and nothing ever arrives.  Registering
// after open() is measured to work; registering at plugin-load time is
// measured not to.  So every listener is registered here, on every open, and
// nothing outside this function may register one.
function openSettingsWindow() {
  try {
    // Reloading on each open costs nothing and means the page always starts
    // from a known state rather than whatever the last session left behind.
    standaloneWindow.loadFile("settings.html");
    standaloneWindow.setProperty({ title: "DeckLink Output", resizable: true });
    standaloneWindow.setFrame(560, 780);
    standaloneWindow.open();
    registerWindowHandlers();
  } catch (e) {
    console.error(`could not open the settings window: ${e}`);
    core.osd(`DeckLink: could not open settings (${e})`);
  }
  // The contents are not sent here.  loadFile is asynchronous, so the page has
  // not registered its own handlers yet and anything posted now is dropped —
  // which shows up as a window that opens on its placeholder text.  The page
  // announces itself with "ready" once it is listening, and this answers that.
}

function registerWindowHandlers() {
  standaloneWindow.onMessage("ready", () => {
    console.log("settings window is ready; sending its contents");
    sendSettingsToWindow();
  });

  // A script error in the page otherwise leaves it stuck on its placeholder
  // text with no trace anywhere, since the webview has no console we can read.
  standaloneWindow.onMessage("pageerror", (data) => {
    console.error(
      `settings page error at line ${data.line}:${data.column}: ${data.message}`,
    );
  });

  standaloneWindow.onMessage("save", (data) => {
    Settings.write(data);
    applySettings();
  });

  standaloneWindow.onMessage("refresh", () => {
    refreshDeviceList();
  });

  standaloneWindow.onMessage("chooseStillDir", () => {
    // chooseFile is the only file-system panel a plugin can open, and it
    // opens existing files or directories — there is no save panel, which is
    // why the still folder is chosen once here rather than per capture.
    let path;
    try {
      path = utils.chooseFile("Choose a folder for stills", { chooseDir: true });
    } catch (e) {
      return;   // the user cancelled the panel
    }
    if (!path) return;
    Settings.write({ stillDir: path });
    sendSettingsToWindow();
  });

  standaloneWindow.onMessage("revealStillDir", () => {
    const dir = utils.resolvePath(settings.stillDir || "~/Desktop");
    if (file.exists(dir)) file.showInFinder(dir);
    else core.osd(`DeckLink: ${dir} doesn't exist yet`);
  });

  standaloneWindow.onMessage("download", async () => {
    const error = await downloadHelper((text) =>
      standaloneWindow.postMessage("download-progress", { text }),
    );
    standaloneWindow.postMessage("helper", {
      installed: !!findHelper(settings),
      error,
    });
    if (!error) refreshDeviceList();
  });
}

function sendSettingsToWindow() {
  settings = Settings.read();
  standaloneWindow.postMessage("settings", {
    settings,
    options: {
      pixelFormats: Settings.PIXEL_FORMATS,
      levels: Settings.LEVELS,
      linkModes: Settings.LINK_MODES,
      framing: Settings.FRAMING,
      hwdec: Settings.HWDEC,
      onBlur: Settings.ON_BLUR,
      // No resolutions here on purpose: the only authority on what this card
      // can output is the card, and enumerating it is asynchronous. The page
      // shows a placeholder until the "resolutions" message arrives, rather
      // than offering sizes the hardware may not have.
    },
  });
  standaloneWindow.postMessage("helper", { installed: !!findHelper(settings) });
  refreshDeviceList();
}

// The distinct sizes the device can output, largest first.
//
// Only progressive modes are considered: the frame rate is negotiated per
// source once the size is fixed, and offering an interlaced entry would put a
// size in the list that the rest of the pipeline never produces.
function resolutionsFromModes(modes) {
  const seen = new Set();
  const list = [];
  for (const mode of modes || []) {
    if (!mode.progressive) continue;
    const value = `${mode.width}x${mode.height}`;
    if (seen.has(value)) continue;
    seen.add(value);
    list.push({ value, width: mode.width, height: mode.height });
  }
  list.sort((a, b) => b.width - a.width || b.height - a.height);
  if (list.length === 0) return [];
  const options = list.map((r) => ({
    value: r.value,
    label: `${r.width} × ${r.height}`,
  }));
  // Last, not first: a DeckLink is normally wired to a monitor of a known
  // fixed size, so guessing from the source is the exception.
  options.push({ value: "", label: "Auto (follow the source)" });
  return options;
}

async function refreshDeviceList() {
  try {
    // The link is passed so the query goes over the socket when output is
    // running; see the note on queryJSON in helper-lib.js.
    const devices = await listDevices(settings, link);
    console.log(
      `device scan: ${devices ? devices.devices.join(", ") || "none" : "helper query failed"}`,
    );
    standaloneWindow.postMessage("devices", devices || { driver: false, devices: [] });

    let resolutions = [];
    if (devices && devices.devices.length > 0) {
      const modes = await listModes(
        settings,
        settings.device || devices.devices[0],
        link,
      );
      resolutions = resolutionsFromModes(modes && modes.modes);
      console.log(
        `modes: ${resolutions.length ? resolutions.length - 1 : 0} output sizes`,
      );
    }
    // Sent unconditionally, including empty: the page cannot tell "still
    // scanning" from "nothing to offer" otherwise, and would sit on its
    // placeholder forever.
    standaloneWindow.postMessage("resolutions", { resolutions });
  } catch (e) {
    // An unhandled rejection here would leave the window on "Looking for
    // DeckLink devices…" with no explanation.
    console.error(`could not list DeckLink devices: ${e}`);
    standaloneWindow.postMessage("devices", { driver: false, devices: [] });
    standaloneWindow.postMessage("resolutions", { resolutions: [] });
  }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
toggleItem = menu.item(
  "Send to DeckLink",
  () => setEnabled(!settings.enabled),
  { selected: settings.enabled },
);
menu.addItem(toggleItem);
menu.addItem(menu.item("DeckLink Settings…", openSettingsWindow));
menu.addItem(menu.separator());
// "i" for "image", matching youtube-decklink's own shortcut for this. Plain,
// unmodified — IINA's default input.conf has nothing bound there.
menu.addItem(menu.item("Grab Still…", grabStill, { keyBinding: "i" }));
menu.addItem(menu.separator());
menu.addItem(
  menu.item("Show DeckLink Sync Status", () => {
    if (!outputActive || !lastStatus) {
      core.osd("DeckLink: output is not running");
      return;
    }
    const audio = lastStatus.audio
      ? `, audio ${(lastStatus.audio_frames / 48000).toFixed(0)}s` +
        (lastStatus.audio_silence
          ? ` (${(lastStatus.audio_silence / 48000).toFixed(2)}s silent)`
          : "")
      : ", no audio";
    core.osd(
      `DeckLink: ${lastStatus.error_ms.toFixed(1)}ms offset, ` +
        `${lastStatus.dropped} dropped, ${lastStatus.repeated} held, ` +
        `${lastStatus.reseeks} reseeks${audio}` +
        // Only when there is something wrong to report: on a healthy feed
        // these are all zero and saying so every time buries the numbers that
        // do move.
        (lastStatus.stalled ? ", CARD STALLED" : "") +
        (lastStatus.card_errors ? `, ${lastStatus.card_errors} card errors` : ""),
    );
  }),
);

// The one way a user can get at the logs after a dropout: both files live in
// the plugin's data folder, which is otherwise unreachable from IINA's UI.
menu.addItem(
  menu.item("Reveal DeckLink Logs…", () => {
    const dir = utils.resolvePath("@data");
    if (file.exists(dir)) {
      file.showInFinder(dir);
      console.log(`revealed the log folder: ${dir}`);
    } else {
      core.osd("DeckLink: no logs yet");
    }
  }),
);
console.log("menu items registered");

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
event.on("iina.file-loaded", () => {
  if (!settings.enabled) return;
  if (link.isConnected()) {
    loadCurrentMedia();
    startTicker();
  } else {
    startOutput();
  }
});

// mpv reports the seek before the new position is settled; playback-restart is
// the point at which time-pos is trustworthy again, so the helper is told
// there rather than here.
event.on("mpv.seek", () => {
  if (!outputActive) return;
  const state = playbackState();
  link.send({ cmd: "seek", position: state.position, speed: state.speed,
              paused: state.paused });
});

event.on("mpv.playback-restart", () => {
  if (!outputActive) return;
  const state = playbackState();
  link.send({ cmd: "seek", position: state.position, speed: state.speed,
              paused: state.paused });
});

event.on("mpv.pause.changed", () => {
  if (!outputActive) return;
  // sendPosition() alone is enough — and, unlike a separate paused-only
  // message with no position, it's actually correct: it carries the exact
  // position IINA is at *right now*, which is what lets the helper's clock
  // snap to the true paused frame instead of guessing one from wall-clock
  // time since the last periodic report (up to 100ms — several frames).
  sendPosition();
});

event.on("mpv.speed.changed", () => {
  if (!outputActive) return;
  sendPosition();
});

event.on("iina.window-will-close", () => {
  stopOutput();
});

// What happens to the card when this window stops being the frontmost one —
// see Settings.ON_BLUR for the choice between the two behaviours.
event.on("iina.window-main.changed", (isMain) => {
  if (settings.onBlur === "release") {
    if (!isMain && outputActive) {
      console.log("window is no longer main — releasing the device");
      stopOutput();
    } else if (isMain && settings.enabled && !link.isConnected()) {
      startOutput();
    }
    return;
  }

  if (settings.onBlur === "blackout") {
    // Ported from youtube-decklink's pause_for_focus_loss(): the card stays
    // open — nothing here calls stopOutput()/startOutput() — so it comes back
    // instantly and nothing else can grab it meanwhile. IINA's own playback
    // is untouched throughout; this affects only what the SDI feed shows.
    if (!outputActive || !link.isConnected()) return;
    link.send({ cmd: "blackout", on: !isMain });
    console.log(isMain ? "window is main again — un-blanking the output"
                        : "window is no longer main — blanking the output");
  }
});

if (settings.enabled) {
  // Wait for the window before touching the card, so any error has somewhere
  // to be displayed.
  event.on("iina.window-loaded", () => {
    if (settings.enabled) startOutput();
  });
}

// The helper's log path is recorded here so a bug report picked up from this
// file alone still says where the other half of the record is.
console.log(`DeckLink Output plugin loaded (helper log: ${helperLogPath()})`);
