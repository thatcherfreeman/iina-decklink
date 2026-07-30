/*
 * Still capture: write a decoded frame as a 16-bit RGB TIFF.
 *
 * Ported from youtube-decklink's stills.py. The frame is rescaled to rgb48le
 * (16 bits per channel) at its native resolution — 8-bit sources gain no
 * precision but lose none, 10/12-bit sources are preserved exactly. As
 * everywhere else in this codebase, only the source's own matrix coefficients
 * are applied to unpack YUV to RGB; primaries and transfer are never touched.
 * The TIFF carries full-range values.
 */

#ifndef IINA_DECKLINK_STILLS_H
#define IINA_DECKLINK_STILLS_H

#include <string>

extern "C" {
#include <libavutil/frame.h>
}

#include "decoder.h"

// `frame` is a native decoded frame (software or VideoToolbox-backed, exactly
// what Decoder::next_frame produces) — not a DeckLink canvas. `src` supplies
// the colour metadata to unpack it with.
bool save_still_tiff(const AVFrame *frame, const SourceInfo &src,
                     const std::string &path, std::string *err);

#endif  // IINA_DECKLINK_STILLS_H
