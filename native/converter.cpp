#include "converter.h"

#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <CoreVideo/CoreVideo.h>

#include "decklink_shim.h"
#include "log.h"

// FrameMapping's destructor lives here, out of line, so converter.h — which
// stills.cpp also includes — doesn't need to pull in CoreVideo just to
// declare the struct. `locked` is a CVPixelBufferRef in disguise.
FrameMapping::~FrameMapping()
{
    if (locked)
        CVPixelBufferUnlockBaseAddress((CVPixelBufferRef)locked,
                                       kCVPixelBufferLock_ReadOnly);
    if (transferred)
        av_frame_free(&transferred);
}

namespace {

// CoreVideo pixel format → (AVPixelFormat, range).
//
// The range half matters more than it looks: VideoToolbox has separate
// video-range and full-range variants of each layout, and mistaking one for
// the other shifts levels on the wire — invisible on a computer display,
// immediately obvious on a reference monitor.
static bool cv_format_to_av(OSType cv, AVPixelFormat *fmt, int *range)
{
    switch (cv) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:   // '420v'
        *fmt = AV_PIX_FMT_NV12;    *range = AVCOL_RANGE_MPEG; return true;
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:    // '420f'
        *fmt = AV_PIX_FMT_NV12;    *range = AVCOL_RANGE_JPEG; return true;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:  // 'x420'
        *fmt = AV_PIX_FMT_P010LE;  *range = AVCOL_RANGE_MPEG; return true;
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:   // 'xf20'
        *fmt = AV_PIX_FMT_P010LE;  *range = AVCOL_RANGE_JPEG; return true;
    case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:  // 'x422'
        *fmt = AV_PIX_FMT_P210LE;  *range = AVCOL_RANGE_MPEG; return true;
    case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:   // 'xf22'
        *fmt = AV_PIX_FMT_P210LE;  *range = AVCOL_RANGE_JPEG; return true;
    case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:  // 'x444'
        *fmt = AV_PIX_FMT_P410LE;  *range = AVCOL_RANGE_MPEG; return true;
    case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:   // 'xf44'
        *fmt = AV_PIX_FMT_P410LE;  *range = AVCOL_RANGE_JPEG; return true;
    case kCVPixelFormatType_422YpCbCr8:                     // '2vuy'
        *fmt = AV_PIX_FMT_UYVY422; *range = AVCOL_RANGE_MPEG; return true;
    case kCVPixelFormatType_32BGRA:                         // 'BGRA'
        *fmt = AV_PIX_FMT_BGRA;    *range = AVCOL_RANGE_JPEG; return true;
    default:
        return false;
    }
}

}  // namespace

// Points `view` at the frame's pixels.  For VideoToolbox frames in a layout we
// recognise this locks the CVPixelBuffer and takes its plane pointers
// directly; for anything else it falls back to av_hwframe_transfer_data, which
// costs a copy but is always correct.
bool map_frame(const AVFrame *frame, SourceView *view, FrameMapping *keep,
               std::string *err)
{
    if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
        for (int i = 0; i < 4; i++) {
            view->data[i]     = frame->data[i];
            view->linesize[i] = frame->linesize[i];
        }
        view->format = (AVPixelFormat)frame->format;
        view->range  = frame->color_range;
        view->width  = frame->width;
        view->height = frame->height;
        return true;
    }

    CVPixelBufferRef pb = (CVPixelBufferRef)frame->data[3];
    if (!pb) {
        if (err)
            *err = "VideoToolbox frame has no pixel buffer";
        return false;
    }

    AVPixelFormat fmt;
    int range;
    if (cv_format_to_av(CVPixelBufferGetPixelFormatType(pb), &fmt, &range)) {
        if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            if (err)
                *err = "could not lock CVPixelBuffer";
            return false;
        }
        keep->locked = pb;

        if (CVPixelBufferIsPlanar(pb)) {
            size_t planes = CVPixelBufferGetPlaneCount(pb);
            for (size_t i = 0; i < planes && i < 4; i++) {
                view->data[i] =
                    (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, i);
                view->linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, i);
            }
        } else {
            view->data[0]     = (const uint8_t *)CVPixelBufferGetBaseAddress(pb);
            view->linesize[0] = (int)CVPixelBufferGetBytesPerRow(pb);
        }
        view->format = fmt;
        view->range  = range;
        view->width  = (int)CVPixelBufferGetWidth(pb);
        view->height = (int)CVPixelBufferGetHeight(pb);
        return true;
    }

    // Unrecognised layout — correctness over speed.
    log_debug("converter: unmapped CVPixelBuffer format, using hwframe transfer");
    AVFrame *sw = av_frame_alloc();
    if (!sw) {
        if (err)
            *err = "out of memory";
        return false;
    }
    if (av_hwframe_transfer_data(sw, frame, 0) < 0) {
        av_frame_free(&sw);
        if (err)
            *err = "av_hwframe_transfer_data failed";
        return false;
    }
    keep->transferred = sw;
    for (int i = 0; i < 4; i++) {
        view->data[i]     = sw->data[i];
        view->linesize[i] = sw->linesize[i];
    }
    view->format = (AVPixelFormat)sw->format;
    view->range  = frame->color_range;
    view->width  = sw->width;
    view->height = sw->height;
    return true;
}

