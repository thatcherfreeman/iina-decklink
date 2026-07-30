/*
 * Decoded frame → DeckLink canvas.
 *
 * Two rules govern everything here:
 *
 *   1. Rescale and repack only.  swscale is told to use the source's own
 *      matrix coefficients for both input and output, so a YUV→YUV conversion
 *      performs no matrix change at all; primaries and transfer function are
 *      never touched.  HDR material therefore reaches the card carrying its
 *      original code values, which is the entire point of feeding a reference
 *      monitor from here rather than from the Mac's display pipeline.
 *
 *   2. Padding is real broadcast black, not zeros.  Zero-filling a YUV canvas
 *      puts chroma at 0 instead of the neutral midpoint and the bars come out
 *      green.
 *
 * VideoToolbox frames are read straight out of their CVPixelBuffer.  macOS has
 * unified memory, so the buffer is already CPU-addressable and there is no
 * reason to pay for av_hwframe_transfer_data's copy — roughly 25 MB per 4K
 * P010 frame, 1.5 GB/s at 60p.
 */

#ifndef IINA_DECKLINK_CONVERTER_H
#define IINA_DECKLINK_CONVERTER_H

#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "decoder.h"

enum class Framing {
    Fit,       // scale to fit the canvas, preserving aspect ratio, pad the rest
    OneToOne,  // no resampling at all: centre, cropping or padding as needed
};

// A read-only view of source pixels, which may point either into a decoded
// AVFrame or straight into a locked CVPixelBuffer. Shared with stills.cpp,
// which needs the same VideoToolbox handling Converter does — a still grab is
// just a second consumer of a decoded frame, and hardware-frame extraction is
// exactly the kind of thing that must not have two subtly different copies.
struct SourceView {
    const uint8_t *data[4]     = {};
    int            linesize[4] = {};
    AVPixelFormat  format      = AV_PIX_FMT_NONE;
    int            range       = AVCOL_RANGE_UNSPECIFIED;
    int            width = 0, height = 0;
};

// Holds whatever needs releasing after a frame has been read via map_frame().
struct FrameMapping {
    void    *locked      = nullptr;  // CVPixelBufferRef, opaque here to avoid
                                     // a CoreVideo include in a header shared
                                     // by non-Apple-framework translation units
    AVFrame *transferred = nullptr;

    ~FrameMapping();
};

// Points `view` at the frame's pixels. For VideoToolbox frames in a layout it
// recognises, locks the CVPixelBuffer and takes its plane pointers directly;
// for anything else, falls back to av_hwframe_transfer_data (a copy, but
// always correct). `keep` must outlive `view`.
bool map_frame(const AVFrame *frame, SourceView *view, FrameMapping *keep,
              std::string *err);

// swscale's SWS_CS_* values coincide with AVColorSpace for everything this
// helper cares about, so the source's own coefficients can be passed straight
// through. Unspecified colorspace falls back to the near-universal convention:
// 709 for HD and up, 601 below it.
int sws_colorspace_for(const SourceInfo &src);

class Converter {
public:
    Converter() = default;
    ~Converter();

    Converter(const Converter &) = delete;
    Converter &operator=(const Converter &) = delete;

    // out_w/out_h are the negotiated DeckLink mode dimensions; pixfmt is a
    // DLK_PIXFMT_*.  full_range selects Full vs Video (SMPTE legal) levels —
    // for RGB output swscale has no legal-range mode, so the shim applies that
    // via LUT during packing and this only affects the padding colour.
    bool configure(const SourceInfo &src, int out_w, int out_h, int pixfmt,
                   Framing framing, bool full_range, std::string *err);

    // Converts one decoded frame into the canvas.  Accepts both software
    // frames and VideoToolbox-backed ones.
    bool convert(const AVFrame *frame, std::string *err);

    // The canvas, ready to hand to the shim.  v210 output is planar
    // (yuv422p10le, for dlk_output_send_planes); everything else is packed
    // (for dlk_output_send_packed).
    bool is_planar() const;
    const uint8_t *plane(int i) const { return canvas_ ? canvas_->data[i] : nullptr; }
    int stride(int i) const { return canvas_ ? canvas_->linesize[i] : 0; }

    // Exposed so --dump can render the finished canvas for inspection without
    // a card attached.
    const AVFrame *canvas() const { return canvas_; }

    // Overwrites the whole canvas with broadcast black — the same fill
    // configure() uses for padding, just covering the picture area too. For
    // blanking the output on focus loss: real black, not a zeroed buffer,
    // which for YUV canvases would put chroma at 0 instead of the neutral
    // midpoint and come out green. A no-op if configure() hasn't run yet.
    void black_out() { if (canvas_) fill_black(); }

private:
    bool build_canvas(std::string *err);
    void fill_black();
    void blit_into_canvas(const AVFrame *scaled);

    // Geometry, computed once in configure().
    int  out_w_ = 0, out_h_ = 0;
    int  crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;  // source region taken
    int  dst_x_ = 0, dst_y_ = 0, dst_w_ = 0, dst_h_ = 0;      // where it lands

    int        pixfmt_ = 0;              // DLK_PIXFMT_*
    AVPixelFormat target_ = AV_PIX_FMT_NONE;
    bool       full_range_ = false;
    SourceInfo src_;

    SwsContext *sws_ = nullptr;
    AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;  // what sws_ was built for
    int         sws_src_range_ = -1;

    AVFrame *canvas_ = nullptr;  // out_w_ x out_h_, target_ format
    AVFrame *scaled_ = nullptr;  // dst_w_ x dst_h_, target_ format
    bool     direct_to_canvas_ = false;  // scaled region covers the whole canvas
};

#endif  // IINA_DECKLINK_CONVERTER_H
