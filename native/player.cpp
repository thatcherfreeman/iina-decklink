#include "player.h"

#include <algorithm>
#include <cmath>

#include "log.h"
#include "stills.h"

namespace {

// Frames decoded ahead of the playhead.  Deep enough to absorb decode jitter,
// shallow enough that a seek doesn't throw much work away.
constexpr size_t kQueueDepth = 12;

// kQueueDepth is a count of source frames, which is a fixed span of *source*
// time — but the real-time budget that span represents shrinks as speed
// climbs above 1x. Audio rides the same demux pass as video and has no depth
// cap of its own, so once the video queue fills and decode_loop() blocks,
// audio stops arriving too; at 2x that fixed source-time buffer drains in
// half the real time, and if decode can't refill fast enough in that shorter
// window, the audio queue runs dry — heard as periodic silence with the video
// mostly unaffected, since a starved video frame just repeats the last one
// rather than going silent. Scaling the effective depth by speed keeps the
// *real-time* headroom roughly constant instead. Capped well short of
// unbounded so a pathological speed value can't run decode-ahead away.
size_t effective_queue_depth(double speed)
{
    double scaled = (double)kQueueDepth * std::max(1.0, speed);
    return (size_t)std::min(scaled, (double)kQueueDepth * 8);
}

// Decoded frames to accumulate before the feed loop starts scheduling, so its
// first frame selection has something to choose between.  Capped by a timeout
// so a source that can't produce this many (a very short clip) still plays.
constexpr size_t kPrerollFrames = 4;

// Errors below one output frame are left alone, so ordinary timing noise never
// perturbs the pulldown cadence.
//
// These two were picked by measurement rather than taste — see the --play
// --null harness, which reconstructs what was actually on the wire at each
// instant and compares it against the master clock.  Across 23.976/24/29.97
// sources against 23.976/48/59.94/60 output modes, this pairing holds the
// error inside roughly half a source frame while leaving the 3:2 cadence
// exactly 96/96.  A higher gain chases the mean closer to zero on some
// combinations but widens the spread and starts disturbing the cadence, which
// is the worse trade for a reference feed: a standing offset is trimmable, a
// wandering one isn't.
constexpr double kDeadbandFrames = 1.0;
constexpr double kServoGain = 0.02;

// Drift past this is too large to servo away — re-seek instead.  Also the
// threshold that catches an ordinary user seek that arrived as a position
// jump rather than an explicit seek command.
constexpr double kReseekSeconds = 0.5;

// Minimum spacing between re-anchors.  A seek takes a moment to work through
// the demuxer and refill the queue, during which the error is still large;
// without this the feed loop fires several more reseeks into the gap and each
// one throws away the decode work the last one started.
constexpr auto kReseekCooldown = std::chrono::milliseconds(400);

// How much of the discrepancy between a routine position report and the
// clock's own extrapolation is taken on board.  Reports arrive ten times a
// second, so 0.05 gives a tracking time constant of about two seconds.
constexpr double kTrackingGain = 0.05;

// A discrepancy larger than this isn't drift, it's a discontinuity — the user
// seeked, or reports stopped arriving for a while — so the clock snaps.
constexpr double kClockSnapSeconds = 0.25;

// How far the free-running audio cursor may wander from the picture before it
// is dragged back. Generous on purpose: ordinary servo trims must never reach
// it, and a jump this large is audible as a discontinuity either way.
constexpr double kAudioResyncSeconds = 0.25;

}  // namespace

// ---------------------------------------------------------------------------
// MasterClock
// ---------------------------------------------------------------------------

