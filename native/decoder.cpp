#include "decoder.h"

#include <cmath>
#include <cstdio>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include "decklink_shim.h"   // DLK_AUDIO_RATE — the card's fixed 48 kHz
#include "log.h"

// A PTS this far from where the running sample count says we are is a real
// discontinuity in the stream, not timestamp rounding.
static constexpr double kAudioJumpSeconds = 0.05;

// ---------------------------------------------------------------------------
// Hardware format negotiation
// ---------------------------------------------------------------------------

// Called by the decoder once it knows which formats it can produce.  Taking
// AV_PIX_FMT_VIDEOTOOLBOX here is what actually engages hardware decode; if
// it isn't on offer we return the decoder's own first choice, which is the
// software path.  That is the transparent fallback for codecs with no VT
// support at all (prores_raw, VC-1, DNxHD).
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *formats)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX)
            return *p;
    }
    return formats[0];
}

// True if this codec advertises a VideoToolbox hwaccel at all.  Profile-level
// support (HEVC 4:4:4, 12-bit) can still fail later at session creation, which
// is what the reopen-in-software path in open() covers.
static bool codec_supports_videotoolbox(const AVCodec *codec)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg)
            return false;
        if (cfg->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX &&
            (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return true;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Decoder::~Decoder()
{
    close();
}

void Decoder::close()
{
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (video_ctx_) {
        avcodec_free_context(&video_ctx_);
    }
    if (audio_ctx_) {
        avcodec_free_context(&audio_ctx_);
    }
    if (swr_) {
        swr_free(&swr_);
    }
    if (audio_frame_) {
        av_frame_free(&audio_frame_);
    }
    teardown_tempo_filter();
    if (tempo_frame_) {
        av_frame_free(&tempo_frame_);
    }
    av_channel_layout_uninit(&swr_layout_);
    audio_queue_.clear();
    out_channels_      = 0;
    audio_next_time_   = -1.0;
    audio_source_next_ = -1.0;
    swr_rate_     = 0;
    swr_format_   = -1;
    audio_speed_  = 1.0;
    tempo_dirty_  = false;
    tempo_pts_    = 0;
    if (hw_device_) {
        av_buffer_unref(&hw_device_);
    }
    if (fmt_) {
        avformat_close_input(&fmt_);
    }
    video_index_ = -1;
    audio_index_ = -1;
    eof_sent_    = false;
    info_        = SourceInfo{};
}

bool Decoder::open(const std::string &path, HwMode mode, int audio_channels,
                   std::string *err)
{
    close();

    int rc = avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        char buf[256];
        av_strerror(rc, buf, sizeof(buf));
        if (err)
            *err = std::string("could not open input: ") + buf;
        return false;
    }

    if ((rc = avformat_find_stream_info(fmt_, nullptr)) < 0) {
        if (err)
            *err = "could not read stream info";
        close();
        return false;
    }

    video_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index_ < 0) {
        if (err)
            *err = "no video stream found";
        close();
        return false;
    }
    audio_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (!open_video_stream(mode, err)) {
        close();
        return false;
    }

    packet_ = av_packet_alloc();
    if (!packet_) {
        if (err)
            *err = "out of memory";
        close();
        return false;
    }

    // Geometry and color metadata, taken from the codec context so it
    // reflects what the decoder will actually produce.
    AVStream *st = fmt_->streams[video_index_];
    info_.width       = video_ctx_->width;
    info_.height      = video_ctx_->height;
    info_.colorspace  = video_ctx_->colorspace;
    info_.color_range = video_ctx_->color_range;
    info_.primaries   = video_ctx_->color_primaries;
    info_.transfer    = video_ctx_->color_trc;
    info_.codec_name  = avcodec_get_name(video_ctx_->codec_id);

    AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
    info_.fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;

    if (fmt_->duration != AV_NOPTS_VALUE)
        info_.duration = (double)fmt_->duration / AV_TIME_BASE;

    if (audio_index_ >= 0) {
        AVCodecParameters *ap = fmt_->streams[audio_index_]->codecpar;
        info_.has_audio       = true;
        info_.audio_channels  = ap->ch_layout.nb_channels;
        info_.audio_rate      = ap->sample_rate;
    }

    // Audio is optional in every direction: no stream, no decoder, or no
    // request for it all mean the same thing here — a video-only feed.
    if (audio_channels > 0 && audio_index_ >= 0)
        open_audio_stream(audio_channels);

    log_info("decoder: %s %dx%d @ %.3f fps, %s decode",
             info_.codec_name.c_str(), info_.width, info_.height, info_.fps,
             info_.hardware ? "hardware" : "software");
    return true;
}

