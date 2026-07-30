/*
 * The feed loop: decode ahead on one thread, and on another pick the frame
 * that belongs on the wire right now and hand it to the card.
 *
 * IINA and the DeckLink run on independent clocks — IINA is paced by the Mac's
 * audio device, the card by its own crystal — so the helper cannot simply play
 * at nominal speed and hope.  Instead MasterClock tracks where IINA says it is
 * and extrapolates between reports, and the feed loop resolves, for every
 * output frame slot, which source frame is nearest that position.
 *
 * That single mechanism covers three jobs:
 *
 *   - frame rate conversion: holding each source frame for mode_fps/src_fps
 *     output frames *is* pulldown, so 23.976p on a 60p output falls out with
 *     no special case;
 *   - drift: a ±1 adjustment to that hold count, outside a deadband so it
 *     doesn't hunt;
 *   - seeks: anything past a large threshold re-seeks the demuxer and
 *     re-anchors the hardware schedule instead of trying to servo.
 */

#ifndef IINA_DECKLINK_PLAYER_H
#define IINA_DECKLINK_PLAYER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "converter.h"
#include "decklink_shim.h"
#include "decoder.h"
#include "output.h"

struct OutputConfig {
    std::string device;      // "" = first found
    std::string mode_code;   // "" = auto-select
    int     pixfmt         = DLK_PIXFMT_V210;
    bool    full_range     = false;
    int     link_mode      = DLK_LINK_SINGLE;
    int     preroll        = 3;
    int     fixed_width    = 0;   // >0 locks the resolution, fps still auto
    int     fixed_height   = 0;
    Framing framing        = Framing::Fit;
    bool    enable_audio   = false;
    int     audio_channels = 2;
    double  offset_ms      = 0.0; // manual trim for the report→wire latency
    HwMode  hw             = HwMode::Auto;
    bool    null_output    = false;  // simulate the card, for testing
    double  null_fps       = 0.0;    // pin the simulated mode rate
    // Servo tuning, exposed so the constants can be compared against measured
    // behaviour rather than guessed at.  Zero means "use the default".
    double  servo_gain     = 0.0;
    double  servo_deadband = 0.0;
};

// Where the master (IINA) says playback is, extrapolated between reports so a
// late or dropped report costs nothing.
class MasterClock {
public:
    // update() filters: it treats the report as a noisy observation of where
    // IINA is. reset() snaps, for the discontinuities where the report is
    // simply the truth. See the commentary in player.cpp.
    void update(double position, double speed, bool paused);
    void reset(double position, double speed, bool paused);
    void set_paused(bool paused);
    double position() const;
    // Elapsed real (output/wall-clock) time since the clock was first
    // anchored — source time with playback speed's effect removed, which is
    // what audio needs: the card plays at a fixed real-time rate regardless
    // of speed, so audio has to be scheduled on this axis, not position()'s.
    // Maintained the same piecewise way position() is — folding each interval
    // in at its own speed when the clock is re-anchored, never by dividing
    // the whole-history position() by whatever speed happens to be current —
    // which is what makes it correct across any number of speed changes,
    // where a fresh division each time would not be: source time is a
    // whole-history quantity, and speed is only ever known instantaneously.
    double output_position() const;
    double speed() const;
    bool paused() const;

private:
    mutable std::mutex mutex_;
    double position_ = 0.0;
    double output_position_ = 0.0;
    double speed_    = 1.0;
    bool   paused_   = false;
    bool   anchored_ = false;  // false until the first report, which must snap
    std::chrono::steady_clock::time_point stamp_ = std::chrono::steady_clock::now();
};

struct PlayerStatus {
    bool   running     = false;
    int    width       = 0;
    int    height      = 0;
    double mode_fps    = 0.0;
    std::string mode_code;
    int    pixfmt      = 0;     // as negotiated, which may differ from requested
    bool   audio       = false;
    bool   hardware    = false;
    double position    = 0.0;   // where the output currently is, in source time
    double error_ms    = 0.0;   // most recent sample of output minus IINA
    // Error statistics, gathered after a short warm-up so the startup
    // transient doesn't contaminate them.  A single instantaneous sample says
    // very little about a servo that oscillates by design; the mean is the
    // standing bias worth trimming out, the RMS is the jitter.
    double error_mean_ms = 0.0;
    double error_rms_ms  = 0.0;
    double error_min_ms  = 0.0;
    double error_max_ms  = 0.0;
    int64_t error_samples = 0;
    // Both counters mean exactly one thing, because a fuzzier definition
    // misleads: an earlier `repeated` counted every servo nudge, and reported
    // hundreds of repeated frames on a 1:1 feed that was in fact showing each
    // frame exactly once — a nudge shifts the fractional accumulator and
    // leaves the hold count at one.
    int64_t dropped    = 0;   // source frames discarded without reaching the wire
    int64_t repeated   = 0;   // output slots that re-showed the last frame
                              // because nothing newer was decoded in time.
                              // Pulldown holds are by design and are not
                              // counted; --play --null reports the cadence.
    int64_t reseeks    = 0;
    // Audio, when the card was opened with it.  `audio_silence` counts sample
    // frames the feed had to invent because nothing was decoded for that
    // instant — the audio equivalent of `repeated`, and the number that says
    // whether the embedded track is actually intact.
    int64_t audio_frames  = 0;
    int64_t audio_silence = 0;
    int64_t audio_resyncs = 0;
};

class Player {
public:
    Player() = default;
    ~Player();

    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    bool start(const std::string &path, const OutputConfig &cfg, std::string *err);
    void stop();
    bool running() const { return running_; }

    MasterClock &clock() { return clock_; }

    // For tests: the simulated card, when cfg.null_output was set.
    VideoOutput *output() { return output_.get(); }