// A routine position report.
//
// The reported position is quantised to the frame IINA is displaying: it
// advances in whole source-frame steps, so any individual report disagrees with
// the true playhead by up to half a frame either way.  Snapping the clock to
// each one injects that quantisation straight into the feed loop, which then
// dutifully corrects it by holding and dropping frames — a 23.976p source on a
// 23.976p output, which should sit at hold=1 forever, instead accumulates
// repeats for as long as it plays.
//
// So a routine report only nudges the clock and the noise averages out.  The
// cases that genuinely are discontinuous — a seek, a rate change, a gap in
// reporting — are handled by reset(), and caught here as a fallback.
void MasterClock::update(double position, double speed, bool paused)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto now = std::chrono::steady_clock::now();

    if (!anchored_) {
        // No prior state to reconcile against — this defines the shared
        // reference point the decoder's own first post-load/seek anchor
        // independently arrives at too, via the identical position/speed
        // formula (see decoder.cpp's discontinuity handling in
        // drain_audio_frames). Neither side signals the other directly; they
        // agree because they compute the same thing from the same numbers.
        position_        = position;
        output_position_ = position / std::max(0.1, speed);
        speed_    = speed;
        paused_   = paused;
        stamp_    = now;
        anchored_ = true;
        return;
    }

    if (speed != speed_ || paused != paused_) {
        // A genuine change, but not a discontinuity — no seek accompanies
        // this, so the decoder never resets its own accumulator for it
        // either (see the AudioChunk comment in decoder.h). Both
        // accumulators just keep accumulating across it, each still folding
        // in the interval that ran under the state now ending.
        if (!paused_) {
            double elapsed = std::chrono::duration<double>(now - stamp_).count();
            output_position_ += elapsed;
        }
        position_ = position;
        speed_    = speed;
        paused_   = paused;
        stamp_    = now;
        return;
    }

    if (!paused_) {
        double elapsed = std::chrono::duration<double>(now - stamp_).count();
        output_position_ += elapsed;
    }

    const double extrapolated =
        paused_ ? position_
                : position_ + std::chrono::duration<double>(now - stamp_).count() * speed_;
    const double error = position - extrapolated;

    position_ = std::fabs(error) > kClockSnapSeconds
                    ? position
                    : extrapolated + kTrackingGain * error;
    stamp_    = now;
}

// A known discontinuity: the file just loaded, or the user seeked. There is
// nothing to filter here — the reported position is the truth, on both axes:
// output_position_ is reset to match, via the exact formula the decoder
// applies to its own next post-seek frame (see decoder.cpp), rather than
// carrying forward whatever it was — that previous value belonged to material
// on the other side of the seek and has no relationship to this one.
void MasterClock::reset(double position, double speed, bool paused)
{
    std::lock_guard<std::mutex> guard(mutex_);
    position_        = position;
    output_position_ = position / std::max(0.1, speed);
    speed_    = speed;
    paused_   = paused;
    stamp_    = std::chrono::steady_clock::now();
    anchored_ = true;
}

void MasterClock::set_paused(bool paused)
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Fold elapsed time into the stored position before the rate changes,
    // otherwise the pause retroactively applies to time already run.
    auto now = std::chrono::steady_clock::now();
    if (!paused_) {
        double elapsed = std::chrono::duration<double>(now - stamp_).count();
        position_        += elapsed * speed_;
        output_position_ += elapsed;
    }
    paused_ = paused;
    stamp_  = now;
}

double MasterClock::position() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (paused_)
        return position_;
    auto now = std::chrono::steady_clock::now();
    return position_ + std::chrono::duration<double>(now - stamp_).count() * speed_;
}

double MasterClock::output_position() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (paused_)
        return output_position_;
    auto now = std::chrono::steady_clock::now();
    return output_position_ + std::chrono::duration<double>(now - stamp_).count();
}

double MasterClock::speed() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return speed_;
}

bool MasterClock::paused() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return paused_;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Player::~Player()
{
    stop();
}