bool Decoder::open_audio_stream(int channels)
{
    AVStream *st = fmt_->streams[audio_index_];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        log_info("decoder: no decoder for the audio stream — video only");
        return false;
    }

    audio_ctx_ = avcodec_alloc_context3(codec);
    if (!audio_ctx_)
        return false;
    if (avcodec_parameters_to_context(audio_ctx_, st->codecpar) < 0) {
        avcodec_free_context(&audio_ctx_);
        return false;
    }
    audio_ctx_->pkt_timebase = st->time_base;

    if (avcodec_open2(audio_ctx_, codec, nullptr) < 0) {
        log_info("decoder: could not open the audio decoder — video only");
        avcodec_free_context(&audio_ctx_);
        return false;
    }

    audio_frame_ = av_frame_alloc();
    tempo_frame_ = av_frame_alloc();
    if (!audio_frame_ || !tempo_frame_) {
        avcodec_free_context(&audio_ctx_);
        return false;
    }

    out_channels_ = channels;
    log_info("decoder: audio %s %d ch @ %d Hz → %d ch @ %d Hz",
             avcodec_get_name(audio_ctx_->codec_id),
             audio_ctx_->ch_layout.nb_channels, audio_ctx_->sample_rate,
             out_channels_, DLK_AUDIO_RATE);
    return true;
}

// Built lazily from the first decoded frame rather than from the codec
// parameters: for some formats the real sample format and layout are only
// known once decoding has started, and they can change mid-stream.
bool Decoder::init_resampler(const AVFrame *frame)
{
    if (swr_ && frame->sample_rate == swr_rate_ &&
        frame->format == swr_format_ &&
        av_channel_layout_compare(&frame->ch_layout, &swr_layout_) == 0)
        return true;

    if (swr_)
        swr_free(&swr_);

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels_);

    int rc = swr_alloc_set_opts2(&swr_,
                                 &out_layout, AV_SAMPLE_FMT_S32, DLK_AUDIO_RATE,
                                 &frame->ch_layout, (AVSampleFormat)frame->format,
                                 frame->sample_rate,
                                 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || !swr_ || swr_init(swr_) < 0) {
        log_error("decoder: could not build the audio resampler — muting");
        if (swr_)
            swr_free(&swr_);
        return false;
    }

    swr_rate_   = frame->sample_rate;
    swr_format_ = frame->format;
    av_channel_layout_uninit(&swr_layout_);
    av_channel_layout_copy(&swr_layout_, &frame->ch_layout);
    return true;
}

// Resamples one already-tempo-corrected (or, at 1x, untouched) frame to the
// card's fixed format and parks the result on the queue. The timestamp comes
// from audio_next_time_, a running count of samples already queued — not from
// this frame's own PTS. A decoder's output is contiguous by construction, but
// container timestamps are not sample-accurate: Matroska's timebase is a
// millisecond, while a 512-sample AAC frame is 10.667 ms, so every PTS is
// rounded by up to half a millisecond — 24 samples. Believing them makes
// consecutive chunks overlap or leave a hole, and the feed then fills those
// holes with silence: measured at 2.8% of every sample sent, which is
// continuous crackle rather than anything subtle. So PTS is only ever used by
// the caller to anchor a discontinuity; between those, the sample count is
// the truth.
void Decoder::resample_and_queue(const AVFrame *frame)
{
    if (!init_resampler(frame))
        return;

    // swr may hold samples back or emit buffered ones, so ask it how many are
    // actually coming rather than assuming the input count.
    int64_t delay = swr_get_delay(swr_, frame->sample_rate);
    int out_max = (int)av_rescale_rnd(delay + frame->nb_samples,
                                      DLK_AUDIO_RATE, frame->sample_rate,
                                      AV_ROUND_UP);
    if (out_max <= 0)
        return;

    AudioChunk chunk;
    chunk.samples.resize((size_t)out_max * out_channels_);
    uint8_t *dst = (uint8_t *)chunk.samples.data();
    int produced = swr_convert(swr_, &dst, out_max,
                               (const uint8_t **)frame->extended_data,
                               frame->nb_samples);
    if (produced <= 0)
        return;

    chunk.nframes = produced;
    chunk.samples.resize((size_t)produced * out_channels_);
    chunk.time = audio_next_time_;
    audio_next_time_ += (double)produced / DLK_AUDIO_RATE;
    audio_queue_.push_back(std::move(chunk));
}

