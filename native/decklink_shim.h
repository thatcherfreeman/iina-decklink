/*
 * decklink_shim — DeckLink SDI output, pure-C interface.
 *
 * Ported from the author's youtube-decklink project, which in turn ports the
 * scheduled-playback design from the DeckLink output written for mpv
 * (vf_decklink_output.mm).  The implementation carries a QueryInterface
 * fallback ladder covering the 14.2.1, 15.3.1 and 16.0 SDK eras, so it keeps
 * working against older Desktop Video installs.
 *
 * Nothing here links against Blackmagic binaries at build time: on macOS the
 * SDK's DeckLinkAPIDispatch.cpp dlopens DeckLinkAPI.framework at runtime, so a
 * machine with no driver installed simply reports zero devices.
 */

#ifndef DECKLINK_SHIM_H
#define DECKLINK_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel formats.  These values are also used in the JSON control protocol and
 * in the plugin's saved settings, so they must stay stable. */
#define DLK_PIXFMT_UYVY   1   /* 8-bit  YUV 4:2:2 packed (bmdFormat8BitYUV)  */
#define DLK_PIXFMT_V210   2   /* 10-bit YUV 4:2:2 packed (bmdFormat10BitYUV) */
#define DLK_PIXFMT_ARGB   3   /* 8-bit  RGB 4:4:4 packed (bmdFormat8BitARGB) */
#define DLK_PIXFMT_RGB10  4   /* 10-bit RGB 4:4:4 packed (bmdFormat10BitRGB) */

/* SDI link configuration.  Higher link counts split the signal across more
 * connectors to raise available bandwidth; some high-bandwidth modes only
 * appear in the device's mode list once the link mode is set. */
#define DLK_LINK_SINGLE   0
#define DLK_LINK_DUAL     1
#define DLK_LINK_QUAD     2

#define DLK_AUDIO_RATE    48000

typedef struct dlk_output dlk_output;

/* level: 0 = error, 1 = info, 2 = debug. */
typedef void (*dlk_log_fn)(int level, const char *msg);
void dlk_set_log_callback(dlk_log_fn fn);

/* Frees strings returned by the enumeration calls below. */
void dlk_free_string(char *s);

/* Newline-separated device display names.  NULL if the Desktop Video driver
 * is missing entirely (as opposed to an empty string, which means the driver
 * is present but no devices are connected). */
char *dlk_enumerate_devices(void);

/* One mode per line: "code\twidth\theight\tfps\tscan", where scan is "p" or
 * "i" and code is the 4-character BMD display-mode code.  NULL on failure. */
char *dlk_enumerate_modes(const char *device_name);

/*
 * Opens a device and starts scheduled playback.
 *
 *   device_name   NULL/"" → first device found
 *   format_code   NULL/"" → auto-select; otherwise an explicit 4-char BMD code
 *   src_*         source geometry, used to drive mode auto-selection
 *   pixfmt        DLK_PIXFMT_*
 *   preroll       frames to buffer before playback starts (clamped to 1..16)
 *   rgb_legal     1 → scale RGB output to SMPTE legal levels
 *   fixed_*       >0 → lock the output resolution, auto-negotiate only fps
 *   link_mode     DLK_LINK_*
 *
 * Returns NULL on failure, having logged the reason.
 */
dlk_output *dlk_output_create(const char *device_name,
                              const char *format_code,
                              int src_width, int src_height, double src_fps,
                              int pixfmt,
                              int preroll,
                              int enable_audio, int audio_channels,
                              int rgb_legal,
                              int fixed_width, int fixed_height,
                              int link_mode);

void dlk_output_destroy(dlk_output *d);

/* Reports the mode actually negotiated.  Any out-param may be NULL; code must
 * point to at least 5 bytes.  The reported pixfmt may differ from the one
 * requested: 4:4:4 falls back to same-depth 4:2:2 when the link can't carry
 * it at this mode. */
void dlk_output_get_info(dlk_output *d, int *width, int *height, double *fps,
                         char *code, int *audio_on, int *pixfmt);

/* Backpressure: 0 when the in-flight queue is full and a send would be
 * dropped. */
int dlk_output_can_send(dlk_output *d);
int dlk_output_buffered_video_frames(dlk_output *d);

/*
 * Both send calls take a frame already sized exactly to the output mode, and
 * display it for `repeat` mode-frame periods (repeat > 1 is how a 24p source
 * holds across a 48p or 60p output).  Return 1 if scheduled, 0 if dropped.
 *
 *   send_packed   UYVY / ARGB / RGB10.  RGB10 input must be x2rgb10le; it is
 *                 byte-swapped to the big-endian wire format during packing.
 *   send_planes   yuv422p10le planes, packed to v210.  V210 output only.
 */
int dlk_output_send_packed(dlk_output *d, const uint8_t *data, int stride,
                           int repeat);
int dlk_output_send_planes(dlk_output *d,
                           const uint8_t *y_plane, int y_stride,
                           const uint8_t *u_plane, int u_stride,
                           const uint8_t *v_plane, int v_stride,
                           int repeat);

/* Interleaved S32 PCM at 48 kHz.  Returns sample frames accepted. */
int dlk_output_send_audio(dlk_output *d, const int32_t *interleaved,
                          int nframes);
int dlk_output_buffered_audio_frames(dlk_output *d);

/* Hardware playback position in seconds since playback started, or -1. */
double dlk_output_stream_time(dlk_output *d);

/* Re-anchors the schedule just ahead of the hardware clock.  Call after a
 * pause, seek, or underrun, otherwise frames scheduled in the past flush out
 * in a burst. */
void dlk_output_resync(dlk_output *d);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* DECKLINK_SHIM_H */
