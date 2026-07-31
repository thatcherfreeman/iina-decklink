// The link to iina-decklink-helper: a WebSocket server the helper connects
// back to, plus the process itself.
//
// The server lives here rather than in the helper because iina.ws only offers
// a server — there is no raw socket API in a plugin.  That turns out to be the
// right way round anyway: utils.exec gives us no handle on the process we
// started and no way to kill it, so the helper is written to exit when this
// connection drops.  Closing the server is therefore how the device gets
// released, and it is the only mechanism that works when IINA quits abruptly.

const { utils, ws, http, file } = iina;
const console = require("./log-lib.js");

const HELPER_RELATIVE = "@data/bin/iina-decklink-helper";

// The helper ships separately from the plugin rather than inside it.  IINA
// installs plugins by downloading the repository's contents, so committing a
// ~10 MB signed binary (plus its FFmpeg libraries) would bloat every clone and
// every update.  Fetching it on first run keeps the plugin itself tiny and
// matches what IINA's own Online Media plugin does for yt-dlp.
const RELEASE_BASE =
  "https://github.com/thatcherfreeman/iina-decklink/releases/latest/download";

// Chosen from the ephemeral range.  If the port is taken the server reports
// "failed" asynchronously, so a fresh one is tried rather than giving up.
function randomPort() {
  return 49152 + Math.floor(Math.random() * 16000);
}

function findHelper(settings) {
  const candidates = [settings.helperPath, HELPER_RELATIVE].filter(Boolean);
  for (const candidate of candidates) {
    if (utils.fileInPath(candidate)) return candidate;
  }
  return null;
}

// How long to wait for the helper to answer a query before giving up. It is
// answering off a local socket, so this only has to cover the driver's own
// enumeration call.
const QUERY_TIMEOUT_MS = 4000;

class HelperLink {
  constructor() {
    this.conn = null;
    this.port = null;
    this.running = false;
    this.serverStarted = false;
    this.handlers = {};
    this.attempts = 0;
    // Event name → callbacks waiting for the next one, for query().
    this.waiters = {};
  }

  // handlers: { onEvent(obj), onExit(status), onLog(line) }
  start(settings, handlers) {
    if (this.running) return true;
    this.handlers = handlers || {};

    const helper = findHelper(settings);
    if (!helper) {
      this._fail(
        "The DeckLink helper isn't installed yet. Open Preferences → DeckLink " +
          "Output and use “Download helper”, or point it at a local build.",
      );
      return false;
    }
    this.helper = helper;
    this.running = true;
    this.attempts = 0;
    this._startServer();
    return true;
  }

  _startServer() {
    this.port = randomPort();
    this.conn = null;

    // These callbacks are registered once per server instance.  A "failed"
    // state can arrive after startServer() returns — a port collision shows up
    // that way rather than as a thrown error — so retrying has to happen here.
    ws.onStateUpdate((state, error) => {
      if (state === "ready") {
        console.log(`control server listening on 127.0.0.1:${this.port}`);
        this._launchHelper();
      } else if (state === "failed" || state === "cancelled") {
        const message = error ? error.message : state;
        if (this.running && this.attempts < 5) {
          this.attempts++;
          console.log(`control server ${state} (${message}); retrying on a new port`);
          this._startServer();
        } else if (this.running) {
          this._fail(`Could not open a control port for the helper: ${message}`);
        }
      }
    });

    ws.onNewConnection((conn) => {
      console.log(`helper connected (${conn})`);
      this.conn = conn;
      if (this.handlers.onConnect) this.handlers.onConnect();
    });

    ws.onConnectionStateUpdate((conn, state) => {
      if ((state === "failed" || state === "cancelled") && conn === this.conn) {
        console.log(`helper connection ${state}`);
        this.conn = null;
      }
    });

    ws.onMessage((_conn, message) => {
      let obj;
      try {
        obj = JSON.parse(message.text());
      } catch (e) {
        console.warn(`ignoring unparseable helper message: ${e}`);
        return;
      }
      const waiting = this.waiters[obj.event];
      if (waiting && waiting.length) {
        waiting.shift()(obj);
        return;
      }
      if (this.handlers.onEvent) this.handlers.onEvent(obj);
    });

    try {
      ws.createServer({ port: this.port });
      ws.startServer();
      this.serverStarted = true;
    } catch (e) {
      this._fail(`Could not create the control server: ${e}`);
    }
  }

  _launchHelper() {
    const url = `ws://127.0.0.1:${this.port}/`;
    console.log(`launching helper: ${this.helper} --connect ${url}`);

    // This promise doesn't settle until the helper exits, which is exactly how
    // the exit is detected — there is no other signal available.
    utils
      .exec(
        this.helper,
        ["--connect", url],
        null,
        (out) => {
          if (out.trim() && this.handlers.onLog) this.handlers.onLog(out.trim());
        },
        (err) => {
          for (const line of err.split("\n")) {
            if (line.trim() && this.handlers.onLog) this.handlers.onLog(line.trim());
          }
        },
      )
      .then((result) => {
        this.conn = null;
        const wasRunning = this.running;
        this.running = false;
        if (this.handlers.onExit) this.handlers.onExit(result.status, wasRunning);
      })
      .catch((e) => {
        this.conn = null;
        this.running = false;
        this._fail(`Could not run the helper: ${e}`);
      });
  }