// Pulls everything the audio decoder has ready, resamples it, and parks it on
// the queue.  Called from inside the demux loop, so it must never block.
void Decoder::drain_audio_frames()
{
    if (!audio_ctx_)
        return;

    AVStream *st = fmt_->streams[audio_index_];
    const bool tempo_active = std::fabs(audio_speed_ - 1.0) > 1e-6;

    for (;;) {
        int rc = avcodec_receive_frame(audio_ctx_, audio_frame_);
        if (rc == AVERROR_EOF) {
            // The codec will never produce another frame until the next seek
            // reopens it — drain whatever the tempo filter is still holding
            // back rather than silently dropping the last fraction of a
            // second, the same reason youtube-decklink's own TimeStretcher
            // has an explicit flush() called once at end of stream.
            flush_tempo_filter();
            return;
        }
        if (rc != 0)
            return;   // EAGAIN or a real error — nothing more right now

        // Discontinuity check, kept strictly in source time: this frame's own
        // PTS against where the *previous* frame's duration said the next one
        // should start. Both sides are source-domain, so this stays correct
        // no matter what speed is doing — comparing against audio_next_time_
        // directly, as an earlier version of this code did, doesn't: that
        // accumulator lives in output time (see the AudioChunk comment in
        // decoder.h), which is source time divided by whatever speed was in
        // effect over its *entire history*, not just the current instant. The
        // moment speed changed, every subsequent frame's rescaled PTS would
        // land far from the accumulator's true position purely because of
        // that history mismatch — indistinguishable from a real jump — and
        // this would re-anchor on every single frame after a speed change,
        // each time discarding however much output-time the run had already
        // correctly accumulated. That surfaced as roughly half of all audio
        // silently replaced by silence at 2x: not a decode shortfall at all,
        // just this anchor firing continuously and resetting a queue that was
        // never actually behind.
        int64_t ts = audio_frame_->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = audio_frame_->pts;
        double pts = -1.0;
        if (ts != AV_NOPTS_VALUE) {
            int64_t rel = ts;
            if (st->start_time != AV_NOPTS_VALUE)
                rel -= st->start_time;
            pts = (double)rel * av_q2d(st->time_base);
        }
        bool discontinuity =
            audio_next_time_ < 0.0 ||
            (pts >= 0.0 && audio_source_next_ >= 0.0 &&
             std::fabs(pts - audio_source_next_) > kAudioJumpSeconds);
        if (discontinuity) {
            // A fresh start: source and output time coincide at the instant
            // of the jump itself, regardless of speed, since nothing has
            // accumulated yet on the other side of it.
            double src_pts = pts >= 0.0 ? pts : 0.0;
            audio_source_next_ = src_pts;
            audio_next_time_   = src_pts / audio_speed_;
        }
        if (audio_frame_->sample_rate > 0)
            audio_source_next_ += (double)audio_frame_->nb_samples / audio_frame_->sample_rate;

        if (!tempo_active) {
            resample_and_queue(audio_frame_);
            av_frame_unref(audio_frame_);
            continue;
        }

        if (!init_tempo_filter(audio_frame_)) {
            // Filter unusable for this format — better to leave a gap (which
            // the player fills with silence) than emit audio at the wrong
            // pace, which would be heard drifting off the picture.
            av_frame_unref(audio_frame_);
            continue;
        }

        // Fed as the timestamp atempo reasons about durations with — a
        // running input sample count, not container PTS, matching why the
        // queue's own timestamps distrust it too (see above).
        audio_frame_->pts       = tempo_pts_;
        audio_frame_->time_base = AVRational{1, audio_frame_->sample_rate};
        tempo_pts_ += audio_frame_->nb_samples;

        // On success this takes ownership and resets audio_frame_ itself; on
        // failure the frame is untouched and still ours to release.
        if (av_buffersrc_add_frame_flags(tempo_src_ctx_, audio_frame_, 0) < 0) {
            av_frame_unref(audio_frame_);
            continue;
        }

        for (;;) {
            int frc = av_buffersink_get_frame(tempo_sink_ctx_, tempo_frame_);
            if (frc < 0)
                break;   // EAGAIN — needs more input before it has output
            resample_and_queue(tempo_frame_);
            av_frame_unref(tempo_frame_);
        }
    }
}

