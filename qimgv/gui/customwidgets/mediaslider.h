#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <functional>

class QWheelEvent;

// A flat horizontal slider: a thin track, the part up to the value in the fill colour, and a knob that
// only shows under the mouse. The audio view uses it for the seek bar and the volume.
//
// Custom-painted rather than a styled QSlider so that a position update repaints only the pixels that
// moved, and so that it can never take keyboard focus away from ViewerWidget.
//
// User input never changes the value directly: it is reported through valueRequested and the owner
// decides. While the user drags, setValue() is ignored so the knob does not jump back to the player's
// position between two seeks.
class MediaSlider : public QWidget {
    Q_OBJECT
public:
    explicit MediaSlider(QWidget *parent = nullptr);

    // 0 means there is nothing to seek in: only the empty track is drawn and input is ignored.
    void setMaximum(double maximum);
    double maximum() const;
    void setValue(double value);
    double value() const;
    // What one wheel notch adds or subtracts.
    void setWheelStep(double step);
    // While dragging, valueRequested is sent at most once per `ms`; the value at release always is.
    void setDragInterval(int ms);
    void setColors(QColor const &track, QColor const &fill, QColor const &knob);
    // Draws the fill faded, e.g. the volume while muted.
    void setFillFaded(bool faded);
    // Text of the tooltip for the value under the mouse. No tooltip when unset.
    void setToolTipText(std::function<QString(double)> text);
    bool isDragging() const;
    // Steps the value for a wheel event that may have arrived elsewhere, such as beside the slider. False when
    // there is nothing to step, so that the event can go on to the viewer.
    bool takeWheel(QWheelEvent *event);

signals:
    void valueRequested(double value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    friend class MediaSliderAccessible;

    QRectF trackRect() const;
    qreal positionOf(double value) const;
    double valueAt(qreal x) const;
    double shownValue() const;
    bool acceptsInput() const;
    void setHovered(bool hovered);
    void updateBetween(qreal x1, qreal x2);
    void sendDragValue();
    void finishDrag();
    void cancelDrag();
    void showToolTip(QPoint pos);

    double mMaximum = 0, mValue = 0, mDragValue = 0, mLastSent = -1, mWheelStep = 1;
    int mDragInterval = 0, mWheelRemainder = 0;
    bool mHovered = false, mDragging = false, mFillFaded = false;
    QColor mTrackColor, mFillColor, mKnobColor;
    std::function<QString(double)> mToolTipText;
    QElapsedTimer mSinceSent;
    QTimer mDragTimer;
};
