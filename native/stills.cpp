#include "stills.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "converter.h"   // SourceView, FrameMapping, map_frame, sws_colorspace_for
#include "log.h"

namespace {

// Frees whatever it was given, in the order a partially-built encode chain
// needs — codec before format context, packet and frame last.  Every early
// return below just fills in what got as far as being allocated.
struct EncodeState {
    AVFormatContext *ofmt   = nullptr;
    AVCodecContext  *enc    = nullptr;
    AVPacket        *pkt    = nullptr;
    AVFrame         *rgb    = nullptr;
    SwsContext      *sws    = nullptr;
    bool             opened_io = false;

    ~EncodeState()
    {
        if (sws)
            sws_freeContext(sws);
        if (rgb)
            av_frame_free(&rgb);
        if (pkt)
            av_packet_free(&pkt);
        if (enc)
            avcodec_free_context(&enc);
        if (ofmt) {
            if (opened_io)
                avio_closep(&ofmt->pb);
            avformat_free_context(ofmt);
        }
    }
};

}  // namespace

bool save_still_tiff(const AVFrame *frame, const SourceInfo &src,
                     const std::string &path, std::string *err)
{
    SourceView view;
    FrameMapping keep;
    if (!map_frame(frame, &view, &keep, err))
        return false;
    if (view.width <= 0 || view.height <= 0) {
        if (err)
            *err = "frame has no dimensions";
        return false;
    }

    EncodeState st;

    // Format-only conversion at native resolution: same rule as the DeckLink
    // canvas — the source's own matrix coefficients on both sides, so a
    // YUV→RGB unpack applies no colour change, only the range moves from the
    // source's own (MPEG or JPEG) to full, matching a TIFF's usual convention.
    int src_range = view.range == AVCOL_RANGE_JPEG ? 1 : 0;
    st.sws = sws_getContext(view.width, view.height, view.format,
                            view.width, view.height, AV_PIX_FMT_RGB48LE,
                            SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!st.sws) {
        if (err)
            *err = "could not create still scaler";
        return false;
    }
    const int cs = sws_colorspace_for(src);
    if (sws_setColorspaceDetails(st.sws, sws_getCoefficients(cs), src_range,
                                 sws_getCoefficients(cs), /*dst full range=*/1,
                                 0, 1 << 16, 1 << 16) < 0) {
        log_debug("stills: scaler rejected explicit colorspace details");
    }

    st.rgb = av_frame_alloc();
    if (!st.rgb) {
        if (err)
            *err = "out of memory";
        return false;
    }
    st.rgb->format = AV_PIX_FMT_RGB48LE;
    st.rgb->width  = view.width;
    st.rgb->height = view.height;
    if (av_frame_get_buffer(st.rgb, 64) < 0) {
        if (err)
            *err = "could not allocate still buffer";
        return false;
    }

    if (sws_scale(st.sws, view.data, view.linesize, 0, view.height,
                 st.rgb->data, st.rgb->linesize) <= 0) {
        if (err)
            *err = "still scaling failed";
        return false;
    }

    if (avformat_alloc_output_context2(&st.ofmt, nullptr, "image2",
                                       path.c_str()) < 0 ||
        !st.ofmt) {
        if (err)
            *err = "could not create image writer";
        return false;
    }
    // Without this the image2 muxer assumes `path` is a sequence pattern
    // (frame%03d.tiff) rather than one literal filename, and complains on
    // stderr about it — writing to it anyway, but by an undocumented
    // fallback rather than the muxer's actual single-file mode.
    av_opt_set(st.ofmt->priv_data, "update", "1", 0);

    const AVCodec *codec = avcodec_find_encoder_by_name("tiff");
    if (!codec) {
        if (err)
            *err = "no TIFF encoder in this FFmpeg build";
        return false;
    }
    AVStream *stream = avformat_new_stream(st.ofmt, nullptr);
    if (!stream) {
        if (err)
            *err = "out of memory";
        return false;
    }

    st.enc = avcodec_alloc_context3(codec);
    if (!st.enc) {
        if (err)
            *err = "out of memory";
        return false;
    }
    st.enc->width     = view.width;
    st.enc->height    = view.height;
    st.enc->pix_fmt   = AV_PIX_FMT_RGB48LE;
    st.enc->time_base = AVRational{1, 1};
    if (avcodec_open2(st.enc, codec, nullptr) < 0) {
        if (err)
            *err = "could not open TIFF encoder";
        return false;
    }
    if (avcodec_parameters_from_context(stream->codecpar, st.enc) < 0) {
        if (err)
            *err = "could not set up the image stream";
        return false;
    }

    if (avio_open(&st.ofmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        if (err)
            *err = "could not open " + path + " for writing";
        return false;
    }
    st.opened_io = true;

    if (avformat_write_header(st.ofmt, nullptr) < 0) {
        if (err)
            *err = "could not write the image header";
        return false;
    }

    st.pkt = av_packet_alloc();
    if (!st.pkt) {
        if (err)
            *err = "out of memory";
        return false;
    }

    st.rgb->pts = 0;
    bool encode_failed = false;
    for (const AVFrame *in : {(const AVFrame *)st.rgb, (const AVFrame *)nullptr}) {
        if (avcodec_send_frame(st.enc, in) < 0 && in) {
            encode_failed = true;
            break;
        }
        for (;;) {
            int rc = avcodec_receive_packet(st.enc, st.pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
                break;
            if (rc < 0) {
                encode_failed = true;
                break;
            }
            st.pkt->stream_index = stream->index;
            av_interleaved_write_frame(st.ofmt, st.pkt);
            av_packet_unref(st.pkt);
        }
        if (encode_failed)
            break;
    }
    if (encode_failed) {
        if (err)
            *err = "TIFF encode failed";
        return false;
    }

    if (av_write_trailer(st.ofmt) < 0) {
        if (err)
            *err = "could not finish writing the image";
        return false;
    }

    log_info("stills: saved %s (%dx%d rgb48le)", path.c_str(), view.width, view.height);
    return true;
}
