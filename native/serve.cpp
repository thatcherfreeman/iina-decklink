#include "serve.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "enumerate.h"
#include "json.h"
#include "log.h"
#include "ws_client.h"

namespace {

// If the plugin goes quiet for this long, assume it is gone and exit.
//
// This matters more than it looks.  utils.exec gives the plugin no way to kill
// a process it started, so the helper has to decide for itself when to leave —
// and a helper that outlives its plugin holds the DeckLink open with nothing
// able to reclaim it.  The plugin sends position reports several times a
// second, so silence for this long is unambiguous.
constexpr double kWatchdogSeconds = 5.0;

// How often to report sync state back to the plugin.
constexpr auto kStatusInterval = std::chrono::milliseconds(1000);

const char *pixfmt_name(int pixfmt)
{
    switch (pixfmt) {
    case DLK_PIXFMT_UYVY:  return "uyvy";
    case DLK_PIXFMT_V210:  return "v210";
    case DLK_PIXFMT_ARGB:  return "argb";
    case DLK_PIXFMT_RGB10: return "rgb10";
    default:               return "unknown";
    }
}

bool parse_pixfmt_name(const std::string &s, int *out)
{
    if (s == "uyvy")  { *out = DLK_PIXFMT_UYVY;  return true; }
    if (s == "v210")  { *out = DLK_PIXFMT_V210;  return true; }
    if (s == "argb")  { *out = DLK_PIXFMT_ARGB;  return true; }
    if (s == "rgb10") { *out = DLK_PIXFMT_RGB10; return true; }
    return false;
}

class Session {
public:
    Session(WsClient *ws, const OutputConfig &defaults)
        : ws_(ws), cfg_(defaults) {}

    void handle(const std::string &text);
    void tick_status();
    bool should_quit() const { return quit_; }

private:
    void send_event(const std::string &json);
    void send_error(const std::string &message);
    void send_enumeration(const std::string &event, const std::string &payload);
    void send_started();
    void apply_config(const JsonObject &msg);
    void do_load(const JsonObject &msg);
    void restart_for_config();

    WsClient    *ws_;
    OutputConfig cfg_;
    Player       player_;
    std::string  path_;
    std::mutex   mutex_;
    std::atomic<bool> quit_{false};
    std::chrono::steady_clock::time_point last_status_{};
};

void Session::send_event(const std::string &json)
{
    if (ws_)
        ws_->send(json);
}

// devices_json()/modes_json() already produce a complete object; splicing the
// event name in beside its fields keeps the wire format the same shape as
// every other event, and identical to what the one-shot CLI prints.
void Session::send_enumeration(const std::string &event, const std::string &payload)
{
    if (payload.size() < 2 || payload.front() != '{') {
        send_error("could not enumerate " + event);
        return;
    }
    send_event("{\"event\":\"" + event + "\"," + payload.substr(1));
}

void Session::send_error(const std::string &message)
{
    log_error("%s", message.c_str());
    send_event("{\"event\":\"error\",\"message\":\"" +
               json_escape_string(message) + "\"}");
}

void Session::send_started()
{
    PlayerStatus s = player_.status();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"started\",\"mode\":\"%s\",\"width\":%d,\"height\":%d,"
             "\"fps\":%.4f,\"pixfmt\":\"%s\",\"audio\":%s,\"hardware\":%s}",
             json_escape_string(s.mode_code).c_str(), s.width, s.height,
             s.mode_fps, pixfmt_name(s.pixfmt),
             s.audio ? "true" : "false", s.hardware ? "true" : "false");
    send_event(buf);
}

void Session::apply_config(const JsonObject &msg)
{
    if (msg.has("device"))    cfg_.device    = msg.str("device");
    if (msg.has("mode"))      cfg_.mode_code = msg.str("mode");
    if (msg.has("preroll"))   cfg_.preroll   = msg.integer("preroll", cfg_.preroll);
    if (msg.has("offset_ms")) cfg_.offset_ms = msg.num("offset_ms", cfg_.offset_ms);
    if (msg.has("lock_w"))    cfg_.fixed_width  = msg.integer("lock_w", 0);
    if (msg.has("lock_h"))    cfg_.fixed_height = msg.integer("lock_h", 0);
    if (msg.has("audio"))     cfg_.enable_audio = msg.boolean("audio", false);
    if (msg.has("audio_channels"))
        cfg_.audio_channels = msg.integer("audio_channels", 2);

    if (msg.has("pixfmt")) {
        int pixfmt = cfg_.pixfmt;
        if (parse_pixfmt_name(msg.str("pixfmt"), &pixfmt))
            cfg_.pixfmt = pixfmt;
        else
            log_error("unknown pixel format '%s' — keeping %s",
                      msg.str("pixfmt").c_str(), pixfmt_name(cfg_.pixfmt));
    }
    if (msg.has("levels"))
        cfg_.full_range = (msg.str("levels") == "full");
    if (msg.has("framing"))
        cfg_.framing = (msg.str("framing") == "1:1") ? Framing::OneToOne
                                                     : Framing::Fit;
    if (msg.has("link")) {
        std::string link = msg.str("link");
        cfg_.link_mode = (link == "quad") ? DLK_LINK_QUAD
                       : (link == "dual") ? DLK_LINK_DUAL
                                          : DLK_LINK_SINGLE;
    }
    if (msg.has("hwdec")) {
        std::string hw = msg.str("hwdec");
        cfg_.hw = (hw == "software") ? HwMode::Software
                : (hw == "vt")       ? HwMode::VideoToolbox
                                     : HwMode::Auto;
    }
}

