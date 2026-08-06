/*
 * iina-decklink-helper — companion process for the IINA DeckLink plugin.
 *
 * An IINA plugin is JavaScript in a sandbox with no access to decoded frames,
 * so it cannot feed a DeckLink itself.  Instead it launches this helper, which
 * owns the card, decodes the same media independently, and is slaved to IINA's
 * playhead over a WebSocket.
 *
 * Commands:
 *
 *   --list-devices              connected cards, as JSON
 *   --list-modes [--device N]   supported display modes, as JSON
 *   --probe FILE                source geometry and colour metadata, as JSON
 *   --dump FILE --out P.ppm     run one frame through the real conversion path
 *                               and write the finished canvas as a PPM, so the
 *                               geometry, padding and levels can be checked
 *                               with no card attached
 *   --still FILE --out S.tiff   write one native decoded frame as a 16-bit
 *                               RGB TIFF (what "Grab Still" sends the helper)
 *
 * Logs go to stderr, which the plugin captures via utils.exec's stderrHook and
 * forwards to IINA's log window.  stdout carries JSON only.  --log additionally
 * appends everything, debug lines included, to a file: IINA's log window is
 * in-memory and per-session, which makes it useless for the faults worth
 * diagnosing — the intermittent ones nobody is watching for when they happen.
 */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "converter.h"
#include "decklink_shim.h"
#include "decoder.h"
#include "enumerate.h"
#include "log.h"
#include "player.h"
#include "serve.h"
#include "stills.h"

// ---------------------------------------------------------------------------
// Minimal JSON output.  The helper only ever emits a handful of shapes, so a
// full serializer would be more machinery than the job needs.
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\"";  break;
        case '\\': out += "\\\\";  break;
        case '\n': out += "\\n";   break;
        case '\r': out += "\\r";   break;
        case '\t': out += "\\t";   break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

