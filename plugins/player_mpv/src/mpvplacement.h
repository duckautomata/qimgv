#pragma once

#include <QSize>
#include <algorithm>
#include <cmath>
#include "videoplayer.h"

// The mpv options behind one VideoPlayer::setPlacement(). Pure, so the ranges and the rotation rule
// are tested without libmpv. Relies on MpvWidget setting video-recenter=yes.
struct MpvPlacementOptions {
    enum Unscaled { UNSCALED_NO, UNSCALED_YES, UNSCALED_DOWNSCALE_BIG };
    Unscaled unscaled = UNSCALED_DOWNSCALE_BIG;
    double zoom = 0;
    double alignX = 0;
    double alignY = 0;
    bool operator==(MpvPlacementOptions const &) const = default;
};

inline const char *mpvUnscaledName(MpvPlacementOptions::Unscaled unscaled) {
    switch(unscaled) {
    case MpvPlacementOptions::UNSCALED_NO:
        return "no";
    case MpvPlacementOptions::UNSCALED_YES:
        return "yes";
    case MpvPlacementOptions::UNSCALED_DOWNSCALE_BIG:
        break;
    }
    return "downscale-big";
}

inline MpvPlacementOptions mpvOptionsFor(VideoPlayer::Placement mode, double scale, double alignX, double alignY) {
    // mpv rejects an out-of-range value instead of clamping it, and keeps the old one.
    auto align = [](double a) { return std::isfinite(a) ? std::clamp(a, -1.0, 1.0) : 0.0; };
    switch(mode) {
    case VideoPlayer::PLACEMENT_FIT_GROW:
        return {MpvPlacementOptions::UNSCALED_NO, 0, 0, 0};
    case VideoPlayer::PLACEMENT_SCALED:
        // mpv keeps video-zoom as a float and truncates the scaled size, so an exact fit (1000 px for
        // a 1920 px video at 1000/1920) can come out a pixel short and leave a line of background. The
        // nudge is far too small to show otherwise.
        if(std::isfinite(scale) && scale > 0)
            return {MpvPlacementOptions::UNSCALED_YES, std::clamp(std::log2(scale) + 1e-6, -20.0, 20.0), align(alignX),
                    align(alignY)};
        break;
    case VideoPlayer::PLACEMENT_FIT_SHRINK:
        break;
    }
    // Today's default, and what a nonsensical scale falls back to.
    return {};
}

// video-out-params dw/dh carry the aspect ratio and crop but not the rotation, while vo_libmpv rotates
// 90/270 degree video itself (VO_CAP_ROTATE90) and so shows it transposed.
inline QSize mpvDisplaySize(int dw, int dh, int rotate) {
    if(dw <= 0 || dh <= 0)
        return QSize();
    return rotate % 180 == 90 ? QSize(dh, dw) : QSize(dw, dh);
}
