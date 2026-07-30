/*
 * Demux + decode, with VideoToolbox hardware acceleration and a transparent
 * software fallback.
 *
 * IINA is already decoding the same file, so this is the second decode of it;
 * hardware acceleration is what keeps that affordable.  VideoToolbox is a
 * decoder only — it emits the coded YUV samples and performs no color
 * management — so hardware and software paths produce the same code values,
 * which is what makes it acceptable for a reference-monitor feed.
 */

#ifndef IINA_DECKLINK_DECODER_H
#define IINA_DECKLINK_DECODER_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
}

enum class HwMode {
    Auto,          // VideoToolbox when the codec supports it, else software
    VideoToolbox,  // fail rather than silently fall back (for diagnosis)
    Software,      // never use hardware — required for 4:4:4 and 12-bit
};

struct SourceInfo {
    int    width  = 0;
    int    height = 0;
    double fps    = 0.0;   // nominal frame rate; 0 if unknown
    double duration = 0.0; // seconds; 0 if unknown (live/streaming)

    // Carried through to swscale so the conversion uses the source's own
    // matrix and range.  Primaries and transfer are recorded for reporting
    // only — they are deliberately never acted on, so HDR material reaches
    // the card with its original code values.
    AVColorSpace                  colorspace = AVCOL_SPC_UNSPECIFIED;
    AVColorRange                  color_range = AVCOL_RANGE_UNSPECIFIED;
    AVColorPrimaries              primaries  = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer   = AVCOL_TRC_UNSPECIFIED;

    std::string codec_name;
    bool        hardware = false;  // decoding on VideoToolbox

    bool has_audio     = false;
    int  audio_channels = 0;
    int  audio_rate     = 0;
};

// A run of decoded audio, resampled to exactly what the card takes: 48 kHz
// signed 32-bit interleaved, in the channel count the output was opened with.
//
// `time` is presentation time, but which axis depends on whether the chunk
// was tempo-corrected. At 1x it is source time, exactly as playing normally.
// Away from 1x, atempo has already changed how many samples cover a given
// span of source material — a chunk built from tempo=2 audio holds half as
// many samples per second of *source* time, but still exactly 48000 samples
// per second of *output* time, since that's what it will actually take to
// play — so `time` there is output (wall-clock) time instead, expressed on
// the same axis as MasterClock's `position / speed`. At speed 1 the two axes
// coincide, which is what lets the player-side code stay speed-agnostic.
struct AudioChunk {
    std::vector<int32_t> samples;   // nframes * channels
    int    nframes = 0;
    double time    = 0.0;
};

class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    // audio_channels is the count the card was opened with — 2, 8 or 16 — or
    // 0 to skip audio entirely.  Decoding it here rather than in the player
    // keeps the resample (whatever the source is → 48 kHz S32 interleaved) in
    // one place, and means the player only ever sees card-ready samples.
    bool open(const std::string &path, HwMode mode, int audio_channels,
              std::string *err);
    void close();

    const SourceInfo &info() const { return info_; }
    bool is_open() const { return fmt_ != nullptr; }

    // Decodes the next video frame.
    //   1  got a frame — caller owns *out and must av_frame_free() it
    //   0  end of stream
    //  -1  error
    //
    // The returned frame may be VideoToolbox-backed (format
    // AV_PIX_FMT_VIDEOTOOLBOX, CVPixelBufferRef in data[3]); Converter handles
    // both that and ordinary software frames.
    int next_frame(AVFrame **out);

    // Audio decoded as a side effect of next_frame(): the demux loop is shared,
    // so audio packets met while looking for the next video frame are decoded
    // and parked here.  Returns false when nothing is waiting.
    bool take_audio(AudioChunk *out);
    bool audio_active() const { return audio_ctx_ != nullptr; }

    // Sets the pitch-preserving tempo applied to decoded audio, matching
    // whatever IINA is reporting as its playback speed. 1.0 turns tempo
    // correction off entirely — audio then flows through exactly the same
    // path it always has. Takes effect on the next decoded audio frame; only
    // ever call this from the thread that also calls next_frame(), which owns
    // every other piece of decode state.
    void set_speed(double speed);

    // Seeks to the keyframe at or before `seconds` and flushes the decoder.
    // Frames returned afterwards may start slightly earlier than requested;
    // the caller is expected to discard frames before its target.
    bool seek(double seconds);

    // Presentation time of a decoded frame, in seconds.
    double frame_time(const AVFrame *frame) const;

