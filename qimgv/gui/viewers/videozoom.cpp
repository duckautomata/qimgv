#include "videozoom.h"
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QWheelEvent>
#include <limits>
#include "videoplayer.h"

// ImageViewerV2's scroll step, in logical pixels.
static constexpr double kScrollDistance = 240;

VideoZoom::VideoZoom(VideoPlayer *view, QObject *parent) : QObject(parent), mView(view) {
    mLastTrackpad.invalidate();
}

void VideoZoom::setSettings(VideoZoomSettings const &settings) {
    mModel.setSettings(settings);
    apply();
    reportScale(false);
}

// Every video opens fitted to the window; a zoom or pan does not carry over. A video opened in the
// fit shows no indicator, as before.
void VideoZoom::onNewFile() {
    endGesture();
    mActive = true;
    mScaleShown = false;
    mModel.reset();
    // Forget the previous file's size: until the player sends this one's, zoom and pan are ignored
    // rather than worked out against the wrong video.
    mModel.setVideoSize(QSize());
    apply();
}

void VideoZoom::deactivate() {
    endGesture();
    mActive = false;
}

void VideoZoom::setInteractionEnabled(bool enabled) {
    mInteraction = enabled;
    if(!enabled)
        endGesture();
}

VideoZoomModel const &VideoZoom::model() const {
    return mModel;
}

void VideoZoom::zoomIn() {
    zoomAt(true, viewCentre());
}

void VideoZoom::zoomOut() {
    zoomAt(false, viewCentre());
}

// At the cursor only while it is over the video, as ImageViewerV2 does.
void VideoZoom::zoomInCursor() {
    zoomAt(true, mView->underMouse() ? cursorPos() : viewCentre());
}

void VideoZoom::zoomOutCursor() {
    zoomAt(false, mView->underMouse() ? cursorPos() : viewCentre());
}

// Scrolling up shows more of the top, so the content moves down.
void VideoZoom::scrollUp() {
    panBy(QPointF(0, kScrollDistance * dpr()));
}

void VideoZoom::scrollDown() {
    panBy(QPointF(0, -kScrollDistance * dpr()));
}

void VideoZoom::scrollLeft() {
    panBy(QPointF(kScrollDistance * dpr(), 0));
}

void VideoZoom::scrollRight() {
    panBy(QPointF(-kScrollDistance * dpr(), 0));
}

// apply() does nothing while inactive: sizes are only stored until the video is on screen.
void VideoZoom::onVideoSizeChanged(QSize size) {
    mModel.setVideoSize(size);
    apply();
    reportScale(false);
}

void VideoZoom::onViewportResized(QSize devicePixels) {
    mModel.setViewport(devicePixels);
    apply();
    reportScale(false);
}

bool VideoZoom::eventFilter(QObject *watched, QEvent *event) {
    const QEvent::Type type = event->type();
    const bool mouse = type == QEvent::MouseButtonPress || type == QEvent::MouseMove ||
                       type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick;
    if(!mouse && type != QEvent::Wheel)
        return false;
    if(!mActive || !mInteraction || !mModel.hasVideo()) {
        endGesture();
        return false;
    }
    // QWidget's meta-object is Qt's own, so this cast is safe on the plugin's widget.
    auto widget = qobject_cast<QWidget *>(watched);
    auto toView = [&](QPointF pos) { return widget && widget != mView ? widget->mapTo(mView, pos) : pos; };
    if(type == QEvent::Wheel) {
        auto wheelEvent = static_cast<QWheelEvent *>(event);
        return wheel(wheelEvent, toView(wheelEvent->position()));
    }
    auto mouseEvent = static_cast<QMouseEvent *>(event);
    switch(type) {
    case QEvent::MouseButtonPress:
        return press(mouseEvent, toView(mouseEvent->position()));
    case QEvent::MouseMove:
        return move(mouseEvent, toView(mouseEvent->position()));
    case QEvent::MouseButtonRelease:
        return release(mouseEvent);
    default:
        // A quick second press of any other button arrives only as a double-click.
        if(mouseEvent->button() != Qt::LeftButton)
            return press(mouseEvent, toView(mouseEvent->position()));
        // Left double-click: the main window toggles fullscreen, as before.
        endGesture();
        return false;
    }
}