void Decoder::set_speed(double speed)
{
    speed = std::max(0.1, speed);
    if (std::fabs(speed - audio_speed_) < 1e-6)
        return;
    audio_speed_ = speed;
    tempo_dirty_ = true;   // rebuilt lazily, on the next audio frame
}

void Decoder::teardown_tempo_filter()
{
    if (tempo_graph_)
        avfilter_graph_free(&tempo_graph_);   // frees every context it owns
    tempo_src_ctx_  = nullptr;
    tempo_sink_ctx_ = nullptr;
}

void Decoder::flush_tempo_filter()
{
    if (!tempo_graph_)
        return;
    (void)av_buffersrc_add_frame_flags(tempo_src_ctx_, nullptr, 0);   // signals EOF
    for (;;) {
        int frc = av_buffersink_get_frame(tempo_sink_ctx_, tempo_frame_);
        if (frc < 0)
            break;
        resample_and_queue(tempo_frame_);
        av_frame_unref(tempo_frame_);
    }
}

// Built lazily — only once tempo correction is actually needed, and rebuilt
// whenever the requested speed or the decoded format changes. Chains several
// atempo stages when the target ratio is outside what one stage accepts:
// modern FFmpeg widened that to 0.5–100, but portably assuming the older
// 0.5–2.0 cap costs nothing (a single-stage ratio just becomes a one-element
// chain) and keeps this correct on whatever FFmpeg the helper is actually
// built against.
bool Decoder::init_tempo_filter(const AVFrame *frame)
{
    bool format_changed =
        !tempo_graph_ ||
        frame->sample_rate != tempo_rate_ ||
        frame->format != tempo_format_ ||
        av_channel_layout_compare(&frame->ch_layout, &tempo_layout_) != 0;

    if (!tempo_dirty_ && !format_changed)
        return true;

    teardown_tempo_filter();
    tempo_dirty_ = false;

    std::vector<double> stages;
    double remaining = audio_speed_;
    for (int guard = 0; remaining > 2.0 && guard < 32; guard++) {
        stages.push_back(2.0);
        remaining /= 2.0;
    }
    for (int guard = 0; remaining < 0.5 && guard < 32; guard++) {
        stages.push_back(0.5);
        remaining /= 0.5;
    }
    stages.push_back(remaining);

    tempo_graph_ = avfilter_graph_alloc();
    if (!tempo_graph_)
        return false;

    const AVFilter *abuffer     = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    const AVFilter *atempo      = avfilter_get_by_name("atempo");
    if (!abuffer || !abuffersink || !atempo) {
        teardown_tempo_filter();
        return false;
    }

    tempo_src_ctx_ = avfilter_graph_alloc_filter(tempo_graph_, abuffer, "src");
    if (!tempo_src_ctx_) {
        teardown_tempo_filter();
        return false;
    }
    AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
    if (!params) {
        teardown_tempo_filter();
        return false;
    }
    params->format      = frame->format;
    params->sample_rate = frame->sample_rate;
    params->time_base   = AVRational{1, frame->sample_rate};
    av_channel_layout_copy(&params->ch_layout, &frame->ch_layout);
    int rc = av_buffersrc_parameters_set(tempo_src_ctx_, params);
    av_channel_layout_uninit(&params->ch_layout);
    av_free(params);
    if (rc < 0 || avfilter_init_str(tempo_src_ctx_, nullptr) < 0) {
        teardown_tempo_filter();
        return false;
    }

    if (avfilter_graph_create_filter(&tempo_sink_ctx_, abuffersink, "sink",
                                     nullptr, nullptr, tempo_graph_) < 0) {
        teardown_tempo_filter();
        return false;
    }

    AVFilterContext *last = tempo_src_ctx_;
    for (size_t i = 0; i < stages.size(); i++) {
        char opts[32];
        snprintf(opts, sizeof(opts), "tempo=%.6f", stages[i]);
        char name[16];
        snprintf(name, sizeof(name), "atempo%zu", i);
        AVFilterContext *stage = nullptr;
        if (avfilter_graph_create_filter(&stage, atempo, name, opts, nullptr,
                                         tempo_graph_) < 0 ||
            avfilter_link(last, 0, stage, 0) < 0) {
            teardown_tempo_filter();
            return false;
        }
        last = stage;
    }
    if (avfilter_link(last, 0, tempo_sink_ctx_, 0) < 0 ||
        avfilter_graph_config(tempo_graph_, nullptr) < 0) {
        teardown_tempo_filter();
        return false;
    }

    tempo_rate_   = frame->sample_rate;
    tempo_format_ = frame->format;
    av_channel_layout_uninit(&tempo_layout_);
    av_channel_layout_copy(&tempo_layout_, &frame->ch_layout);
    tempo_pts_ = 0;
    log_info("decoder: audio tempo %.3fx (%zu stage%s)", audio_speed_,
             stages.size(), stages.size() == 1 ? "" : "s");
    return true;
}

