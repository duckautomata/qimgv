#include "videozoommodel.h"
#include <QStringList>
#include <algorithm>
#include <cmath>

void VideoZoomModel::setSettings(VideoZoomSettings const &settings) {
    mSettings = settings;
}

void VideoZoomModel::setViewport(QSize devicePixels) {
    mViewport = devicePixels;
}

void VideoZoomModel::setVideoSize(QSize displaySize) {
    mVideoSize = displaySize;
}

void VideoZoomModel::reset() {
    mFitted = true;
    mScale = 1;
    mAlign = QPointF();
}

bool VideoZoomModel::zoomTo(double target, QPointF anchor) {
    if(!hasVideo())
        return false;
    const double current = scale();
    const QPointF currentAlign = align();
    const double newScale = std::clamp(target, minScale(), kMaxScale);
    // Keeps the point of the video under the anchor where it is, as far as the edges allow.
    auto axis = [&](double view, double length, double oldAlign, double anchorPos) {
        const double oldEdge = edge(view, length * current, oldAlign);
        const double fraction = (anchorPos - oldEdge) / (length * current);
        return alignForEdge(view, length * newScale, anchorPos - fraction * length * newScale);
    };
    mAlign = QPointF(axis(mViewport.width(), mVideoSize.width(), currentAlign.x(), anchor.x()),
                     axis(mViewport.height(), mVideoSize.height(), currentAlign.y(), anchor.y()));
    mScale = newScale;
    // Zooming back to exactly the fit (a zoom level, or the minimum when that is the fit) returns to it,
    // so it follows the window again.
    mFitted = newScale == fitScale();
    // A request that was clamped still reports, so the indicator shows why nothing moved.
    return target != current;
}

bool VideoZoomModel::zoomStep(bool in, QPointF anchor) {
    const double s = scale();
    const double target =
        in ? stepIn(s, mSettings.zoomStep, mSettings.zoomLevels) : stepOut(s, mSettings.zoomStep, mSettings.zoomLevels);
    return zoomTo(target, anchor);
}

void VideoZoomModel::panBy(QPointF delta) {
    if(!overflows())
        return;
    const double s = scale();
    const QPointF a = align();
    const double w = mVideoSize.width() * s, h = mVideoSize.height() * s;
    mAlign = QPointF(alignForEdge(mViewport.width(), w, edge(mViewport.width(), w, a.x()) + delta.x()),
                     alignForEdge(mViewport.height(), h, edge(mViewport.height(), h, a.y()) + delta.y()));
}

bool VideoZoomModel::hasVideo() const {
    return !mVideoSize.isEmpty() && !mViewport.isEmpty();
}

bool VideoZoomModel::isFitted() const {
    return mFitted;
}

bool VideoZoomModel::overflows() const {
    if(!hasVideo())
        return false;
    const double s = scale();
    return mVideoSize.width() * s > mViewport.width() + 0.5 || mVideoSize.height() * s > mViewport.height() + 0.5;
}

double VideoZoomModel::scale() const {
    if(!hasVideo())
        return 1;
    return mFitted ? fitScale() : mScale;
}

double VideoZoomModel::minScale() const {
    if(!hasVideo())
        return 1;
    if(mSettings.unlockMinZoom)
        return std::max(10.0 / mVideoSize.width(), 10.0 / mVideoSize.height());
    return videoFits() ? 1.0
                       : std::min(double(mViewport.width()) / mVideoSize.width(),
                                  double(mViewport.height()) / mVideoSize.height());
}

QSize VideoZoomModel::viewport() const {
    return mViewport;
}

QRectF VideoZoomModel::videoRect() const {
    if(!hasVideo())
        return QRectF();
    const double s = scale();
    const QPointF a = align();
    const double w = mVideoSize.width() * s, h = mVideoSize.height() * s;
    return QRectF(edge(mViewport.width(), w, a.x()), edge(mViewport.height(), h, a.y()), w, h);
}