bool VideoZoom::press(QMouseEvent *event, QPointF pos) {
    // With no other button held, a gesture still on record lost its release (to a popup or a modal
    // dialog, say) and is over.
    if(event->buttons() == event->button())
        endGesture();
    // A second button in the middle of a gesture goes on as it always has.
    if(mGesture != GESTURE_NONE)
        return false;
    if(event->button() == Qt::LeftButton && mModel.overflows()) {
        // Held back from the player's click-to-pause: a move makes it a pan, a release without one
        // pauses (see release()). A video that fits pauses on press, as before.
        mGesture = GESTURE_PAN_PENDING;
        mPressPos = mLastPos = pos;
        return true;
    }
    if(event->button() == Qt::RightButton) {
        mGesture = GESTURE_RIGHT_PENDING;
        mPressPos = mLastPos = pos;
    }
    // The right press goes on: ViewerWidget still hides an open context menu with it.
    return false;
}

bool VideoZoom::move(QMouseEvent *event, QPointF pos) {
    if(mGesture == GESTURE_PAN_PENDING) {
        if(!(event->buttons() & Qt::LeftButton)) {
            endGesture();
            return false;
        }
        // The same threshold decides "click" in release(), so a small drag never both pans and pauses.
        if((pos - mPressPos).manhattanLength() < QApplication::startDragDistance())
            return true;
        mGesture = GESTURE_PAN;
        mView->setCursor(Qt::ClosedHandCursor);
    }
    // The release went elsewhere; see press().
    if((mGesture == GESTURE_PAN && !(event->buttons() & Qt::LeftButton)) ||
       (mGesture >= GESTURE_RIGHT_PENDING && !(event->buttons() & Qt::RightButton))) {
        endGesture();
        return false;
    }
    if(mGesture == GESTURE_PAN) {
        // The first step includes the distance travelled before the threshold.
        panBy((pos - mLastPos) * dpr());
        mLastPos = pos;
        return true;
    }
    if(mGesture == GESTURE_RIGHT_PENDING) {
        // ImageViewerV2's jitter allowance before a right drag counts as zooming.
        if(qAbs(mPressPos.y() - pos.y()) <= int(dpr() * 4) / dpr())
            return false;
        mGesture = GESTURE_ZOOM;
        mView->setCursor(Qt::SizeVerCursor);
    }
    if(mGesture == GESTURE_ZOOM) {
        // ImageViewerV2's rate: dragging up zooms in, anchored where the drag started.
        const double target = mModel.scale() * (1 + 0.003 * (mLastPos.y() - pos.y()) * dpr());
        mLastPos = pos;
        const bool report = mModel.zoomTo(target, mPressPos * dpr());
        apply();
        if(report)
            reportScale(true);
        // Consumed, which also keeps the video controls from popping up mid-drag.
        return true;
    }
    return false;
}

bool VideoZoom::release(QMouseEvent *event) {
    if(event->button() == Qt::LeftButton && (mGesture == GESTURE_PAN_PENDING || mGesture == GESTURE_PAN)) {
        const bool click = mGesture == GESTURE_PAN_PENDING;
        endGesture();
        if(click)
            mView->pauseResume();
        // Passed on so ViewerWidget shows the cursor again; nothing is bound to a left release.
        return false;
    }
    if(event->button() == Qt::RightButton && mGesture >= GESTURE_RIGHT_PENDING) {
        const bool zoomed = mGesture != GESTURE_RIGHT_PENDING;
        endGesture();
        // Swallowing it keeps the "RMB" action -- the context menu -- from firing after a zoom.
        return zoomed;
    }
    // Nor in the middle of a pan, as on images: its popup would take the left release.
    if(event->button() == Qt::RightButton && (mGesture == GESTURE_PAN_PENDING || mGesture == GESTURE_PAN))
        return true;
    return false;
}