bool Player::start(const std::string &path, const OutputConfig &cfg, std::string *err)
{
    stop();
    cfg_ = cfg;

    // The card takes 2, 8 or 16 channels and silently clamps anything else, so
    // normalise here — before either the decoder or the card is opened, since
    // the two must resample to and expect the same count or every sample frame
    // is misaligned.
    if (cfg_.audio_channels != 8 && cfg_.audio_channels != 16)
        cfg_.audio_channels = 2;

    const int want_audio_channels = cfg_.enable_audio ? cfg_.audio_channels : 0;
    if (!decoder_.open(path, cfg.hw, want_audio_channels, err))
        return false;

    const SourceInfo &src = decoder_.info();

    OutputOpenParams params;
    params.device         = cfg.device;
    params.mode_code      = cfg.mode_code;
    params.src_width      = src.width;
    params.src_height     = src.height;
    params.src_fps        = src.fps;
    params.pixfmt         = cfg.pixfmt;
    params.preroll        = cfg.preroll;
    params.enable_audio   = cfg.enable_audio;
    params.audio_channels = cfg_.audio_channels;
    // swscale has no legal-range RGB, so RGB output is written full-range and
    // the shim scales it during packing when Video levels are selected.
    params.rgb_legal      = !cfg.full_range;
    params.fixed_width    = cfg.fixed_width;
    params.fixed_height   = cfg.fixed_height;
    params.link_mode      = cfg.link_mode;
    params.force_fps      = cfg.null_fps;

    // The card picks the mode, so it has to be opened before the converter can
    // be told what canvas to fill.
    if (cfg.null_output)
        output_.reset(new NullOutput());
    else
        output_.reset(new DeckLinkOutput());

    if (!output_->open(params, err)) {
        output_.reset();
        decoder_.close();
        return false;
    }

    const OutputInfo &oi = output_->info();
    out_width_     = oi.width;
    out_height_    = oi.height;
    mode_fps_      = oi.fps;
    mode_code_     = oi.mode_code;
    actual_pixfmt_ = oi.pixfmt;
    audio_on_      = oi.audio;

    if (!converter_.configure(src, out_width_, out_height_, actual_pixfmt_,
                              cfg.framing, cfg.full_range, err)) {
        output_.reset();
        decoder_.close();
        return false;
    }

    log_info("player: %s → %s %dx%d @ %.3f fps",
             path.c_str(), mode_code_.c_str(), out_width_, out_height_, mode_fps_);

    phase_           = 0.0;
    last_frame_time_ = -1.0;
    {
        std::lock_guard<std::mutex> guard(stats_mutex_);
        err_sum_ = err_sum_sq_ = err_min_ = err_max_ = 0.0;
        err_count_ = 0;
    }
    dropped_         = 0;
    repeated_        = 0;
    reseeks_         = 0;

    // Audio only runs when the card actually came up with it, the source has
    // it, and the decoder managed to open it. Any of those failing leaves a
    // silent but otherwise correct video feed.
    audio_channels_ = (audio_on_ && decoder_.audio_active())
                          ? cfg_.audio_channels : 0;
    audio_cursor_   = -1.0;
    audio_frac_     = 0.0;
    audio_frames_   = 0;
    audio_silence_  = 0;
    audio_resyncs_  = 0;
    audio_queue_.clear();
    if (audio_channels_ > 0) {
        // One second of headroom, so a long hold never reallocates on the
        // feed thread.
        audio_scratch_.assign((size_t)DLK_AUDIO_RATE * audio_channels_, 0);
        log_info("player: embedded audio, %d channels @ %d Hz",
                 audio_channels_, DLK_AUDIO_RATE);
    } else if (cfg_.enable_audio) {
        log_info("player: embedded audio requested but unavailable — video only");
    }

    stopping_        = false;
    running_         = true;
    blackout_        = false;

    // Re-anchor the clock now.  Opening the decoder and the card takes on the
    // order of 100 ms, and the clock extrapolates from whenever it was last
    // stamped — so without this the feed loop's first reads would show a
    // position that had already run ahead, and the servo would spend its first
    // frames hurrying to catch up to a target that was never real.  The caller
    // overwrites this with IINA's true position immediately afterwards.
    clock_.update(0.0, 1.0, false);

    decode_thread_ = std::thread(&Player::decode_loop, this);
    feed_thread_   = std::thread(&Player::feed_loop, this);
    return true;
}

void Player::stop()
{
    if (!running_ && !decode_thread_.joinable() && !feed_thread_.joinable())
        return;

    stopping_ = true;
    queue_cv_.notify_all();
    if (decode_thread_.joinable())
        decode_thread_.join();
    if (feed_thread_.joinable())
        feed_thread_.join();
    running_ = false;

    clear_queue();
    output_.reset();
    decoder_.close();

    // Cleared unconditionally: stop() runs on every reload as well as final
    // teardown, and a still grabbed in the gap must never hand back a frame
    // from whatever was playing before.
    std::lock_guard<std::mutex> guard(still_mutex_);
    if (still_frame_)
        av_frame_free(&still_frame_);
}