VideoZoomPlacement VideoZoomModel::placement() const {
    if(!hasVideo() || mFitted)
        return {mSettings.expandImage ? VideoPlayer::PLACEMENT_FIT_GROW : VideoPlayer::PLACEMENT_FIT_SHRINK, 1, 0, 0};
    // Centred on an axis the video fits, as edge() assumes. The player only does that by itself from
    // libmpv 0.40 (video-recenter); before, it would pin the video to the side the align points at.
    auto fits = [](double view, double size) { return size <= view + 0.5; };
    const double alignX = fits(mViewport.width(), mVideoSize.width() * mScale) ? 0 : mAlign.x();
    const double alignY = fits(mViewport.height(), mVideoSize.height() * mScale) ? 0 : mAlign.y();
    return {VideoPlayer::PLACEMENT_SCALED, mScale, alignX, alignY};
}

VideoZoomSettings const &VideoZoomModel::settings() const {
    return mSettings;
}

// Left/top edge of a video of `size` in a `view`, as the player positions it: centred when it fits,
// otherwise align -1 puts its left edge at 0 and 1 its right edge at the view's. The half-pixel slack
// matches a scale that fits to within rounding.
double VideoZoomModel::edge(double view, double size, double align) {
    if(size <= view + 0.5)
        return (view - size) / 2;
    return (view - size) * (align + 1) / 2;
}

// Inverse of edge(); the clamp is what stops a pan at the video's edges.
double VideoZoomModel::alignForEdge(double view, double size, double edge) {
    if(size <= view + 0.5)
        return 0;
    return std::clamp(2 * edge / (view - size) - 1, -1.0, 1.0);
}

// Mirrors ImageViewerV2::doZoomIn / doZoomOut, fixed zoom levels included.
double VideoZoomModel::stepIn(double scale, double step, QList<double> const &levels) {
    if(levels.isEmpty() || scale >= levels.last())
        return scale * (1 + step);
    if(scale < levels.first())
        return std::min(scale * (1 + step), levels.first());
    for(double level : levels)
        if(level > scale)
            return level;
    return scale * (1 + step);
}

double VideoZoomModel::stepOut(double scale, double step, QList<double> const &levels) {
    if(levels.isEmpty() || scale <= levels.first())
        return scale * (1 - step);
    if(scale > levels.last())
        return std::max(levels.last(), scale * (1 - step));
    for(auto level = levels.crbegin(); level != levels.crend(); ++level)
        if(*level < scale)
            return *level;
    return scale * (1 - step);
}

QList<double> VideoZoomModel::parseZoomLevels(QString const &csv) {
    QList<double> levels;
    for(QString const &entry : csv.split(',')) {
        bool ok = false;
        const double level = entry.trimmed().toDouble(&ok);
        if(ok && level > 0)
            levels.append(level);
    }
    std::sort(levels.begin(), levels.end());
    return levels;
}

// ImageViewerV2::wheelEvent's heuristic: a mouse wheel moves in whole notches, a trackpad does not.
bool VideoZoomModel::isMouseWheel(QPoint angleDelta, Qt::ScrollPhase phase, qint64 msSinceTrackpad, bool detection,
                                  bool wayland) {
    if(!detection)
        return true;
    if(wayland)
        return phase == Qt::NoScrollPhase;
    const int y = angleDelta.y();
    return y != 0 && std::abs(y) >= 120 && y % 60 == 0 && msSinceTrackpad > 250;
}

QPointF VideoZoomModel::align() const {
    return mFitted ? QPointF() : mAlign;
}

bool VideoZoomModel::videoFits() const {
    return mVideoSize.width() <= mViewport.width() && mVideoSize.height() <= mViewport.height();
}

// What the player's own fit shows: never enlarged with expand off (mpv's video-unscaled=downscale-big),
// enlarged to the window with it on (video-unscaled=no), as videos always have been.
double VideoZoomModel::fitScale() const {
    if(!mSettings.expandImage && videoFits())
        return 1;
    return std::min(double(mViewport.width()) / mVideoSize.width(), double(mViewport.height()) / mVideoSize.height());
}