bool VideoZoom::wheel(QWheelEvent *event, QPointF pos) {
    const QPoint angle = event->angleDelta();
    // Right button + wheel zooms at the pointer, as on images.
    if(event->buttons() & Qt::RightButton) {
        // The right release that follows must not open the context menu, nor a left one pause. Only
        // while a gesture of ours is live, though: a press made elsewhere (on a panel, say) is
        // released there, not here.
        if(mGesture != GESTURE_NONE)
            mGesture = GESTURE_WHEEL_ZOOM;
        if(angle.y() != 0)
            zoomAt(angle.y() > 0, pos * dpr());
        return true;
    }
    // Ctrl+wheel reaches zoomInCursor() through the action manager.
    if(event->modifiers() != Qt::NoModifier)
        return false;
    VideoZoomSettings const &s = mModel.settings();
    const qint64 sinceTrackpad = mLastTrackpad.isValid() ? mLastTrackpad.elapsed() : std::numeric_limits<qint64>::max();
    if(!VideoZoomModel::isMouseWheel(angle, event->phase(), sinceTrackpad, s.trackpadDetection, s.wayland)) {
        mLastTrackpad.restart();
        // A video that fits keeps going to the next file, as it always has.
        if(!mModel.overflows())
            return false;
        if(s.scrolling != SCROLL_NONE) {
            const QPoint pixels = event->pixelDelta();
            const int dx = qAbs(angle.x()) > qAbs(pixels.x()) ? angle.x() : pixels.x();
            const int dy = qAbs(angle.y()) > qAbs(pixels.y()) ? angle.y() : pixels.y();
            // ImageViewerV2's trackpad multiplier.
            panBy(QPointF(dx, dy) * 0.7 * dpr());
        }
        return true;
    }
    if(s.scrolling == SCROLL_BY_TRACKPAD_AND_WHEEL && angle.y() != 0) {
        // Scroll through a tall video, then on to the next file at its edge -- as on images, with their
        // two pixels of slack for an edge that is not quite flush.
        const QRectF rect = mModel.videoRect();
        const double slack = 2 * dpr();
        const bool roomBelow = rect.bottom() > mModel.viewport().height() + slack;
        const bool roomAbove = rect.top() < -slack;
        if((angle.y() < 0 && roomBelow) || (angle.y() > 0 && roomAbove)) {
            panBy(QPointF(0, angle.y() * 2.0 * s.scrollingSpeed * dpr()));
            return true;
        }
    }
    return false;
}

void VideoZoom::zoomAt(bool in, QPointF anchor) {
    if(!mActive || !mModel.hasVideo())
        return;
    const bool report = mModel.zoomStep(in, anchor);
    apply();
    if(report)
        reportScale(true);
}

void VideoZoom::panBy(QPointF delta) {
    if(!mActive || !mModel.hasVideo())
        return;
    mModel.panBy(delta);
    apply();
}

void VideoZoom::apply() {
    if(!mActive)
        return;
    const VideoZoomPlacement p = mModel.placement();
    mView->setPlacement(p.mode, p.scale, p.alignX, p.alignY);
}

// The indicator never shows over an image, or for a player that is not on screen.
void VideoZoom::reportScale(bool force) {
    if(!mActive)
        return;
    const qreal scale = mModel.scale();
    if(!force && (!mScaleShown || scale == mLastReported))
        return;
    mScaleShown = true;
    mLastReported = scale;
    emit scaleChanged(scale);
}

void VideoZoom::endGesture() {
    if(mGesture == GESTURE_NONE)
        return;
    mGesture = GESTURE_NONE;
    mView->unsetCursor();
}

qreal VideoZoom::dpr() const {
    return mView->devicePixelRatioF();
}

QPointF VideoZoom::viewCentre() const {
    const QSize viewport = mModel.viewport();
    return QPointF(viewport.width() / 2.0, viewport.height() / 2.0);
}

QPointF VideoZoom::cursorPos() const {
    return mView->mapFromGlobal(QPointF(QCursor::pos())) * dpr();
}