void Player::clear_queue()
{
    std::lock_guard<std::mutex> guard(queue_mutex_);
    for (QueuedFrame &qf : queue_)
        av_frame_free(&qf.frame);
    queue_.clear();
    audio_queue_.clear();
}

void Player::request_seek(double position)
{
    {
        std::lock_guard<std::mutex> guard(queue_mutex_);
        seek_pending_ = true;
        seek_target_  = position;
        generation_++;
        for (QueuedFrame &qf : queue_)
            av_frame_free(&qf.frame);
        queue_.clear();
        // Queued audio belongs to where we were.
        audio_queue_.clear();
    }
    // The cursor itself is feed-thread state, so ask rather than reach in.
    audio_reanchor_ = true;
    queue_cv_.notify_all();
}

void Player::set_blackout(bool on, double resume_at)
{
    bool was_on = blackout_.exchange(on);
    if (was_on && !on) {
        // Coming back: whatever decode built up while blacked out is however
        // far it got from wherever position was *when blackout started*, not
        // from where IINA's playhead — which kept moving the whole time — is
        // now. Cheapest correct fix is the one already proven for every other
        // "the queue no longer matches where we need to be" case: reseek.
        request_seek(resume_at);
    }
}

void Player::update_still_frame(const AVFrame *frame)
{
    AVFrame *clone = av_frame_clone(frame);
    if (!clone)
        return;   // out of memory — the still just stays one frame stale
    std::lock_guard<std::mutex> guard(still_mutex_);
    if (still_frame_)
        av_frame_free(&still_frame_);
    still_frame_ = clone;
}

bool Player::grab_still(const std::string &path, std::string *err)
{
    AVFrame *clone = nullptr;
    {
        std::lock_guard<std::mutex> guard(still_mutex_);
        if (still_frame_)
            clone = av_frame_clone(still_frame_);
    }
    if (!clone) {
        if (err)
            *err = "no frame decoded yet";
        return false;
    }
    // TIFF encoding runs outside the lock: it can take a few milliseconds for
    // a large frame, and the feed loop must never wait on it.
    bool ok = save_still_tiff(clone, decoder_.info(), path, err);
    av_frame_free(&clone);
    return ok;
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------
void Player::decode_loop()
{
    uint64_t my_generation = 0;
    // decoder_ is only ever touched from this thread, so the speed it was
    // last told about lives here rather than as a field — MasterClock is
    // already the single source of truth for the current value, updated by
    // whichever control-thread command (position/seek/speed) last reported
    // it, and this just notices when that value has moved.
    double decoder_speed = 1.0;

    for (;;) {
        double seek_to = 0.0;
        bool   do_seek = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&] {
                return stopping_ || seek_pending_ ||
                       queue_.size() < effective_queue_depth(clock_.speed());
            });
            if (stopping_)
                return;
            if (seek_pending_) {
                seek_pending_ = false;
                do_seek       = true;
                seek_to       = seek_target_;
                my_generation = generation_;
            }
        }

        if (do_seek) {
            decoder_.seek(seek_to);
            continue;
        }

        double speed_now = clock_.speed();
        if (speed_now != decoder_speed) {
            decoder_.set_speed(speed_now);
            decoder_speed = speed_now;
        }

        AVFrame *frame = nullptr;
        int rc = decoder_.next_frame(&frame);
        if (rc <= 0) {
            if (rc < 0)
                log_error("player: decode failed");
            // End of stream: idle until a seek or shutdown arrives.  The feed
            // loop keeps the last frame on the wire so the card never starves.
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stopping_ || seek_pending_; });
            if (stopping_)
                return;
            continue;
        }

        double t = decoder_.frame_time(frame);
        {
            std::lock_guard<std::mutex> guard(queue_mutex_);
            // A seek landed while this frame was in flight — it belongs to the
            // previous generation, so drop it.
            if (generation_ != my_generation) {
                my_generation = generation_;
                av_frame_free(&frame);
                continue;
            }
            queue_.push_back(QueuedFrame{frame, t});
            // Audio rode in on the same demux pass. Moving it across under the
            // same lock and the same generation check keeps it in step with the
            // video: a seek discards both or neither.
            AudioChunk chunk;
            while (decoder_.take_audio(&chunk))
                audio_queue_.push_back(std::move(chunk));
        }
        queue_cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------
