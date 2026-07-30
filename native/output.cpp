#include "output.h"

#include <cmath>

#include "log.h"

// ---------------------------------------------------------------------------
// DeckLinkOutput
// ---------------------------------------------------------------------------
DeckLinkOutput::~DeckLinkOutput()
{
    close();
}

bool DeckLinkOutput::open(const OutputOpenParams &p, std::string *err)
{
    close();

    out_ = dlk_output_create(p.device.empty() ? nullptr : p.device.c_str(),
                             p.mode_code.empty() ? nullptr : p.mode_code.c_str(),
                             p.src_width, p.src_height, p.src_fps,
                             p.pixfmt, p.preroll,
                             p.enable_audio ? 1 : 0, p.audio_channels,
                             p.rgb_legal ? 1 : 0,
                             p.fixed_width, p.fixed_height,
                             p.link_mode);
    if (!out_) {
        if (err)
            *err = "could not open the DeckLink output (see log for details)";
        return false;
    }

    char code[5] = {};
    int audio_on = 0;
    dlk_output_get_info(out_, &info_.width, &info_.height, &info_.fps, code,
                        &audio_on, &info_.pixfmt);
    info_.mode_code = code;
    info_.audio     = audio_on != 0;
    return true;
}

void DeckLinkOutput::close()
{
    if (out_) {
        dlk_output_destroy(out_);
        out_ = nullptr;
    }
    info_ = OutputInfo{};
}

bool DeckLinkOutput::can_send()
{
    return out_ && dlk_output_can_send(out_) != 0;
}

int DeckLinkOutput::buffered_frames()
{
    return out_ ? dlk_output_buffered_video_frames(out_) : 0;
}

bool DeckLinkOutput::send_packed(const uint8_t *data, int stride, int repeat,
                                 double source_time)
{
    (void)source_time;
    return out_ && dlk_output_send_packed(out_, data, stride, repeat) != 0;
}

bool DeckLinkOutput::send_planes(const uint8_t *y, int ys, const uint8_t *u,
                                 int us, const uint8_t *v, int vs, int repeat,
                                 double source_time)
{
    (void)source_time;
    return out_ && dlk_output_send_planes(out_, y, ys, u, us, v, vs, repeat) != 0;
}

int DeckLinkOutput::send_audio(const int32_t *interleaved, int nframes)
{
    return out_ ? dlk_output_send_audio(out_, interleaved, nframes) : 0;
}

int DeckLinkOutput::buffered_audio_frames()
{
    return out_ ? dlk_output_buffered_audio_frames(out_) : 0;
}

void DeckLinkOutput::resync()
{
    if (out_)
        dlk_output_resync(out_);
}

// ---------------------------------------------------------------------------
// NullOutput
// ---------------------------------------------------------------------------
namespace {

// A representative set of BMD-supported rates, so the simulated negotiation
// behaves like the real find_mode() ladder: exact match, then the smallest
// clean integer multiple, then whatever is nearest 30.
constexpr double kCommonFps[] = {23.976, 24.0, 25.0, 29.97, 30.0,
                                 47.952, 48.0, 50.0, 59.94, 60.0};

double negotiate_fps(double want)
{
    if (want <= 0.0)
        return 30.0;
    for (double f : kCommonFps) {
        if (std::fabs(f - want) < 0.02)
            return f;
    }
    double best = 0.0;
    int best_ratio = 0;
    for (double f : kCommonFps) {
        double ratio = f / want;
        int iratio = (int)std::lround(ratio);
        if (iratio >= 1 && iratio <= 4 && std::fabs(ratio - iratio) < 0.01 &&
            (best == 0.0 || iratio < best_ratio)) {
            best = f;
            best_ratio = iratio;
        }
    }
    if (best != 0.0)
        return best;

    double nearest = kCommonFps[0];
    for (double f : kCommonFps) {
        if (std::fabs(f - 30.0) < std::fabs(nearest - 30.0))
            nearest = f;
    }
    return nearest;
}

}  // namespace