bool Decoder::take_audio(AudioChunk *out)
{
    if (audio_queue_.empty())
        return false;
    *out = std::move(audio_queue_.front());
    audio_queue_.pop_front();
    return true;
}

bool Decoder::open_video_stream(HwMode mode, std::string *err)
{
    AVStream *st = fmt_->streams[video_index_];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (err)
            *err = "no decoder for this codec";
        return false;
    }

    bool want_hw = (mode != HwMode::Software) && codec_supports_videotoolbox(codec);
    if (mode == HwMode::VideoToolbox && !want_hw) {
        if (err)
            *err = std::string("VideoToolbox has no hwaccel for ") + codec->name;
        return false;
    }

    // Try hardware first, then software.  A VideoToolbox session can fail at
    // creation time for profiles the hardware doesn't implement (HEVC 4:4:4,
    // 12-bit), which surfaces here rather than at codec-config time — so the
    // fallback has to be a full reopen rather than a flag.
    for (int attempt = 0; attempt < 2; attempt++) {
        bool use_hw = want_hw && attempt == 0;

        video_ctx_ = avcodec_alloc_context3(codec);
        if (!video_ctx_) {
            if (err)
                *err = "out of memory";
            return false;
        }
        if (avcodec_parameters_to_context(video_ctx_, st->codecpar) < 0) {
            if (err)
                *err = "could not copy codec parameters";
            avcodec_free_context(&video_ctx_);
            return false;
        }
        video_ctx_->pkt_timebase = st->time_base;

        // Frame + slice threading for the software path.  Harmless when the
        // hwaccel takes over, since VT does its own scheduling.
        video_ctx_->thread_count = 0;  // 0 = pick based on CPU count

        if (use_hw && !init_hardware(codec, err)) {
            avcodec_free_context(&video_ctx_);
            if (mode == HwMode::VideoToolbox)
                return false;
            continue;  // retry in software
        }

        int rc = avcodec_open2(video_ctx_, codec, nullptr);
        if (rc < 0) {
            char buf[256];
            av_strerror(rc, buf, sizeof(buf));
            avcodec_free_context(&video_ctx_);
            if (hw_device_)
                av_buffer_unref(&hw_device_);
            if (use_hw && mode == HwMode::Auto) {
                log_info("decoder: hardware decode unavailable (%s) — "
                         "falling back to software", buf);
                continue;
            }
            if (err)
                *err = std::string("could not open decoder: ") + buf;
            return false;
        }

        info_.hardware = use_hw;
        return true;
    }

    if (err)
        *err = "could not open decoder";
    return false;
}