// Feed thread
// ---------------------------------------------------------------------------
// Every scheduled output frame goes through here, which is why the audio is
// fed here too rather than at the four call sites: the card timestamps audio
// off the video frame counter, so "one output frame's worth of samples per
// scheduled output frame" has to hold on every path — the ordinary one, the
// starved one, the paused one and the post-reseek one alike. Tying the two
// together in one place makes that structural instead of remembered.
bool Player::send_canvas(int repeat, double source_time)
{
    bool sent;
    if (converter_.is_planar()) {
        sent = output_->send_planes(converter_.plane(0), converter_.stride(0),
                                    converter_.plane(1), converter_.stride(1),
                                    converter_.plane(2), converter_.stride(2),
                                    repeat, source_time);
    } else {
        sent = output_->send_packed(converter_.plane(0), converter_.stride(0),
                                    repeat, source_time);
    }
    // A dropped video frame advances neither axis, so its audio must not go
    // out either.
    if (sent)
        feed_audio(repeat, source_time);
    return sent;
}

// ---------------------------------------------------------------------------
// Audio feed
// ---------------------------------------------------------------------------
//
// Audio is scheduled against the same stream-time axis as video: the shim
// derives its audio timestamp from the video frame counter, so the invariant
// this code exists to hold is that exactly one output frame's worth of samples
// goes out per scheduled output frame. Send too few or too many and the two
// axes separate permanently, which is heard as the sound sliding off the
// picture.
//
// Within that constraint the cursor is deliberately independent of the video
// servo. The servo trims the pulldown accumulator by hundredths of a frame to
// hold sync; following those trims in audio would mean resampling or dropping
// samples continuously, and a correction invisible in the picture is an
// audible click. So audio runs free and is only re-anchored to the picture on
// a seek, or if the two somehow drift far enough apart to be noticed anyway.
//
// "Runs free" is on the output (wall-clock) time axis, not source time — the
// two are identical at 1x, which is what lets this code stay unaware of
// speed changes entirely; decoder.h's AudioChunk comment has the reasoning
// for why chunks away from 1x are already timestamped that way.

int Player::pull_audio(double from, int nframes, int32_t *dst)
{
    const int ch = audio_channels_;
    int filled  = 0;
    int silence = 0;

    std::lock_guard<std::mutex> guard(queue_mutex_);
    while (filled < nframes) {
        if (audio_queue_.empty())
            break;

        AudioChunk &chunk = audio_queue_.front();
        const double want = from + (double)filled / DLK_AUDIO_RATE;
        // Where in this chunk that instant falls.
        long offset = lround((want - chunk.time) * DLK_AUDIO_RATE);

        if (offset >= chunk.nframes) {
            audio_queue_.pop_front();   // entirely behind us
            continue;
        }
        if (offset < 0) {
            // A hole: the next decoded audio starts later than the instant we
            // need. Fill up to it rather than pulling the future forward.
            long gap = std::min<long>(-offset, nframes - filled);
            std::fill_n(dst + (size_t)filled * ch, (size_t)gap * ch, 0);
            filled  += (int)gap;
            silence += (int)gap;
            continue;
        }

        const int avail = chunk.nframes - (int)offset;
        const int take  = std::min(avail, nframes - filled);
        std::copy_n(chunk.samples.data() + (size_t)offset * ch,
                    (size_t)take * ch,
                    dst + (size_t)filled * ch);
        filled += take;
        if (take == avail)
            audio_queue_.pop_front();
    }

    if (filled < nframes) {
        // Nothing decoded for the rest — the decoder is behind, or we are past
        // the end of the file.
        std::fill_n(dst + (size_t)filled * ch, (size_t)(nframes - filled) * ch, 0);
        silence += nframes - filled;
    }
    return silence;
}