void Session::do_load(const JsonObject &msg)
{
    std::string path = msg.str("path");
    if (path.empty()) {
        send_error("load: no path given");
        return;
    }

    player_.stop();
    path_ = path;

    std::string err;
    if (!player_.start(path, cfg_, &err)) {
        send_error("could not start output: " + err);
        return;
    }

    // The clock has to be set before the feed loop's first selection, or it
    // will pick frames against a position of zero.
    player_.clock().reset(msg.num("position", 0.0), msg.num("speed", 1.0),
                          msg.boolean("paused", false));
    if (msg.num("position", 0.0) > 0.0)
        player_.request_seek(msg.num("position", 0.0));

    send_started();
}

// Reopening the card is the only way to change mode, pixel format or link
// configuration, so a configure while playing restarts at the current position.
void Session::restart_for_config()
{
    if (!player_.running() || path_.empty())
        return;

    PlayerStatus s = player_.status();
    double position = player_.clock().position();
    bool   paused   = player_.clock().paused();
    double speed    = player_.clock().speed();
    (void)s;

    player_.stop();
    std::string err;
    if (!player_.start(path_, cfg_, &err)) {
        send_error("could not reopen output: " + err);
        return;
    }
    player_.clock().reset(position, speed, paused);
    if (position > 0.0)
        player_.request_seek(position);
    send_started();
}

void Session::handle(const std::string &text)
{
    JsonObject msg;
    if (!JsonObject::parse(text, &msg)) {
        log_error("ignoring malformed control message");
        return;
    }

    std::lock_guard<std::mutex> guard(mutex_);
    std::string cmd = msg.str("cmd");

    if (cmd == "configure") {
        apply_config(msg);
        if (player_.running())
            restart_for_config();
    } else if (cmd == "load") {
        if (msg.has("device") || msg.has("pixfmt") || msg.has("mode"))
            apply_config(msg);   // load may carry the settings with it
        do_load(msg);
    } else if (cmd == "position") {
        player_.clock().update(msg.num("position", 0.0), msg.num("speed", 1.0),
                              msg.boolean("paused", false));
    } else if (cmd == "seek") {
        double position = msg.num("position", 0.0);
        player_.clock().reset(position, msg.num("speed", 1.0),
                              msg.boolean("paused", false));
        player_.request_seek(position);
    } else if (cmd == "pause") {
        player_.clock().set_paused(msg.boolean("paused", true));
    } else if (cmd == "blackout") {
        // IINA's own playback keeps running throughout — this only affects
        // what the DeckLink shows, so the resume target is wherever the
        // clock's own extrapolation says the playhead has gotten to right
        // now, not any position carried in the message.
        player_.set_blackout(msg.boolean("on", false), player_.clock().position());
    } else if (cmd == "speed") {
        player_.clock().reset(msg.num("position", player_.clock().position()),
                             msg.num("speed", 1.0),
                             msg.boolean("paused", player_.clock().paused()));
    } else if (cmd == "grab_still") {
        std::string path = msg.str("path");
        std::string err;
        if (path.empty()) {
            send_event("{\"event\":\"still\",\"ok\":false,"
                       "\"message\":\"no path given\"}");
        } else if (player_.grab_still(path, &err)) {
            send_event("{\"event\":\"still\",\"ok\":true,\"path\":\"" +
                      json_escape_string(path) + "\"}");
        } else {
            send_event("{\"event\":\"still\",\"ok\":false,\"message\":\"" +
                      json_escape_string(err) + "\"}");
        }
    } else if (cmd == "list-devices") {
        // The plugin asks over the socket rather than running --list-devices,
        // because IINA's utils.exec serialises and this process is itself an
        // outstanding exec: a second one would never resolve while output runs.
        send_enumeration("devices", devices_json());
    } else if (cmd == "list-modes") {
        send_enumeration("modes", modes_json(msg.str("device")));
    } else if (cmd == "stop") {
        player_.stop();
        send_event("{\"event\":\"stopped\"}");
    } else if (cmd == "quit") {
        player_.stop();
        quit_ = true;
    } else if (cmd == "ping") {
        send_event("{\"event\":\"pong\"}");
    } else if (!cmd.empty()) {
        log_debug("ignoring unknown command '%s'", cmd.c_str());
    }
}

void Session::tick_status()
{
    auto now = std::chrono::steady_clock::now();
    if (now - last_status_ < kStatusInterval)
        return;
    last_status_ = now;

    if (!player_.running())
        return;

    PlayerStatus s = player_.status();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"status\",\"position\":%.3f,\"error_ms\":%.1f,"
             "\"dropped\":%lld,\"repeated\":%lld,\"reseeks\":%lld,"
             "\"audio\":%s,\"audio_frames\":%lld,\"audio_silence\":%lld,"
             "\"audio_resyncs\":%lld}",
             s.position, s.error_ms, (long long)s.dropped,
             (long long)s.repeated, (long long)s.reseeks,
             s.audio ? "true" : "false", (long long)s.audio_frames,
             (long long)s.audio_silence, (long long)s.audio_resyncs);
    send_event(buf);
}

}  // namespace

int run_serve(const std::string &url, const OutputConfig &defaults)
{
    WsClient ws;
    std::string err;
    if (!ws.connect(url, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }
    log_info("connected to %s", url.c_str());

    Session session(&ws, defaults);
    ws.start([&session](const std::string &text) { session.handle(text); });

    ws.send("{\"event\":\"ready\",\"version\":\"0.1.0\"}");

    while (!session.should_quit() && ws.connected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        session.tick_status();

        if (ws.seconds_since_last_message() > kWatchdogSeconds) {
            log_info("no control messages for %.0fs — the plugin has gone away, "
                     "releasing the device", kWatchdogSeconds);
            break;
        }
    }

    ws.stop();
    log_info("helper exiting");
    return 0;
}
