#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include "videozoommodel.h"

class QMouseEvent;
class QWheelEvent;
class VideoPlayer;

// Zoom and pan for the video player: routes actions and mouse input into VideoZoomModel and hands the
// result to the player. Lives on the app side because the plugin cannot read settings.
class VideoZoom : public QObject {
    Q_OBJECT
public:
    explicit VideoZoom(VideoPlayer *view, QObject *parent = nullptr);
    void setSettings(VideoZoomSettings const &settings);
    // Back to the fit, for a new file. Only once the player has covered the previous file's frame:
    // mpv would visibly re-lay out that frame at the new file's zoom.
    void onNewFile();
    // The video player is no longer the viewer on screen.
    void deactivate();
    void setInteractionEnabled(bool enabled);
    VideoZoomModel const &model() const;
    bool eventFilter(QObject *watched, QEvent *event) override;

public slots:
    void zoomIn();
    void zoomOut();
    void zoomInCursor();
    void zoomOutCursor();
    void scrollUp();
    void scrollDown();
    void scrollLeft();
    void scrollRight();
    void onVideoSizeChanged(QSize size);
    void onViewportResized(QSize devicePixels);

signals:
    void scaleChanged(qreal scale);

private:
    // The right-button gestures come last: release() tests for them with >=.
    enum Gesture {
        GESTURE_NONE,
        GESTURE_PAN_PENDING,
        GESTURE_PAN,
        GESTURE_RIGHT_PENDING,
        GESTURE_ZOOM,
        GESTURE_WHEEL_ZOOM
    };
    bool press(QMouseEvent *event, QPointF pos);
    bool move(QMouseEvent *event, QPointF pos);
    bool release(QMouseEvent *event);
    bool wheel(QWheelEvent *event, QPointF pos);
    void zoomAt(bool in, QPointF anchor);
    void panBy(QPointF delta);
    void apply();
    void reportScale(bool force);
    void endGesture();
    qreal dpr() const;
    // Model (device pixel) coordinates.
    QPointF viewCentre() const;
    QPointF cursorPos() const;

    VideoPlayer *mView;
    VideoZoomModel mModel;
    bool mActive = false;
    bool mInteraction = true;
    // The indicator stays off for a video nobody has zoomed.
    bool mScaleShown = false;
    qreal mLastReported = 0;
    Gesture mGesture = GESTURE_NONE;
    // Logical view coordinates.
    QPointF mPressPos, mLastPos;
    // Invalid until the first trackpad scroll, which counts as "long ago".
    QElapsedTimer mLastTrackpad;
};
