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
//
// The same collision also happens *within* one context, which is subtler and
// cost us a diagnosis: index.js and helper-lib.js each require this module and
// each got their own module-level buffer, because require does not hand both
// of them the same instance. Since every line rewrites the whole file, the two
// buffers overwrote each other, and index.js — which logs far more often — won
// every time. The result was that not one line helper-lib.js ever logged
// reached the file, so when the helper failed to launch, the entire launch
// path went unrecorded and the failure looked like silence.
//
// Anchoring the state on the context's global object gives every instance in
// that context the same buffer, while the separate contexts stay separate,
// which is what setFile is for.
if (!globalThis.__decklinkLogState) {
  globalThis.__decklinkLogState = {
    path: "@data/decklink.log",
    buffer: "",
    broken: false,
  };
}
const state = globalThis.__decklinkLogState;

// Rewriting the whole buffer per line is quadratic in the number of lines, so
// the buffer is capped and the oldest lines fall off the front. A handful of
// lines a session never reaches this; a session where the helper is reporting
// a fault every few seconds for hours does, and that is exactly the session
// whose tail is worth keeping. The helper's own log (@data/helper.log) is
// appended to rather than rewritten and holds the detailed record.
const MAX_BUFFER = 256 * 1024;

function emit(level, text) {
  const line = `${new Date().toISOString()} [${level}] ${text}`;
  if (!state.broken) {
    state.buffer += line + "\n";
    if (state.buffer.length > MAX_BUFFER) {
      // Trim to a line boundary so the file never opens mid-line.
      const cut = state.buffer.indexOf("\n", state.buffer.length - MAX_BUFFER);
      state.buffer = state.buffer.slice(
        cut < 0 ? state.buffer.length - MAX_BUFFER : cut + 1,
      );
    }
    try {
      iina.file.write(state.path, state.buffer);
    } catch (e) {
      // A read-only data folder must not cost us the Log Viewer line too.
      state.broken = true;
    }
  }
  return line;
}

module.exports = {
  log: (text) => iina.console.log(emit("info", text)),
  warn: (text) => iina.console.warn(emit("warn", text)),
  error: (text) => iina.console.error(emit("error", text)),
  setFile: (name) => {
    state.path = `@data/${name}`;
  },
};