void Player::feed_audio(int output_frames, double source_time)
{
    if (audio_channels_ <= 0 || output_frames <= 0 || mode_fps_ <= 0.0)
        return;

    // Samples per output frame is rarely a whole number — 48000/29.97 is
    // 1601.6 — so the remainder is carried rather than rounded away, which
    // would drift by 36 samples a second.
    const double exact = output_frames * (double)DLK_AUDIO_RATE / mode_fps_ + audio_frac_;
    int need = (int)exact;
    audio_frac_ = exact - need;
    if (need <= 0)
        return;
    if ((size_t)need * audio_channels_ > audio_scratch_.size())
        audio_scratch_.resize((size_t)need * audio_channels_);

    // audio_cursor_ lives on the same axis AudioChunk.time does: output
    // (wall-clock) time — see the AudioChunk comment in decoder.h.
    // clock_.output_position() is source_time with speed's effect correctly
    // removed; it is *not* source_time/clock_.speed(), which looks
    // equivalent but silently assumes the current speed was always in
    // effect. That assumption holds until the first speed change and is
    // wrong for every one after — output_position() stays correct by
    // construction because it never makes it, closing out every interval at
    // whatever speed actually applied instead of re-deriving the whole
    // history from whatever speed is current now.
    const double output_time = clock_.output_position();

    if (audio_reanchor_.exchange(false))
        audio_cursor_ = -1.0;
    if (audio_cursor_ < 0.0)
        audio_cursor_ = output_time;

    int silence;
    if (clock_.paused() || blackout_.load()) {
        // Nothing to play — either IINA paused, or the picture is blanked for
        // focus loss and the SDI feed has nothing worth sending either.
        // Either way the slot is still consumed so the timestamp axes stay
        // together once playback resumes.
        std::fill_n(audio_scratch_.data(), (size_t)need * audio_channels_, 0);
        silence = need;
        audio_cursor_ = output_time;   // stay glued to the picture while muted
    } else {
        if (std::fabs(audio_cursor_ - output_time) > kAudioResyncSeconds) {
            audio_cursor_ = output_time;
            audio_resyncs_++;
        }
        silence = pull_audio(audio_cursor_, need, audio_scratch_.data());
        audio_cursor_ += (double)need / DLK_AUDIO_RATE;
    }

    output_->send_audio(audio_scratch_.data(), need);
    audio_frames_  += need;
    audio_silence_ += silence;
}

// Owns one reference to a decoded frame, so the feed loop can work on it
// outside the queue lock without a concurrent seek freeing it underneath.
namespace {
struct FrameRef {
    AVFrame *frame = nullptr;
    ~FrameRef() { if (frame) av_frame_free(&frame); }
    FrameRef() = default;
    FrameRef(const FrameRef &) = delete;
    FrameRef &operator=(const FrameRef &) = delete;
};
}  // namespace