    // Discards decoded frames and re-anchors the card's schedule.  Called on a
    // real seek, and by the feed loop when drift is too large to servo away.
    void request_seek(double position);

    // Blanks the output to black and holds it there — for the reference
    // monitor while IINA isn't the frontmost app, so it isn't left showing a
    // frozen frame (or worse, contributing to burn-in) for as long as the
    // user is away. Decode keeps running underneath (nothing here touches
    // the decode thread), so the queue simply fills to its cap and the
    // demuxer idles there via the same backpressure an ordinary full queue
    // already produces — cheap to enter and leave, and nothing to explicitly
    // suspend or resume on the decode side.
    //
    // Turning it off re-seeks to `resume_at`: whatever was queued belongs to
    // wherever IINA's position was when blackout started, and IINA's own
    // playback keeps advancing throughout — unlike an ordinary pause, this
    // is a DeckLink-only concern the moment it's lifted.
    void set_blackout(bool on, double resume_at);
    bool blackout() const { return blackout_.load(); }

    // Writes the frame currently on the wire — native resolution, 16-bit RGB
    // TIFF — to `path`. Safe to call from the control thread at any time; it
    // takes its own clone of whatever the feed loop last displayed rather than
    // touching the feed loop's state, so a still grab never stalls playback
    // and playback never blocks a still grab. False (with *err set) if nothing
    // has been decoded yet.
    bool grab_still(const std::string &path, std::string *err);

    PlayerStatus status() const;

private:
    void decode_loop();
    void feed_loop();
    void clear_queue();
    bool send_canvas(int repeat, double source_time);

    // Hands the card the audio belonging to `output_frames` output slots,
    // whose picture is at `source_time`.  Must be called exactly once per
    // scheduled video frame: the card timestamps audio on the same axis as
    // video, so a missed or doubled call walks the two apart permanently.
    void feed_audio(int output_frames, double source_time);
    // Copies `nframes` sample frames starting at source time `from` into dst,
    // filling anything not decoded with silence.  Returns the number of frames
    // of silence it had to invent.
    int  pull_audio(double from, int nframes, int32_t *dst);
    // Clones `frame` for grab_still() to find later. Called by the feed loop
    // right after a native frame is converted onto the canvas.
    void update_still_frame(const AVFrame *frame);

    struct QueuedFrame {
        AVFrame *frame = nullptr;
        double   time  = 0.0;
    };

    Decoder     decoder_;
    Converter   converter_;
    std::unique_ptr<VideoOutput> output_;
    OutputConfig cfg_;

    MasterClock clock_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> blackout_{false};

    std::thread decode_thread_;
    std::thread feed_thread_;

    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueuedFrame> queue_;

    // The native decoded frame most recently put on the wire, for on-demand
    // still capture. Its own lock, separate from queue_mutex_: a still grab
    // must never contend with the feed loop's real-time frame selection, and
    // the feed loop must never wait on TIFF encoding.
    mutable std::mutex still_mutex_;
    AVFrame *still_frame_ = nullptr;
    // Decoded audio waiting to be scheduled, oldest first.  Filled by the
    // decode thread under the same lock as the video queue, since both are
    // produced by the one demux pass and both are discarded by the same seek.
    std::deque<AudioChunk>  audio_queue_;
    // Bumped on every seek so frames decoded before it can be discarded
    // without ambiguity.
    uint64_t                generation_ = 0;
    bool                    seek_pending_ = false;
    double                  seek_target_  = 0.0;

    // Output mode, fixed once the card is open.
    int    out_width_  = 0;
    int    out_height_ = 0;
    double mode_fps_   = 0.0;
    std::string mode_code_;
    int    actual_pixfmt_ = 0;
    bool   audio_on_   = false;

    // Feed-loop state.  The two reported values are atomic because status()
    // is called from the control thread while the feed loop is writing them.
    double  phase_            = 0.0;   // fractional pulldown accumulator
    std::atomic<double> last_frame_time_{-1.0};  // source time now on the wire
    std::atomic<double> last_error_{0.0};

    // Error statistics.  Written only by the feed thread; the mutex guards
    // them against status() reading a torn set.
    mutable std::mutex stats_mutex_;
    double  err_sum_    = 0.0;
    double  err_sum_sq_ = 0.0;
    double  err_min_    = 0.0;
    double  err_max_    = 0.0;
    int64_t err_count_  = 0;
    std::atomic<int64_t> dropped_{0};
    std::atomic<int64_t> repeated_{0};
    std::atomic<int64_t> reseeks_{0};

    // Audio feed state, touched only by the feed thread.
    //
    // The cursor runs on its own: audio advances by exactly one output frame's
    // worth of samples per scheduled video frame, and is not dragged around by
    // the video servo's sub-frame trims — a nudge that is invisible in the
    // picture would be an audible click. It is only snapped back to the
    // picture on a seek, or if the two somehow drift a quarter-second apart.
    int     audio_channels_   = 0;    // 0 when the card has no audio enabled
    double  audio_cursor_     = -1.0; // source time of the next sample to send
    double  audio_frac_       = 0.0;  // sub-sample remainder of samples-per-frame
    std::vector<int32_t> audio_scratch_;
    // Set by request_seek on the control thread and consumed by the feed
    // thread, which owns the cursor.
    std::atomic<bool> audio_reanchor_{false};
    std::atomic<int64_t> audio_frames_{0};
    std::atomic<int64_t> audio_silence_{0};
    std::atomic<int64_t> audio_resyncs_{0};
    // When the last re-anchor happened, so the feed loop doesn't stack more of
    // them while the decoder is still working through the first.
    std::chrono::steady_clock::time_point last_reseek_{};
};

#endif  // IINA_DECKLINK_PLAYER_H
