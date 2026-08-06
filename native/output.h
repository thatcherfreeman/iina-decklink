/*
 * The card, behind an interface.
 *
 * DeckLinkOutput is the real thing, wrapping decklink_shim.  NullOutput
 * simulates a card that consumes frames at the mode rate against the wall
 * clock, which is what makes the feed loop — pulldown cadence, drift
 * correction, seek re-anchoring — testable with no hardware attached.
 */

#ifndef IINA_DECKLINK_OUTPUT_H
#define IINA_DECKLINK_OUTPUT_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "decklink_shim.h"

struct OutputOpenParams {
    std::string device;
    std::string mode_code;
    int    src_width  = 0;
    int    src_height = 0;
    double src_fps    = 0.0;
    int    pixfmt     = DLK_PIXFMT_V210;
    int    preroll    = 3;
    bool   enable_audio   = false;
    int    audio_channels = 2;
    bool   rgb_legal      = true;
    int    fixed_width  = 0;
    int    fixed_height = 0;
    int    link_mode    = DLK_LINK_SINGLE;
    // Test hook, honoured only by NullOutput: pin the simulated mode rate so
    // pulldown cadences can be exercised without hunting for a device that
    // lacks the source's own frame rate.
    double force_fps    = 0.0;
    // Test hook, honoured only by NullOutput: this many seconds after opening,
    // stop consuming frames — the simulated card wedges with its queue full and
    // never retires another frame.  This is the fault the feed loop's watchdog
    // exists to report, and the only way to exercise that reporting without
    // waiting for real hardware to do it.
    double stall_after  = 0.0;
};

struct OutputInfo {
    int    width  = 0;
    int    height = 0;
    double fps    = 0.0;
    std::string mode_code;
    int    pixfmt = 0;   // as negotiated; may differ from what was requested
    bool   audio  = false;
};

// What the card says about itself, for the feed loop's watchdog.  Mirrors
// dlk_health (see decklink_shim.h for what each counter distinguishes);
// NullOutput fills in the few it can answer honestly and leaves the rest zero,
// so the watchdog code is the same either way.
struct OutputHealth {
    int     frames_in_flight = 0;
    int     inflight_limit   = 0;
    int     buffered_video   = 0;     // -1 if the driver wouldn't say
    int     buffered_audio   = 0;
    double  stream_time      = -1.0;  // hardware clock, seconds; -1 if unavailable
    int64_t scheduled        = 0;
    int64_t completed        = 0;
    int64_t late             = 0;
    int64_t dropped          = 0;
    int64_t flushed          = 0;
    int64_t schedule_errors  = 0;
    int64_t audio_errors     = 0;
    int32_t last_error       = 0;
    bool    playback_stopped = false;
};

class VideoOutput {
public:
    virtual ~VideoOutput() = default;

    virtual bool open(const OutputOpenParams &p, std::string *err) = 0;
    virtual void close() = 0;
    virtual const OutputInfo &info() const = 0;

    virtual bool can_send() = 0;
    virtual int  buffered_frames() = 0;
    // source_time is the presentation time of the frame being scheduled.  The
    // real card ignores it; NullOutput records it so tests can reconstruct
    // exactly which source frame occupied each output slot, and compare that
    // against where the master clock was at the moment it went out.
    virtual bool send_packed(const uint8_t *data, int stride, int repeat,
                             double source_time) = 0;
    virtual bool send_planes(const uint8_t *y, int ys, const uint8_t *u, int us,
                             const uint8_t *v, int vs, int repeat,
                             double source_time) = 0;
    // Interleaved 48 kHz signed 32-bit PCM, in the channel count the output
    // was opened with.  Returns the sample frames accepted.  The card
    // timestamps these against the same stream-time axis as video, so the
    // caller must send exactly one output frame's worth per scheduled frame
    // or the two axes drift apart.
    virtual int  send_audio(const int32_t *interleaved, int nframes) = 0;
    virtual int  buffered_audio_frames() = 0;
    virtual void resync() = 0;
    // A snapshot of whatever the output can say about its own state.  Cheap
    // enough to take once a second from the feed loop; see OutputHealth.
    virtual OutputHealth health() = 0;
};

// The real card.
class DeckLinkOutput : public VideoOutput {
public:
    ~DeckLinkOutput() override;
    bool open(const OutputOpenParams &p, std::string *err) override;
    void close() override;
    const OutputInfo &info() const override { return info_; }
    bool can_send() override;
    int  buffered_frames() override;
    bool send_packed(const uint8_t *data, int stride, int repeat,
                     double source_time) override;
    bool send_planes(const uint8_t *y, int ys, const uint8_t *u, int us,
                     const uint8_t *v, int vs, int repeat,
                     double source_time) override;
    int  send_audio(const int32_t *interleaved, int nframes) override;
    int  buffered_audio_frames() override;
    void resync() override;
    OutputHealth health() override;

private:
    dlk_output *out_ = nullptr;
    OutputInfo  info_;
};

// A card that isn't there.  Consumes frames at the mode rate against the wall
// clock and records the hold count of every scheduled frame, so tests can
// assert on the cadence the feed loop actually produced.
class NullOutput : public VideoOutput {
public:
    bool open(const OutputOpenParams &p, std::string *err) override;
    void close() override;
    const OutputInfo &info() const override { return info_; }
    bool can_send() override;
    int  buffered_frames() override;
    bool send_packed(const uint8_t *data, int stride, int repeat,
                     double source_time) override;
    bool send_planes(const uint8_t *y, int ys, const uint8_t *u, int us,
                     const uint8_t *v, int vs, int repeat,
                     double source_time) override;
    int  send_audio(const int32_t *interleaved, int nframes) override;
    int  buffered_audio_frames() override;
    void resync() override;
    OutputHealth health() override;

    // Sample frames handed over, and how many of those were silence.  The
    // simulated card accepts everything; these exist so the audio path is
    // exercised and measurable with no hardware attached.
    int64_t audio_frames() const;

    // What went out, and when.  One entry per scheduled frame: the output
    // frame index its display started at, how long it was held, and the source
    // time it carried.
    struct ScheduleEntry {
        int64_t output_index;
        int     hold;
        double  source_time;
    };
    std::vector<ScheduleEntry> schedule_log() const;
    // Wall-clock seconds from the card starting to output frame `index`
    // reaching the wire.
    double display_time(int64_t index) const { return (double)index / info_.fps; }
    double started_at() const;

    // The sequence of hold counts, oldest first.  For a 23.976p source on a
    // 60p output this should settle into 3,2,3,2,...
    std::vector<int> cadence() const;
    int64_t scheduled_frames() const;

private:
    int64_t consumed() const;
    bool schedule(int repeat, double source_time);

    mutable std::mutex mutex_;
    OutputInfo info_;
    double  stall_after_    = 0.0;   // see OutputOpenParams::stall_after
    int     inflight_limit_ = 6;
    int64_t scheduled_      = 0;
    int64_t audio_frames_   = 0;
    std::vector<int> cadence_;
    std::vector<ScheduleEntry> log_;
    std::chrono::steady_clock::time_point start_;
};

#endif  // IINA_DECKLINK_OUTPUT_H