void Player::feed_loop()
{
    const double frame_period = mode_fps_ > 0.0 ? 1.0 / mode_fps_ : 1.0 / 30.0;
    const double src_fps = decoder_.info().fps > 0.0 ? decoder_.info().fps : mode_fps_;
    // Frame-rate conversion only — how many output slots one source frame
    // occupies at normal speed. The actual per-iteration ratio also divides
    // by the current playback speed (see below), so this alone is not what
    // drives the accumulator.
    const double base_ratio = src_fps > 0.0 ? mode_fps_ / src_fps : 1.0;
    const double offset  = cfg_.offset_ms / 1000.0;
    const double gain     = cfg_.servo_gain     > 0.0 ? cfg_.servo_gain     : kServoGain;
    const double deadband = cfg_.servo_deadband > 0.0 ? cfg_.servo_deadband : kDeadbandFrames;

    bool have_canvas = false;
    bool was_blacked_out = false;
    const auto feed_start = std::chrono::steady_clock::now();

    // Wait for a few decoded frames before feeding anything.
    //
    // The frame we schedule now reaches the wire once the hardware queue ahead
    // of it has drained, so the correct frame to send first is the one at
    // roughly (position + queue depth), not the one at the current position.
    // Selecting that requires a choice of frames — with an empty queue the loop
    // would emit whatever decoded first and then race to catch up, which shows
    // as a burst of fast playback at every start and seek.
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return stopping_ || queue_.size() >= kPrerollFrames;
        });
    }

    // Anchor the schedule to now.
    //
    // The card's playback clock has been running since the device was opened,
    // but the first frame isn't scheduled until the decoder has warmed up and
    // the queue above has filled — several hundred milliseconds later.  Frame
    // numbering still starts at zero, so without this those first frames are
    // scheduled into slots that have already gone by, and the output ends up
    // permanently running ahead of the master by exactly that startup delay.
    output_->resync();

    while (!stopping_) {
        if (!output_->can_send()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (blackout_.load()) {
            // Filled once per blackout period, not every iteration — a
            // per-pixel canvas fill on every trip through this loop would be
            // wasted work for a picture that never changes until this ends.
            if (!was_blacked_out) {
                converter_.black_out();
                have_canvas = true;
                was_blacked_out = true;
            }
            // last_frame_time_ is deliberately left alone: it still names the
            // last *real* source frame shown, which is what a still grab
            // during blackout should keep returning, and what feed_audio
            // needs as a source_time argument to keep its own bookkeeping
            // sane even though blackout silences the actual samples.
            send_canvas(1, last_frame_time_.load());
            continue;
        }
        was_blacked_out = false;

        // Where the wire should be: IINA's position, plus the depth of the
        // hardware queue we are scheduling behind, plus the manual trim.
        int inflight = output_->buffered_frames();
        double target = clock_.position() + inflight * frame_period + offset;

        // Pick the queued frame nearest the target, discarding everything that
        // is already too old to be shown.  The chosen frame is cloned — a
        // refcounted shallow copy — so request_seek() can clear the queue at
        // any moment without pulling it out from under the conversion below.
        FrameRef chosen;
        double chosen_time = 0.0;
        bool starved = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            while (queue_.size() >= 2 &&
                   queue_[1].time <= target + frame_period * 0.5) {
                av_frame_free(&queue_.front().frame);
                queue_.pop_front();
                dropped_++;
            }
            if (!queue_.empty()) {
                chosen.frame = av_frame_clone(queue_.front().frame);
                chosen_time  = queue_.front().time;
            } else {
                starved = true;
            }
        }
        queue_cv_.notify_all();

        if (starved || !chosen.frame) {
            // Nothing decoded yet (or the clone failed).  Re-show whatever is
            // already on the canvas so the card keeps receiving frames rather
            // than underrunning.
            if (have_canvas) {
                repeated_++;
                send_canvas(1, last_frame_time_.load());
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
        }

        double error = chosen_time - target;
        last_error_.store(error);

        // Skip the first stretch so the startup transient stays out of the
        // statistics — it is a real effect, but it is not the steady-state
        // behaviour these numbers are meant to describe.
        if (std::chrono::steady_clock::now() - feed_start > std::chrono::milliseconds(750) &&
            !clock_.paused()) {
            std::lock_guard<std::mutex> guard(stats_mutex_);
            err_sum_    += error;
            err_sum_sq_ += error * error;
            if (err_count_ == 0 || error < err_min_) err_min_ = error;
            if (err_count_ == 0 || error > err_max_) err_max_ = error;
            err_count_++;
        }

        // Too far out to servo: re-seek and re-anchor the schedule.  Also how
        // an unannounced position jump gets handled.
        if (std::fabs(error) > kReseekSeconds) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_reseek_ < kReseekCooldown) {
                // Still settling from the last one; hold the current picture
                // rather than throwing away the decode already in flight.
                if (have_canvas)
                    send_canvas(1, last_frame_time_.load());
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            log_debug("player: %.3fs out of sync — reseeking to %.3f", error, target);
            last_reseek_ = now;
            reseeks_++;
            request_seek(target);
            output_->resync();
            phase_ = 0.0;
            last_frame_time_.store(-1.0);
            continue;
        }

        // While paused the target stops moving, so hold the current frame on
        // the wire rather than letting the card starve.
        if (clock_.paused()) {
            if (chosen_time != last_frame_time_.load()) {
                std::string err;
                if (converter_.convert(chosen.frame, &err)) {
                    last_frame_time_.store(chosen_time);
                    have_canvas = true;
                    update_still_frame(chosen.frame);
                } else {
                    log_error("player: %s", err.c_str());
                }
            }
            if (have_canvas)
                send_canvas(1, last_frame_time_.load());
            continue;
        }

        // How many output frames this source frame should occupy.  The
        // accumulator keeps non-integer ratios honest, so 23.976p on 60p comes
        // out as a proper 3:2 cadence rather than jitter.
        //
        // Drift is corrected by nudging the accumulator rather than by adding
        // or dropping a whole frame.  A discrete ±1 correction settles
        // anywhere inside its deadband, leaving a standing bias of up to a
        // frame and a half; feeding a small proportional term into the
        // accumulator instead lets the correction spread itself across the
        // cadence and drive the error to zero, while the deadband below keeps
        // sub-half-frame noise from perturbing the pattern at all.
        double trim = 0.0;
        if (std::fabs(error) > deadband * frame_period) {
            trim = std::clamp(error / frame_period, -1.0, 1.0) * gain;
        }

        // Read fresh every iteration, since speed can change mid-stream. The
        // output device fills its schedule at a fixed real-time rate no
        // matter what this is (backpressure via can_send() sees to that), so
        // dividing by speed here is what makes a slower speed hold each
        // source frame across more output slots (fewer, longer-held source
        // frames per unit real time) instead of just — with no other change
        // anywhere in this function — silently continuing to consume source
        // material at the 1x rate while the reported position crawls behind
        // it, which is what every source frame simply racing ahead of target
        // looks like from outside: climbing error, climbing drops, and
        // reseeks that never resolve because the next iteration recreates
        // the exact same mismatch.
        double ratio = base_ratio / std::max(0.1, clock_.speed());

        double next_phase = phase_ + ratio + trim;
        int hold = (int)std::floor(next_phase) - (int)std::floor(phase_);
        phase_ = next_phase;
        if (phase_ > 1e6) {          // keep the accumulator from drifting off
            phase_ -= std::floor(phase_);
        }

        // Retires the frame we just handled from the queue, if a seek hasn't
        // already replaced it.
        auto retire = [&] {
            std::lock_guard<std::mutex> guard(queue_mutex_);
            if (!queue_.empty() && queue_.front().time == chosen_time) {
                av_frame_free(&queue_.front().frame);
                queue_.pop_front();
            }
        };

        if (hold <= 0) {
            // Source runs faster than the output mode (or we are catching up):
            // this frame never reaches the wire.
            retire();
            dropped_++;
            queue_cv_.notify_all();
            continue;
        }

        if (chosen_time != last_frame_time_.load()) {
            std::string err;
            if (!converter_.convert(chosen.frame, &err)) {
                log_error("player: %s", err.c_str());
                retire();
                queue_cv_.notify_all();
                continue;
            }
            last_frame_time_.store(chosen_time);
            have_canvas = true;
            update_still_frame(chosen.frame);
        }

        send_canvas(hold, chosen_time);
        retire();
        queue_cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------
PlayerStatus Player::status() const
{
    PlayerStatus s;
    s.running   = running_;
    s.width     = out_width_;
    s.height    = out_height_;
    s.mode_fps  = mode_fps_;
    s.mode_code = mode_code_;
    s.pixfmt    = actual_pixfmt_;
    s.hardware  = decoder_.info().hardware;
    s.position  = last_frame_time_.load();
    s.error_ms  = last_error_.load() * 1000.0;
    {
        std::lock_guard<std::mutex> guard(stats_mutex_);
        s.error_samples = err_count_;
        if (err_count_ > 0) {
            double mean = err_sum_ / (double)err_count_;
            s.error_mean_ms = mean * 1000.0;
            s.error_rms_ms  = std::sqrt(err_sum_sq_ / (double)err_count_) * 1000.0;
            s.error_min_ms  = err_min_ * 1000.0;
            s.error_max_ms  = err_max_ * 1000.0;
        }
    }
    s.dropped   = dropped_;
    s.repeated  = repeated_;
    s.reseeks   = reseeks_;
    s.audio     = audio_on_ && audio_channels_ > 0;
    s.audio_frames  = audio_frames_;
    s.audio_silence = audio_silence_;
    s.audio_resyncs = audio_resyncs_;
    return s;
}