// Shifts a view's plane pointers to start at (x, y).  av_image_get_linesize
// gives the byte width of `x` pixels for a given plane, which handles packed,
// planar and subsampled layouts uniformly; the vertical shift needs the
// plane's own chroma subsampling.
static void offset_view(SourceView *view, int x, int y)
{
    if (x == 0 && y == 0)
        return;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(view->format);
    if (!desc)
        return;
    int planes = av_pix_fmt_count_planes(view->format);

    for (int p = 0; p < planes && p < 4; p++) {
        if (!view->data[p])
            continue;
        // Planes 1 and 2 carry chroma in every layout we accept here.
        int sub_y = (p == 1 || p == 2) ? desc->log2_chroma_h : 0;
        int x_bytes = av_image_get_linesize(view->format, x, p);
        if (x_bytes < 0)
            x_bytes = 0;
        view->data[p] += (ptrdiff_t)(y >> sub_y) * view->linesize[p] + x_bytes;
    }
    view->width  -= x;
    view->height -= y;
}

int sws_colorspace_for(const SourceInfo &src)
{
    if (src.colorspace != AVCOL_SPC_UNSPECIFIED)
        return (int)src.colorspace;
    // Unspecified: the near-universal convention is 709 for HD and up, 601
    // below it.
    return src.height >= 720 ? (int)AVCOL_SPC_BT709 : (int)AVCOL_SPC_SMPTE170M;
}

namespace {

static AVPixelFormat target_pix_fmt(int dlk_pixfmt)
{
    switch (dlk_pixfmt) {
    case DLK_PIXFMT_UYVY:  return AV_PIX_FMT_UYVY422;
    case DLK_PIXFMT_V210:  return AV_PIX_FMT_YUV422P10LE;
    case DLK_PIXFMT_ARGB:  return AV_PIX_FMT_ARGB;
    case DLK_PIXFMT_RGB10: return AV_PIX_FMT_X2RGB10LE;
    default:               return AV_PIX_FMT_NONE;
    }
}

static bool target_is_rgb(int dlk_pixfmt)
{
    return dlk_pixfmt == DLK_PIXFMT_ARGB || dlk_pixfmt == DLK_PIXFMT_RGB10;
}

}  // namespace

// ---------------------------------------------------------------------------
Converter::~Converter()
{
    if (sws_)
        sws_freeContext(sws_);
    if (canvas_)
        av_frame_free(&canvas_);
    if (scaled_)
        av_frame_free(&scaled_);
}

