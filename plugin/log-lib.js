// Everything the plugin logs goes both to IINA's Log Viewer and to
// @data/decklink.log.
//
// The Log Viewer is the intended channel, but it is in-memory and per-session:
// useless for a bug report, and awkward while developing because a plugin that
// fails during load has usually stopped logging before the window is open. The
// file is rewritten whole on every line — iina.file has no append — which is
// affordable only because this log is a handful of lines per session, not a
// per-frame trace.

// The main entry and the global entry run in separate JavaScript contexts, so
// each gets its own copy of this module and its own buffer. They must not
// write the same file or they would overwrite each other's history; the global
// entry calls setFile to move aside.
let path = "@data/decklink.log";
let buffer = "";
let broken = false;

// Rewriting the whole buffer per line is quadratic in the number of lines, so
// the buffer is capped and the oldest lines fall off the front. A handful of
// lines a session never reaches this; a session where the helper is reporting
// a fault every few seconds for hours does, and that is exactly the session
// whose tail is worth keeping. The helper's own log (@data/helper.log) is
// appended to rather than rewritten and holds the detailed record.
const MAX_BUFFER = 256 * 1024;

function emit(level, text) {
  const line = `${new Date().toISOString()} [${level}] ${text}`;
  if (!broken) {
    buffer += line + "\n";
    if (buffer.length > MAX_BUFFER) {
      // Trim to a line boundary so the file never opens mid-line.
      const cut = buffer.indexOf("\n", buffer.length - MAX_BUFFER);
      buffer = buffer.slice(cut < 0 ? buffer.length - MAX_BUFFER : cut + 1);
    }
    try {
      iina.file.write(path, buffer);
    } catch (e) {
      // A read-only data folder must not cost us the Log Viewer line too.
      broken = true;
    }
  }
  return line;
}

module.exports = {
  log: (text) => iina.console.log(emit("info", text)),
  warn: (text) => iina.console.warn(emit("warn", text)),
  error: (text) => iina.console.error(emit("error", text)),
  setFile: (name) => {
    path = `@data/${name}`;
  },
};