bool Decoder::init_hardware(const AVCodec *codec, std::string *err)
{
    (void)codec;
    int rc = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                    nullptr, nullptr, 0);
    if (rc < 0) {
        char buf[256];
        av_strerror(rc, buf, sizeof(buf));
        if (err)
            *err = std::string("could not create VideoToolbox device: ") + buf;
        return false;
    }
    video_ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
    video_ctx_->get_format    = get_hw_format;
    return true;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
int Decoder::next_frame(AVFrame **out)
{
    *out = nullptr;
    if (!video_ctx_)
        return -1;

    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return -1;

    for (;;) {
        int rc = avcodec_receive_frame(video_ctx_, frame);
        if (rc == 0) {
            *out = frame;
            return 1;
        }
        if (rc == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        }
        if (rc != AVERROR(EAGAIN)) {
            char buf[256];
            av_strerror(rc, buf, sizeof(buf));
            log_error("decoder: receive_frame failed: %s", buf);
            av_frame_free(&frame);
            return -1;
        }

        // Decoder wants more input.
        if (eof_sent_) {
            av_frame_free(&frame);
            return 0;
        }

        rc = av_read_frame(fmt_, packet_);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(video_ctx_, nullptr);  // enter drain mode
            if (audio_ctx_) {
                avcodec_send_packet(audio_ctx_, nullptr);
                drain_audio_frames();
            }
            eof_sent_ = true;
            continue;
        }
        if (rc < 0) {
            char buf[256];
            av_strerror(rc, buf, sizeof(buf));
            log_error("decoder: read_frame failed: %s", buf);
            av_frame_free(&frame);
            return -1;
        }

        if (packet_->stream_index == video_index_) {
            rc = avcodec_send_packet(video_ctx_, packet_);
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                char buf[256];
                av_strerror(rc, buf, sizeof(buf));
                log_error("decoder: send_packet failed: %s", buf);
            }
        } else if (audio_ctx_ && packet_->stream_index == audio_index_) {
            // Audio rides the same demux loop.  A failed send is not worth
            // failing the feed over — the player fills the gap with silence.
            if (avcodec_send_packet(audio_ctx_, packet_) >= 0)
                drain_audio_frames();
        }
        av_packet_unref(packet_);
    }
}

bool Decoder::seek(double seconds)
{
    if (!fmt_ || video_index_ < 0)
        return false;

    AVStream *st = fmt_->streams[video_index_];
    int64_t ts = (int64_t)llround(seconds / av_q2d(st->time_base));
    if (st->start_time != AV_NOPTS_VALUE)
        ts += st->start_time;

    int rc = av_seek_frame(fmt_, video_index_, ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        char buf[256];
        av_strerror(rc, buf, sizeof(buf));
        log_error("decoder: seek to %.3fs failed: %s", seconds, buf);
        return false;
    }

    avcodec_flush_buffers(video_ctx_);
    if (audio_ctx_) {
        avcodec_flush_buffers(audio_ctx_);
        // Samples already queued belong to where we were, not where we are
        // going, and the resampler is holding a tail of them.
        audio_queue_.clear();
        audio_next_time_   = -1.0;   // re-anchor to the first PTS after the seek
        audio_source_next_ = -1.0;
        if (swr_)
            swr_convert(swr_, nullptr, 0, nullptr, 0);
        // The tempo filter's WSOLA-like windowing has its own internal state
        // built from the audio stream up to this point; reused across a seek
        // it would splice pre-seek and post-seek material together as if
        // they were continuous. Torn down and rebuilt fresh on the next
        // frame, matching youtube-decklink's own TimeStretcher, which is
        // documented as not safe to reuse across a seek for exactly this
        // reason.
        tempo_dirty_ = true;
    }
    eof_sent_ = false;
    return true;
}

double Decoder::frame_time(const AVFrame *frame) const
{
    if (!frame || !fmt_ || video_index_ < 0)
        return 0.0;

    int64_t ts = frame->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE)
        ts = frame->pts;
    if (ts == AV_NOPTS_VALUE)
        return 0.0;

    AVStream *st = fmt_->streams[video_index_];
    if (st->start_time != AV_NOPTS_VALUE)
        ts -= st->start_time;
    return (double)ts * av_q2d(st->time_base);
}