bool Converter::configure(const SourceInfo &src, int out_w, int out_h, int pixfmt,
                          Framing framing, bool full_range, std::string *err)
{
    target_ = target_pix_fmt(pixfmt);
    if (target_ == AV_PIX_FMT_NONE) {
        if (err)
            *err = "unknown output pixel format";
        return false;
    }

    src_        = src;
    out_w_      = out_w;
    out_h_      = out_h;
    pixfmt_     = pixfmt;
    full_range_ = full_range;

    if (src.width <= 0 || src.height <= 0) {
        if (err)
            *err = "source has no dimensions";
        return false;
    }

    // Geometry.  Both framing modes reduce to "take this region of the source,
    // put it in this rectangle of the canvas" — for 1:1 the two rectangles are
    // the same size, so no resampling happens.
    if (framing == Framing::Fit) {
        crop_x_ = crop_y_ = 0;
        crop_w_ = src.width;
        crop_h_ = src.height;
        if ((long)src.width * out_h <= (long)src.height * out_w) {
            dst_h_ = out_h;
            dst_w_ = (int)((long)src.width * out_h / src.height) & ~1;
        } else {
            dst_w_ = out_w;
            dst_h_ = (int)((long)src.height * out_w / src.width) & ~1;
        }
    } else {
        crop_w_ = dst_w_ = (src.width  < out_w ? src.width  : out_w) & ~1;
        crop_h_ = dst_h_ = (src.height < out_h ? src.height : out_h) & ~1;
        crop_x_ = ((src.width  - crop_w_) / 2) & ~1;
        crop_y_ = ((src.height - crop_h_) / 2) & ~1;
    }
    dst_x_ = ((out_w - dst_w_) / 2) & ~1;
    dst_y_ = ((out_h - dst_h_) / 2) & ~1;

    direct_to_canvas_ = (dst_w_ == out_w && dst_h_ == out_h &&
                         dst_x_ == 0 && dst_y_ == 0);

    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    sws_src_fmt_   = AV_PIX_FMT_NONE;
    sws_src_range_ = -1;

    if (!build_canvas(err))
        return false;

    log_info("converter: %dx%d source → %dx%d canvas, picture at %dx%d+%d+%d "
             "(%s, %s levels)",
             src.width, src.height, out_w, out_h, dst_w_, dst_h_, dst_x_, dst_y_,
             framing == Framing::Fit ? "fit" : "1:1",
             full_range ? "full" : "video");
    return true;
}

bool Converter::is_planar() const
{
    return pixfmt_ == DLK_PIXFMT_V210;
}

bool Converter::build_canvas(std::string *err)
{
    if (canvas_)
        av_frame_free(&canvas_);
    if (scaled_)
        av_frame_free(&scaled_);

    canvas_ = av_frame_alloc();
    if (!canvas_) {
        if (err)
            *err = "out of memory";
        return false;
    }
    canvas_->format = target_;
    canvas_->width  = out_w_;
    canvas_->height = out_h_;
    if (av_frame_get_buffer(canvas_, 64) < 0) {
        if (err)
            *err = "could not allocate output canvas";
        return false;
    }
    fill_black();

    // When the picture covers the whole canvas, swscale writes straight into
    // it and no intermediate is needed.
    if (!direct_to_canvas_) {
        scaled_ = av_frame_alloc();
        if (!scaled_) {
            if (err)
                *err = "out of memory";
            return false;
        }
        scaled_->format = target_;
        scaled_->width  = dst_w_;
        scaled_->height = dst_h_;
        if (av_frame_get_buffer(scaled_, 64) < 0) {
            if (err)
                *err = "could not allocate scaling buffer";
            return false;
        }
    }
    return true;
}

// Broadcast black for the canvas.  Only the padding bars are ever seen, and
// only when the picture doesn't cover the canvas, so this runs once.
void Converter::fill_black()
{
    switch (target_) {
    case AV_PIX_FMT_UYVY422: {
        // Bytes are U,Y,V,Y; as a little-endian u32 that reads back reversed.
        const uint32_t pattern = full_range_ ? 0x00800080u   // Y=0,   C=128
                                             : 0x10801080u;  // Y=16,  C=128
        for (int y = 0; y < out_h_; y++) {
            uint32_t *row = (uint32_t *)(canvas_->data[0] + (ptrdiff_t)y * canvas_->linesize[0]);
            for (int x = 0; x < out_w_ / 2; x++)
                row[x] = pattern;
        }
        break;
    }
    case AV_PIX_FMT_YUV422P10LE: {
        const uint16_t luma   = full_range_ ? 0 : 64;
        const uint16_t chroma = 512;
        for (int y = 0; y < out_h_; y++) {
            uint16_t *row = (uint16_t *)(canvas_->data[0] + (ptrdiff_t)y * canvas_->linesize[0]);
            for (int x = 0; x < out_w_; x++)
                row[x] = luma;
        }
        for (int p = 1; p <= 2; p++) {
            for (int y = 0; y < out_h_; y++) {
                uint16_t *row =
                    (uint16_t *)(canvas_->data[p] + (ptrdiff_t)y * canvas_->linesize[p]);
                for (int x = 0; x < out_w_ / 2; x++)
                    row[x] = chroma;
            }
        }
        break;
    }
    case AV_PIX_FMT_ARGB: {
        // R=G=B=0, opaque.  When Video levels are selected the shim's LUT maps
        // 0 to 16 while packing, so the bars match the picture's black.
        for (int y = 0; y < out_h_; y++) {
            uint8_t *row = canvas_->data[0] + (ptrdiff_t)y * canvas_->linesize[0];
            for (int x = 0; x < out_w_; x++) {
                row[x * 4 + 0] = 0xFF;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
                row[x * 4 + 3] = 0;
            }
        }
        break;
    }
    default:
        // X2RGB10LE: all-zero is black, and the shim applies legal scaling.
        for (int p = 0; p < 4 && canvas_->data[p]; p++)
            memset(canvas_->data[p], 0,
                   (size_t)canvas_->linesize[p] * (size_t)out_h_);
        break;
    }
}