bool NullOutput::open(const OutputOpenParams &p, std::string *err)
{
    (void)err;
    std::lock_guard<std::mutex> guard(mutex_);

    if (p.fixed_width > 0 && p.fixed_height > 0) {
        info_.width  = p.fixed_width;
        info_.height = p.fixed_height;
    } else if (p.src_width > 1920) {
        info_.width  = 3840;
        info_.height = 2160;
    } else {
        info_.width  = 1920;
        info_.height = 1080;
    }
    info_.fps       = p.force_fps > 0.0 ? p.force_fps : negotiate_fps(p.src_fps);
    info_.mode_code = "null";
    info_.pixfmt    = p.pixfmt;
    info_.audio     = p.enable_audio;

    inflight_limit_ = (p.preroll > 0 ? p.preroll : 3) * 2;
    scheduled_      = 0;
    cadence_.clear();
    log_.clear();
    start_ = std::chrono::steady_clock::now();

    log_info("null output: %dx%d @ %.3f fps (simulated)",
             info_.width, info_.height, info_.fps);
    return true;
}

void NullOutput::close()
{
    std::lock_guard<std::mutex> guard(mutex_);
    scheduled_ = 0;
    cadence_.clear();
    log_.clear();
}

int64_t NullOutput::consumed() const
{
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    return (int64_t)(elapsed * info_.fps);
}

bool NullOutput::can_send()
{
    std::lock_guard<std::mutex> guard(mutex_);
    return scheduled_ - consumed() < inflight_limit_;
}

int NullOutput::buffered_frames()
{
    std::lock_guard<std::mutex> guard(mutex_);
    int64_t n = scheduled_ - consumed();
    return n > 0 ? (int)n : 0;
}

bool NullOutput::schedule(int repeat, double source_time)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (scheduled_ - consumed() >= inflight_limit_)
        return false;
    if (repeat < 1)
        repeat = 1;
    // Frames scheduled into slots that have already elapsed can't go back in
    // time; the hardware would flush them out immediately.  Model that so the
    // simulation doesn't silently absorb a scheduling bug the real card would
    // expose.
    if (scheduled_ < consumed())
        scheduled_ = consumed();
    log_.push_back(ScheduleEntry{scheduled_, repeat, source_time});
    scheduled_ += repeat;
    cadence_.push_back(repeat);
    return true;
}

bool NullOutput::send_packed(const uint8_t *data, int stride, int repeat,
                             double source_time)
{
    (void)data;
    (void)stride;
    return schedule(repeat, source_time);
}

bool NullOutput::send_planes(const uint8_t *y, int ys, const uint8_t *u, int us,
                             const uint8_t *v, int vs, int repeat,
                             double source_time)
{
    (void)y; (void)ys; (void)u; (void)us; (void)v; (void)vs;
    return schedule(repeat, source_time);
}

// The simulated card swallows audio and only counts it.  There is nothing to
// hear, but the accounting still catches the failure that matters: sending the
// wrong number of sample frames per scheduled video frame, which on real
// hardware walks the audio off the video timestamp axis.
int NullOutput::send_audio(const int32_t *interleaved, int nframes)
{
    (void)interleaved;
    if (nframes <= 0)
        return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    audio_frames_ += nframes;
    return nframes;
}

int NullOutput::buffered_audio_frames()
{
    return 0;
}

int64_t NullOutput::audio_frames() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return audio_frames_;
}

void NullOutput::resync()
{
    std::lock_guard<std::mutex> guard(mutex_);
    scheduled_ = consumed() + 2;
}

std::vector<NullOutput::ScheduleEntry> NullOutput::schedule_log() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return log_;
}

double NullOutput::started_at() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return std::chrono::duration<double>(start_.time_since_epoch()).count();
}

std::vector<int> NullOutput::cadence() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return cadence_;
}

int64_t NullOutput::scheduled_frames() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return scheduled_;
}