static std::string json_str(const std::string &s)
{
    return "\"" + json_escape(s) + "\"";
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------
static int cmd_list_devices(void)
{
    printf("%s\n", devices_json().c_str());
    return 0;
}

static int cmd_list_modes(const char *device)
{
    std::string json = modes_json(device ? device : "");
    printf("%s\n", json.c_str());
    return json == "{\"modes\":[]}" ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Source inspection
// ---------------------------------------------------------------------------
static const char *name_or_unknown(const char *s)
{
    return s ? s : "unknown";
}

static void print_source_json(const SourceInfo &info)
{
    printf("{\"width\":%d,\"height\":%d,\"fps\":%.6f,\"duration\":%.3f,"
           "\"codec\":%s,\"hardware\":%s,"
           "\"colorspace\":%s,\"range\":%s,\"primaries\":%s,\"transfer\":%s,"
           "\"audio\":%s",
           info.width, info.height, info.fps, info.duration,
           json_str(info.codec_name).c_str(),
           info.hardware ? "true" : "false",
           json_str(name_or_unknown(av_color_space_name(info.colorspace))).c_str(),
           json_str(name_or_unknown(av_color_range_name(info.color_range))).c_str(),
           json_str(name_or_unknown(av_color_primaries_name(info.primaries))).c_str(),
           json_str(name_or_unknown(av_color_transfer_name(info.transfer))).c_str(),
           info.has_audio ? "true" : "false");
    if (info.has_audio)
        printf(",\"audio_channels\":%d,\"audio_rate\":%d",
               info.audio_channels, info.audio_rate);
    printf("}\n");
}

static int cmd_probe(const char *path, HwMode hw)
{
    Decoder dec;
    std::string err;
    if (!dec.open(path, hw, /*audio_channels=*/0, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }
    print_source_json(dec.info());
    return 0;
}

// ---------------------------------------------------------------------------
// Single-frame conversion dump
//
// Runs a real decode through the real Converter and writes the finished
// DeckLink canvas out as an 8-bit PPM, converted back to RGB purely for
// viewing.  This is how the geometry, padding colour and level handling get
// checked without a card attached.
// ---------------------------------------------------------------------------
static bool write_ppm(const char *path, const AVFrame *canvas)
{
    SwsContext *sws = sws_getContext(canvas->width, canvas->height,
                                     (AVPixelFormat)canvas->format,
                                     canvas->width, canvas->height,
                                     AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        log_error("could not create preview scaler");
        return false;
    }

    uint8_t *rgb_data[4] = {};
    int rgb_stride[4] = {};
    if (av_image_alloc(rgb_data, rgb_stride, canvas->width, canvas->height,
                       AV_PIX_FMT_RGB24, 1) < 0) {
        sws_freeContext(sws);
        log_error("out of memory");
        return false;
    }

    sws_scale(sws, canvas->data, canvas->linesize, 0, canvas->height,
              rgb_data, rgb_stride);
    sws_freeContext(sws);

    FILE *f = fopen(path, "wb");
    if (!f) {
        av_freep(&rgb_data[0]);
        log_error("could not open %s for writing", path);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", canvas->width, canvas->height);
    for (int y = 0; y < canvas->height; y++)
        fwrite(rgb_data[0] + (ptrdiff_t)y * rgb_stride[0], 1,
               (size_t)canvas->width * 3, f);
    fclose(f);
    av_freep(&rgb_data[0]);
    return true;
}

// Writes the canvas in its own layout, tightly packed, for byte-level checks
// (that the padding really is broadcast black rather than zeros, for one).
static bool write_raw(const char *path, const AVFrame *canvas)
{
    int size = av_image_get_buffer_size((AVPixelFormat)canvas->format,
                                        canvas->width, canvas->height, 1);
    if (size <= 0) {
        log_error("could not size the canvas buffer");
        return false;
    }
    std::vector<uint8_t> buf((size_t)size);
    if (av_image_copy_to_buffer(buf.data(), size, canvas->data, canvas->linesize,
                                (AVPixelFormat)canvas->format,
                                canvas->width, canvas->height, 1) < 0) {
        log_error("could not copy the canvas");
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        log_error("could not open %s for writing", path);
        return false;
    }
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
}

struct DumpOptions {
    const char *path    = nullptr;
    const char *out     = "canvas.ppm";
    const char *raw     = nullptr;
    double      at      = 0.0;
    int         out_w   = 1920;
    int         out_h   = 1080;
    int         pixfmt  = DLK_PIXFMT_V210;
    Framing     framing = Framing::Fit;
    bool        full_range = false;
    HwMode      hw      = HwMode::Auto;
};

static int cmd_dump(const DumpOptions &opt)
{
    Decoder dec;
    std::string err;
    if (!dec.open(opt.path, opt.hw, /*audio_channels=*/0, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    if (opt.at > 0.0 && !dec.seek(opt.at))
        return 1;

    // Take the first frame at or after the requested time.
    AVFrame *frame = nullptr;
    for (;;) {
        int rc = dec.next_frame(&frame);
        if (rc <= 0) {
            log_error("no frame decoded%s", rc == 0 ? " (end of stream)" : "");
            return 1;
        }
        if (dec.frame_time(frame) + 1e-6 >= opt.at)
            break;
        av_frame_free(&frame);
    }

    Converter conv;
    if (!conv.configure(dec.info(), opt.out_w, opt.out_h, opt.pixfmt,
                        opt.framing, opt.full_range, &err)) {
        log_error("%s", err.c_str());
        av_frame_free(&frame);
        return 1;
    }
    if (!conv.convert(frame, &err)) {
        log_error("%s", err.c_str());
        av_frame_free(&frame);
        return 1;
    }
    double t = dec.frame_time(frame);
    av_frame_free(&frame);

    if (!write_ppm(opt.out, conv.canvas()))
        return 1;
    if (opt.raw && !write_raw(opt.raw, conv.canvas()))
        return 1;

    printf("{\"wrote\":%s,\"time\":%.3f,\"width\":%d,\"height\":%d,\"format\":%s}\n",
           json_str(opt.out).c_str(), t, opt.out_w, opt.out_h,
           json_str(av_get_pix_fmt_name((AVPixelFormat)conv.canvas()->format)).c_str());
    return 0;
}

// Exercises the still-capture path with no card and no plugin attached: decode
// to the requested timestamp and write the native frame straight to a TIFF,
// bypassing the DeckLink canvas entirely. `--out` defaults to canvas.ppm for
// --dump, which is the wrong extension here, so a still-specific default.
static int cmd_still(const char *path, double at, const char *out, HwMode hw)
{
    Decoder dec;
    std::string err;
    if (!dec.open(path, hw, /*audio_channels=*/0, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    if (at > 0.0 && !dec.seek(at))
        return 1;

    AVFrame *frame = nullptr;
    for (;;) {
        int rc = dec.next_frame(&frame);
        if (rc <= 0) {
            log_error("no frame decoded%s", rc == 0 ? " (end of stream)" : "");
            return 1;
        }
        if (dec.frame_time(frame) + 1e-6 >= at)
            break;
        av_frame_free(&frame);
    }

    std::string outpath = (out && strcmp(out, "canvas.ppm")) ? out : "still.tiff";
    double t = dec.frame_time(frame);
    if (!save_still_tiff(frame, dec.info(), outpath, &err)) {
        log_error("%s", err.c_str());
        av_frame_free(&frame);
        return 1;
    }
    av_frame_free(&frame);

    printf("{\"wrote\":%s,\"time\":%.3f}\n", json_str(outpath).c_str(), t);
    return 0;
}

// ---------------------------------------------------------------------------
// Standalone playback
//
// Runs the real feed loop with a free-running clock instead of IINA's, so the
// output path can be exercised end to end against a card without involving the
// plugin at all.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_interrupted = 0;

static void on_interrupt(int)
{
    g_interrupted = 1;
}

static int cmd_play(const char *path, const OutputConfig &cfg, double seconds,
                    double play_speed)
{
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);

    Player player;
    std::string err;
    if (!player.start(path, cfg, &err)) {
        log_error("%s", err.c_str());
        return 1;
    }

    // A free-running master: set it going once and let it extrapolate.  This
    // is exactly what a paused-forever or perfectly-steady IINA would look
    // like to the feed loop. --speed exercises the pitch-preserving tempo
    // path standalone, without a WebSocket harness driving live changes.
    player.clock().reset(0.0, play_speed, false);
    const double clock_epoch = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    PlayerStatus s = player.status();
    log_info("playing: %s %dx%d @ %.3f fps, %s decode — Ctrl-C to stop",
             s.mode_code.c_str(), s.width, s.height, s.mode_fps,
             s.hardware ? "hardware" : "software");

    auto start = std::chrono::steady_clock::now();
    while (!g_interrupted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds > 0.0 && elapsed >= seconds)
            break;
        s = player.status();
        if (s.audio) {
            // The card's audio buffer depth is the thing to watch: audio is
            // supplied per scheduled video frame, so a slow drain here is the
            // symptom of under-supply, and it ends in an audible underrun long
            // before any counter notices.
            log_debug("t=%.2f pos=%.3f err=%+.1fms dropped=%lld repeated=%lld "
                      "reseeks=%lld abuf=%d asilence=%lld",
                      elapsed, s.position, s.error_ms, (long long)s.dropped,
                      (long long)s.repeated, (long long)s.reseeks,
                      player.output() ? player.output()->buffered_audio_frames() : 0,
                      (long long)s.audio_silence);
        } else {
            log_debug("t=%.2f pos=%.3f err=%+.1fms dropped=%lld repeated=%lld reseeks=%lld",
                      elapsed, s.position, s.error_ms, (long long)s.dropped,
                      (long long)s.repeated, (long long)s.reseeks);
        }
    }

    s = player.status();

    // With the simulated card, report the hold count of every scheduled frame
    // so the pulldown cadence can be checked: 23.976p on a 60p output should
    // settle into 3,2,3,2 and nothing else.
    std::string cadence_json;
    if (NullOutput *nul = dynamic_cast<NullOutput *>(player.output())) {
        std::vector<int> c = nul->cadence();
        int counts[8] = {};
        for (int hold : c)
            if (hold >= 0 && hold < 8)
                counts[hold]++;
        cadence_json = ",\"cadence\":{";
        bool first = true;
        for (int i = 0; i < 8; i++) {
            if (!counts[i])
                continue;
            char buf[48];
            snprintf(buf, sizeof(buf), "%s\"%d\":%d", first ? "" : ",", i, counts[i]);
            cadence_json += buf;
            first = false;
        }
        cadence_json += "}";

        // The first 24 holds, which is where a wrong cadence shows up.
        cadence_json += ",\"cadence_head\":[";
        for (size_t i = 0; i < c.size() && i < 24; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%d", i ? "," : "", c[i]);
            cadence_json += buf;
        }
        cadence_json += "]";

        // Ground truth, independent of the feed loop's own arithmetic: walk
        // every output frame slot the card displayed, work out which source
        // time was on the wire during it, and compare that against where the
        // master clock actually was at that instant.  The feed loop's internal
        // error figure is what it *believed*; this is what a camera pointed at
        // the monitor would have seen.
        std::vector<NullOutput::ScheduleEntry> log = nul->schedule_log();
        double fps = nul->info().fps;
        double null_start = nul->started_at();
        double sum = 0.0, sum_sq = 0.0, lo = 0.0, hi = 0.0;
        int64_t n = 0;
        for (const auto &e : log) {
            for (int j = 0; j < e.hold; j++) {
                double display_wall = null_start + (double)(e.output_index + j) / fps;
                double expected     = display_wall - clock_epoch;
                if (expected < 0.75)   // skip the startup transient
                    continue;
                double err = e.source_time - expected;
                sum += err;
                sum_sq += err * err;
                if (n == 0 || err < lo) lo = err;
                if (n == 0 || err > hi) hi = err;
                n++;
            }
        }
        if (n > 0) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     ",\"true_mean_ms\":%.2f,\"true_rms_ms\":%.2f,"
                     "\"true_min_ms\":%.1f,\"true_max_ms\":%.1f,\"true_samples\":%lld",
                     sum / (double)n * 1000.0,
                     std::sqrt(sum_sq / (double)n) * 1000.0,
                     lo * 1000.0, hi * 1000.0, (long long)n);
            cadence_json += buf;
        }
    }

    // The audio invariant, checked here because it is the one thing about the
    // audio path that can be verified with no hardware and no listening.  The
    // card derives its audio timestamps from the video frame counter, so the
    // sample frames sent must match the output frames *displayed*, to within
    // the rounding carried between them.  `audio_skew` is that difference in
    // samples; anything but a handful means audio walks off the picture.
    //
    // The baseline is the sum of the holds in the schedule log, not
    // scheduled_frames(): the latter includes the jump resync() makes over the
    // startup gap, and those slots carry no picture and no audio.
    std::string audio_json;
    if (s.audio) {
        char buf[320];
        snprintf(buf, sizeof(buf),
                 ",\"audio_frames\":%lld,\"audio_silence\":%lld,"
                 "\"audio_resyncs\":%lld",
                 (long long)s.audio_frames, (long long)s.audio_silence,
                 (long long)s.audio_resyncs);
        audio_json = buf;

        // The skew check needs the schedule log, so it is only meaningful
        // against the simulated card; the real one is not asked to
        // reconstruct what it displayed.
        if (NullOutput *nul = dynamic_cast<NullOutput *>(player.output())) {
            long long displayed = 0;
            for (const auto &e : nul->schedule_log())
                displayed += e.hold;
            long long expected =
                llround((double)displayed * DLK_AUDIO_RATE / s.mode_fps);
            snprintf(buf, sizeof(buf),
                     ",\"audio_expected\":%lld,\"audio_skew\":%lld",
                     expected, (long long)s.audio_frames - expected);
            audio_json += buf;
        }
    }

    player.stop();
    printf("{\"played\":%.2f,\"mode_fps\":%.3f,\"dropped\":%lld,\"repeated\":%lld,"
           "\"reseeks\":%lld,\"err_mean_ms\":%.2f,\"err_rms_ms\":%.2f,"
           "\"err_min_ms\":%.1f,\"err_max_ms\":%.1f,\"samples\":%lld%s%s}\n",
           seconds, s.mode_fps, (long long)s.dropped, (long long)s.repeated,
           (long long)s.reseeks, s.error_mean_ms, s.error_rms_ms,
           s.error_min_ms, s.error_max_ms, (long long)s.error_samples,
           cadence_json.c_str(), audio_json.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------
static bool parse_pixfmt(const char *s, int *out)
{
    if (!strcmp(s, "uyvy"))  { *out = DLK_PIXFMT_UYVY;  return true; }
    if (!strcmp(s, "v210"))  { *out = DLK_PIXFMT_V210;  return true; }
    if (!strcmp(s, "argb"))  { *out = DLK_PIXFMT_ARGB;  return true; }
    if (!strcmp(s, "rgb10")) { *out = DLK_PIXFMT_RGB10; return true; }
    return false;
}

static bool parse_hwmode(const char *s, HwMode *out)
{
    if (!strcmp(s, "auto"))     { *out = HwMode::Auto;         return true; }
    if (!strcmp(s, "vt"))       { *out = HwMode::VideoToolbox; return true; }
    if (!strcmp(s, "software")) { *out = HwMode::Software;     return true; }
    return false;
}

static bool parse_link(const char *s, int *out)
{
    if (!strcmp(s, "single")) { *out = DLK_LINK_SINGLE; return true; }
    if (!strcmp(s, "dual"))   { *out = DLK_LINK_DUAL;   return true; }
    if (!strcmp(s, "quad"))   { *out = DLK_LINK_QUAD;   return true; }
    return false;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: iina-decklink-helper <command> [options]\n"
        "\n"
        "commands:\n"
        "  --list-devices               connected DeckLink devices, as JSON\n"
        "  --list-modes                 supported display modes, as JSON\n"
        "  --probe FILE                 source geometry and colour metadata\n"
        "  --dump FILE                  convert one frame and write it as a PPM\n"
        "  --still FILE                 write one native frame as a 16-bit TIFF\n"
        "  --play FILE                  play to the card with a free-running clock\n"
        "  --connect ws://HOST:PORT/    attach to the plugin and follow its clock\n"
        "  --version\n"
        "\n"
        "options:\n"
        "  --device NAME                DeckLink device (default: first found)\n"
        "  --mode CODE                  explicit 4-char display mode code\n"
        "  --lock WxH                   lock the output resolution, auto fps\n"
        "  --pixfmt uyvy|v210|argb|rgb10   default v210\n"
        "  --framing fit|1:1            default fit\n"
        "  --levels video|full          default video\n"
        "  --link single|dual|quad      default single\n"
        "  --audio                      embed the source's audio in the SDI feed\n"
        "  --audio-channels 2|8|16      channel count, default 2 (implies --audio)\n"
        "  --preroll N                  frames to buffer, default 3\n"
        "  --offset MS                  output timing trim, default 0\n"
        "  --hwdec auto|vt|software     default auto\n"
        "  --duration SECONDS           stop --play after this long\n"
        "  --speed X                    --play at a constant speed, default 1.0\n"
        "                                (pitch-preserving away from 1.0)\n"
        "  --null                       simulate the card instead of opening one\n"
        "  --null-fps N                 pin the simulated mode rate (implies --null)\n"
        "  --null-stall SECONDS         wedge the simulated card this far in, to\n"
        "                                exercise the watchdog (implies --null)\n"
        "  --out PATH                   output path for --dump (default canvas.ppm) or\n"
        "                                --still (default still.tiff)\n"
        "  --raw PATH                   also write the canvas in its native layout\n"
        "  --at SECONDS                 timestamp for --dump or --still (default 0)\n"
        "  --size WxH                   canvas size for --dump (default 1920x1080)\n"
        "  --verbose                    include debug logging on stderr\n"
        "  --log PATH                   also append every line, debug included,\n"
        "                                to PATH (rotated at 4 MB).  The plugin\n"
        "                                passes its own; $IINA_DECKLINK_LOG is\n"
        "                                used when neither does\n");
}

int main(int argc, char **argv)
{
    log_install_shim_callback();

    const char *device  = nullptr;
    const char *command = nullptr;
    const char *cmd_arg = nullptr;
    const char *connect_url = nullptr;
    const char *log_path = getenv("IINA_DECKLINK_LOG");
    DumpOptions dump;
    OutputConfig cfg;
    double play_duration = 0.0;
    double play_speed    = 1.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        auto need_value = [&](const char **out) -> bool {
            if (i + 1 >= argc) {
                log_error("%s needs a value", a);
                return false;
            }
            *out = argv[++i];
            return true;
        };

        if (!strcmp(a, "--device")) {
            if (!need_value(&device)) return 2;
        } else if (!strcmp(a, "--out")) {
            if (!need_value(&dump.out)) return 2;
        } else if (!strcmp(a, "--raw")) {
            if (!need_value(&dump.raw)) return 2;
        } else if (!strcmp(a, "--at")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            dump.at = atof(v);
        } else if (!strcmp(a, "--size")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (sscanf(v, "%dx%d", &dump.out_w, &dump.out_h) != 2) {
                log_error("--size expects WxH, got '%s'", v);
                return 2;
            }
        } else if (!strcmp(a, "--pixfmt")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (!parse_pixfmt(v, &dump.pixfmt)) {
                log_error("unknown pixel format '%s'", v);
                return 2;
            }
        } else if (!strcmp(a, "--framing")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (!strcmp(v, "fit"))      dump.framing = Framing::Fit;
            else if (!strcmp(v, "1:1")) dump.framing = Framing::OneToOne;
            else { log_error("unknown framing '%s'", v); return 2; }
        } else if (!strcmp(a, "--levels")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (!strcmp(v, "full"))       dump.full_range = true;
            else if (!strcmp(v, "video")) dump.full_range = false;
            else { log_error("unknown levels '%s'", v); return 2; }
        } else if (!strcmp(a, "--hwdec")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (!parse_hwmode(v, &dump.hw)) {
                log_error("unknown hwdec mode '%s'", v);
                return 2;
            }
        } else if (!strcmp(a, "--mode")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.mode_code = v;
        } else if (!strcmp(a, "--lock")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (sscanf(v, "%dx%d", &cfg.fixed_width, &cfg.fixed_height) != 2) {
                log_error("--lock expects WxH, got '%s'", v);
                return 2;
            }
        } else if (!strcmp(a, "--link")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            if (!parse_link(v, &cfg.link_mode)) {
                log_error("unknown link mode '%s'", v);
                return 2;
            }
        } else if (!strcmp(a, "--audio")) {
            cfg.enable_audio = true;
        } else if (!strcmp(a, "--audio-channels")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.enable_audio   = true;
            cfg.audio_channels = atoi(v);
        } else if (!strcmp(a, "--preroll")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.preroll = atoi(v);
        } else if (!strcmp(a, "--offset")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.offset_ms = atof(v);
        } else if (!strcmp(a, "--duration")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            play_duration = atof(v);
        } else if (!strcmp(a, "--speed")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            play_speed = atof(v);
        } else if (!strcmp(a, "--connect")) {
            if (!need_value(&connect_url)) return 2;
            command = a;
        } else if (!strcmp(a, "--null")) {
            cfg.null_output = true;
        } else if (!strcmp(a, "--null-fps")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.null_output = true;
            cfg.null_fps = atof(v);
        } else if (!strcmp(a, "--null-stall")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.null_output = true;
            cfg.null_stall = atof(v);
        } else if (!strcmp(a, "--servo-gain")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.servo_gain = atof(v);
        } else if (!strcmp(a, "--servo-deadband")) {
            const char *v = nullptr;
            if (!need_value(&v)) return 2;
            cfg.servo_deadband = atof(v);
        } else if (!strcmp(a, "--verbose")) {
            log_set_verbose(true);
        } else if (!strcmp(a, "--log")) {
            if (!need_value(&log_path)) return 2;
        } else if (!strncmp(a, "--", 2)) {
            command = a;
            // These commands take the media path as their argument.
            if ((!strcmp(a, "--probe") || !strcmp(a, "--dump") ||
                 !strcmp(a, "--still") || !strcmp(a, "--play")) &&
                i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0) {
                cmd_arg = argv[++i];
            }
        } else {
            log_error("unexpected argument '%s'", a);
            usage();
            return 2;
        }
    }

    if (!command) {
        usage();
        return 2;
    }

    // Opened before anything else runs, so a failure to open the device is in
    // the file too.  Each session starts with the command line that produced
    // it: which device, which mode, which pixel format and whether audio was on
    // are the first questions any dropout report raises.
    if (log_path && log_path[0] && log_set_file(log_path)) {
        std::string command_line;
        for (int i = 0; i < argc; i++) {
            command_line += (i ? " " : "");
            command_line += argv[i];
        }
        log_info("iina-decklink-helper 0.1.0 starting (pid %d): %s",
                 (int)getpid(), command_line.c_str());
    }

    if (!strcmp(command, "--list-devices"))
        return cmd_list_devices();
    if (!strcmp(command, "--list-modes"))
        return cmd_list_modes(device);
    if (!strcmp(command, "--version")) {
        printf("{\"version\":\"0.1.0\"}\n");
        return 0;
    }
    if (!strcmp(command, "--probe")) {
        if (!cmd_arg) {
            log_error("--probe needs a file path");
            return 2;
        }
        return cmd_probe(cmd_arg, dump.hw);
    }
    if (!strcmp(command, "--dump")) {
        if (!cmd_arg) {
            log_error("--dump needs a file path");
            return 2;
        }
        dump.path = cmd_arg;
        return cmd_dump(dump);
    }
    if (!strcmp(command, "--still")) {
        if (!cmd_arg) {
            log_error("--still needs a file path");
            return 2;
        }
        return cmd_still(cmd_arg, dump.at, dump.out, dump.hw);
    }
    if (!strcmp(command, "--connect")) {
        if (device)
            cfg.device = device;
        cfg.pixfmt     = dump.pixfmt;
        cfg.framing    = dump.framing;
        cfg.full_range = dump.full_range;
        cfg.hw         = dump.hw;
        return run_serve(connect_url, cfg);
    }
    if (!strcmp(command, "--play")) {
        if (!cmd_arg) {
            log_error("--play needs a file path");
            return 2;
        }
        // The shared flags feed both --dump and --play.
        if (device)
            cfg.device = device;
        cfg.pixfmt     = dump.pixfmt;
        cfg.framing    = dump.framing;
        cfg.full_range = dump.full_range;
        cfg.hw         = dump.hw;
        return cmd_play(cmd_arg, cfg, play_duration, play_speed);
    }

    log_error("unknown command '%s'", command);
    usage();
    return 2;
}