  _fail(message) {
    console.error(message);
    this.running = false;
    if (this.handlers.onError) this.handlers.onError(message);
  }

  send(obj) {
    if (!this.conn) return false;
    try {
      ws.sendText(this.conn, JSON.stringify(obj));
      return true;
    } catch (e) {
      console.warn(`could not send to helper: ${e}`);
      return false;
    }
  }

  isConnected() {
    return !!this.conn;
  }

  // Sends a command and resolves with the next event of the given name, or
  // null if the helper doesn't answer. Resolves rather than rejects: every
  // caller wants "no answer" to look the same as "no devices".
  query(cmd, event, extra) {
    return new Promise((resolve) => {
      if (!this.send(Object.assign({ cmd }, extra || {}))) {
        resolve(null);
        return;
      }
      let settled = false;
      const finish = (value) => {
        if (settled) return;
        settled = true;
        resolve(value);
      };
      (this.waiters[event] = this.waiters[event] || []).push(finish);
      setTimeout(() => {
        const queue = this.waiters[event];
        if (queue) {
          const at = queue.indexOf(finish);
          if (at >= 0) queue.splice(at, 1);
        }
        if (!settled) console.warn(`helper did not answer ${cmd} in time`);
        finish(null);
      }, QUERY_TIMEOUT_MS);
    });
  }

  // Asks the helper to quit, then drops the connection.  Either is enough on
  // its own — the helper exits when the socket closes — but the explicit quit
  // releases the card a beat sooner.
  stop() {
    if (this.conn) this.send({ cmd: "quit" });
    this.running = false;
    this.conn = null;
  }
}

// One-shot invocations for the settings window, which needs the device and
// mode lists without holding the card open.
//
// These are only usable when no helper is running. IINA's utils.exec is
// serialised, and a running helper is itself an outstanding exec that does not
// return until output stops, so a one-shot launched behind it never resolves
// at all — the settings window would sit on "Looking for DeckLink devices…"
// forever. listDevices/listModes below take a link and ask over the socket
// when there is one.
async function queryJSON(settings, args) {
  const helper = findHelper(settings);
  if (!helper) return null;
  try {
    const { status, stdout } = await utils.exec(helper, args);
    if (status !== 0 && !stdout) return null;
    return JSON.parse(stdout);
  } catch (e) {
    console.error(`helper query failed: ${e}`);
    return null;
  }
}

// Downloads and unpacks the helper for this machine's architecture.
// Returns null on success, or a message describing what went wrong.
async function downloadHelper(onProgress) {
  const report = (text) => {
    console.log(text);
    if (onProgress) onProgress(text);
  };

  let arch = "arm64";
  try {
    const { status, stdout } = await utils.exec("/bin/bash", ["-c", "uname -m"]);
    if (status === 0 && stdout.trim() === "x86_64") arch = "x86_64";
  } catch (e) {
    console.warn(`could not determine architecture, assuming arm64: ${e}`);
  }

  const name = `iina-decklink-helper-${arch}.tar.gz`;
  const url = `${RELEASE_BASE}/${name}`;
  const archive = utils.resolvePath(`@data/${name}`);
  const binDir = utils.resolvePath("@data/bin");

  try {
    report(`Downloading ${name}…`);
    await http.download(url, archive);

    report("Unpacking…");
    const { status, stderr } = await utils.exec("/bin/bash", [
      "-c",
      `set -e
       mkdir -p "${binDir}"
       tar -xzf "${archive}" -C "${binDir}"
       chmod +x "${binDir}/iina-decklink-helper"`,
    ]);
    if (status !== 0) return `Could not unpack the helper: ${stderr}`;

    if (!utils.fileInPath(HELPER_RELATIVE))
      return "The archive didn't contain iina-decklink-helper.";

    // Confirm it actually runs before declaring success — a wrong-architecture
    // or unsigned binary fails here rather than at the first playback attempt.
    const check = await utils.exec(HELPER_RELATIVE, ["--version"]);
    if (check.status !== 0)
      return `The helper was installed but won't run: ${check.stderr || check.status}`;

    report("Helper installed.");
    return null;
  } catch (e) {
    return `Download failed: ${e}`;
  } finally {
    try {
      if (file.exists(archive)) file.delete(archive);
    } catch (e) {
      console.warn(`could not remove the temporary archive: ${e}`);
    }
  }
}

function listDevices(settings, link) {
  if (link && link.isConnected()) return link.query("list-devices", "devices");
  return queryJSON(settings, ["--list-devices"]);
}

function listModes(settings, device, link) {
  if (link && link.isConnected())
    return link.query("list-modes", "modes", { device: device || "" });
  const args = ["--list-modes"];
  if (device) args.push("--device", device);
  return queryJSON(settings, args);
}

module.exports = {
  HelperLink,
  findHelper,
  downloadHelper,
  listDevices,
  listModes,
  HELPER_RELATIVE,
};