bool Converter::convert(const AVFrame *frame, std::string *err)
{
    if (!canvas_) {
        if (err)
            *err = "converter not configured";
        return false;
    }

    SourceView view;
    FrameMapping keep;
    if (!map_frame(frame, &view, &keep, err))
        return false;

    // Guard against a stream whose dimensions changed mid-playback; the caller
    // reconfigures on the next frame.
    if (view.width < crop_x_ + crop_w_ || view.height < crop_y_ + crop_h_) {
        if (err)
            *err = "frame smaller than the configured crop";
        return false;
    }

    int src_range = view.range == AVCOL_RANGE_JPEG ? 1 : 0;
    offset_view(&view, crop_x_, crop_y_);

    // Rebuild the scaler if the source layout changed — which it does exactly
    // once in practice, on the first frame.
    if (!sws_ || sws_src_fmt_ != view.format || sws_src_range_ != src_range) {
        if (sws_)
            sws_freeContext(sws_);
        sws_ = sws_getContext(crop_w_, crop_h_, view.format,
                              dst_w_, dst_h_, target_,
                              SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws_) {
            if (err)
                *err = "could not create scaler";
            return false;
        }
        sws_src_fmt_   = view.format;
        sws_src_range_ = src_range;

        // Both sides get the source's own coefficients, so a YUV→YUV
        // conversion applies no matrix at all.  Output range follows the
        // Video/Full setting for YUV; RGB output is written full-range here
        // and scaled to legal by the shim during packing, since swscale has no
        // legal-range RGB.
        const int cs = sws_colorspace_for(src_);
        int dst_range = target_is_rgb(pixfmt_) ? 1 : (full_range_ ? 1 : 0);
        if (sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), src_range,
                                     sws_getCoefficients(cs), dst_range,
                                     0, 1 << 16, 1 << 16) < 0) {
            log_debug("converter: scaler rejected explicit colorspace details");
        }
    }

    AVFrame *dst = direct_to_canvas_ ? canvas_ : scaled_;
    int rc = sws_scale(sws_, view.data, view.linesize, 0, crop_h_,
                       dst->data, dst->linesize);
    if (rc <= 0) {
        if (err)
            *err = "scaling failed";
        return false;
    }

    if (!direct_to_canvas_)
        blit_into_canvas(scaled_);
    return true;
}

// Copies the scaled picture into the canvas at (dst_x_, dst_y_), leaving the
// black bars untouched.
void Converter::blit_into_canvas(const AVFrame *scaled)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(target_);
    if (!desc)
        return;
    int planes = av_pix_fmt_count_planes(target_);

    for (int p = 0; p < planes && p < 4; p++) {
        if (!scaled->data[p] || !canvas_->data[p])
            continue;
        int sub_y = (p == 1 || p == 2) ? desc->log2_chroma_h : 0;
        int row_bytes = av_image_get_linesize(target_, dst_w_, p);
        int x_bytes   = av_image_get_linesize(target_, dst_x_, p);
        if (row_bytes < 0 || x_bytes < 0)
            continue;

        uint8_t *d = canvas_->data[p] +
                     (ptrdiff_t)(dst_y_ >> sub_y) * canvas_->linesize[p] + x_bytes;
        const uint8_t *s = scaled->data[p];
        int rows = dst_h_ >> sub_y;
        for (int y = 0; y < rows; y++) {
            memcpy(d, s, (size_t)row_bytes);
            d += canvas_->linesize[p];
            s += scaled->linesize[p];
        }
    }
}
