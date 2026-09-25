#pragma once

#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include "settings.h"    // ImageScrolling; never the settings object
#include "videoplayer.h" // VideoPlayer::Placement

// Copied out of Settings by VideoPlayerInitProxy, so the model and VideoZoom are testable without it.
struct VideoZoomSettings {
    // The fit every video opens in: enlarged to the window, or never above 100%.
    bool expandImage = false;
    double zoomStep = 0.2;
    QList<double> zoomLevels; // ascending, all > 0; empty unless fixed zoom levels are on
    bool unlockMinZoom = true;
    ImageScrolling scrolling = SCROLL_BY_TRACKPAD;
    double scrollingSpeed = 1.0;
    bool trackpadDetection = true;
    bool wayland = false;
};

struct VideoZoomPlacement {
    VideoPlayer::Placement mode = VideoPlayer::PLACEMENT_FIT_SHRINK;
    double scale = 1;
    double alignX = 0;
    double alignY = 0;
    bool operator==(VideoZoomPlacement const &) const = default;
};

// Zoom and pan of the video player. Every video opens fitted to the window, and a zoom or pan lasts
// until the next one: none of the image viewer's fit modes or locks apply. Device pixels throughout,
// and scale is device pixels per video pixel -- the unit ImageViewerV2 uses. The pan position is kept
// as the player's align, so the player keeps it centred or clamped against any window size.
class VideoZoomModel {
public:
    static constexpr double kMaxScale = 500; // ImageViewerV2::doZoom's bound

    void setSettings(VideoZoomSettings const &settings);
    void setViewport(QSize devicePixels);
    void setVideoSize(QSize displaySize);
    // Back to the fit a video opens in.
    void reset();
    // Both return true when the zoom level should be reported.
    bool zoomTo(double target, QPointF anchor);
    bool zoomStep(bool in, QPointF anchor);
    // Moves the content by delta.
    void panBy(QPointF delta);

    // Size known and viewport not empty.
    bool hasVideo() const;
    // Shown in the fit it opened in, i.e. not zoomed.
    bool isFitted() const;
    bool overflows() const;
    // Effective scale; 1.0 without a video.
    double scale() const;
    double minScale() const;
    QSize viewport() const;
    // Device pixels, unclipped.
    QRectF videoRect() const;
    VideoZoomPlacement placement() const;
    VideoZoomSettings const &settings() const;

    static double edge(double view, double size, double align);
    static double alignForEdge(double view, double size, double edge);
    static double stepIn(double scale, double step, QList<double> const &levels);
    static double stepOut(double scale, double step, QList<double> const &levels);
    static QList<double> parseZoomLevels(QString const &csv);
    static bool isMouseWheel(QPoint angleDelta, Qt::ScrollPhase phase, qint64 msSinceTrackpad, bool detection,
                             bool wayland);

private:
    // Effective: centred while the player fits by itself.
    QPointF align() const;
    bool videoFits() const;
    double fitScale() const;

    VideoZoomSettings mSettings;
    QSize mViewport, mVideoSize;
    bool mFitted = true;
    // Only meaningful when not fitted.
    double mScale = 1;
    QPointF mAlign;
};