private:
    bool open_video_stream(HwMode mode, std::string *err);
    bool init_hardware(const AVCodec *codec, std::string *err);
    bool open_audio_stream(int channels);
    bool init_resampler(const AVFrame *frame);
    void drain_audio_frames();
    bool init_tempo_filter(const AVFrame *frame);
    void teardown_tempo_filter();
    void flush_tempo_filter();
    void resample_and_queue(const AVFrame *frame);

    AVFormatContext *fmt_        = nullptr;
    AVCodecContext  *video_ctx_  = nullptr;
    AVBufferRef     *hw_device_  = nullptr;
    AVPacket        *packet_     = nullptr;
    int              video_index_ = -1;
    int              audio_index_ = -1;
    bool             eof_sent_   = false;
    SourceInfo       info_;

    // Audio path.  Null throughout when the output has no audio enabled, the
    // file has no audio stream, or its decoder wouldn't open — none of which
    // is fatal to a video feed.
    AVCodecContext  *audio_ctx_  = nullptr;
    SwrContext      *swr_        = nullptr;
    AVFrame         *audio_frame_ = nullptr;
    int              out_channels_ = 0;
    // Where the next decoded sample belongs, carried across frames so chunk
    // times come from the sample count rather than from rounded container
    // timestamps.  Negative until anchored by the first PTS.  In output
    // (wall-clock) time once tempo correction is running — see the
    // AudioChunk comment above — which is why a second, purely source-domain
    // tracker exists below rather than reusing this one for discontinuity
    // detection.
    double           audio_next_time_ = -1.0;
    // Expected source PTS of the next decoded frame, advanced by each frame's
    // own duration. Never touched by speed — comparing an incoming frame's
    // real PTS against this is how a genuine content discontinuity is told
    // apart from an ordinary speed change, which is not one.
    double           audio_source_next_ = -1.0;
    // What the resampler was built for, so a mid-stream change is noticed.
    int              swr_rate_    = 0;
    int              swr_format_  = -1;
    AVChannelLayout  swr_layout_  = {};
    std::deque<AudioChunk> audio_queue_;

    // Pitch-preserving tempo correction, ahead of the resample above.  Built
    // lazily and only when actually needed — at speed 1.0 none of this is
    // touched, so the already-verified 1x audio path is unchanged.
    double           audio_speed_  = 1.0;   // last speed passed to set_speed()
    bool             tempo_dirty_  = false; // true → rebuild before next frame
    AVFilterGraph   *tempo_graph_    = nullptr;
    AVFilterContext *tempo_src_ctx_  = nullptr;
    AVFilterContext *tempo_sink_ctx_ = nullptr;
    AVFrame         *tempo_frame_    = nullptr;  // reused abuffersink output
    // Fed to abuffer as each frame's timestamp: a running input sample count,
    // not the container's own PTS — atempo only needs a consistent, gapless
    // axis to reason about durations on, and container timestamps carry the
    // same rounding jitter documented below for the non-tempo path.
    int64_t          tempo_pts_       = 0;
    // What the graph was built for, so a mid-stream format change (or a
    // sample rate change from re-seeking into a different part of a VFR/VBR
    // file) is noticed the same way init_resampler() notices one.
    int              tempo_rate_   = 0;
    int              tempo_format_ = -1;
    AVChannelLayout  tempo_layout_ = {};
};

#endif  // IINA_DECKLINK_DECODER_H
