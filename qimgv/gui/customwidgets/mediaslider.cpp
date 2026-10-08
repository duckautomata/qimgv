#include "mediaslider.h"

#include <QAccessibleWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kTrackThickness = 4;
constexpr qreal kActiveTrackThickness = 6;
constexpr qreal kKnobRadius = 6;
} // namespace

#if QT_CONFIG(accessibility)
// A slider with a value that assistive technology can read and set, rather than a nameless client area.
class MediaSliderAccessible : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit MediaSliderAccessible(MediaSlider *slider) : QAccessibleWidget(slider, QAccessible::Slider) {}

    void *interface_cast(QAccessible::InterfaceType type) override {
        if(type == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface *>(this);
        return QAccessibleWidget::interface_cast(type);
    }

    QString text(QAccessible::Text t) const override {
        // The tooltip's wording: a time for the seek bar, a percentage for the volume.
        if(t == QAccessible::Value && slider()->mToolTipText && slider()->mMaximum > 0)
            return slider()->mToolTipText(slider()->shownValue());
        return QAccessibleWidget::text(t);
    }

    QVariant currentValue() const override { return slider()->shownValue(); }
    QVariant maximumValue() const override { return slider()->mMaximum; }
    QVariant minimumValue() const override { return 0.0; }
    QVariant minimumStepSize() const override { return slider()->mWheelStep; }

    void setCurrentValue(QVariant const &value) override {
        MediaSlider *s = slider();
        if(s->acceptsInput() && !s->mDragging)
            emit s->valueRequested(std::clamp(value.toDouble(), 0.0, s->mMaximum));
    }

private:
    MediaSlider *slider() const { return static_cast<MediaSlider *>(object()); }
};

namespace {
QAccessibleInterface *createAccessible(QString const &key, QObject *object) {
    if(key == QLatin1String("MediaSlider"))
        if(auto slider = qobject_cast<MediaSlider *>(object))
            return new MediaSliderAccessible(slider);
    return nullptr;
}
} // namespace
#endif

MediaSlider::MediaSlider(QWidget *parent) : QWidget(parent) {
#if QT_CONFIG(accessibility)
    static bool const factoryInstalled = [] {
        QAccessible::installFactory(createAccessible);
        return true;
    }();
    Q_UNUSED(factoryInstalled)
#endif
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    mDragTimer.setSingleShot(true);
    connect(&mDragTimer, &QTimer::timeout, this, &MediaSlider::sendDragValue);
}

void MediaSlider::setMaximum(double maximum) {
    maximum = std::max(0.0, maximum);
    if(maximum == mMaximum)
        return;
    mMaximum = maximum;
    if(!acceptsInput())
        cancelDrag();
    update();
}

double MediaSlider::maximum() const {
    return mMaximum;
}

void MediaSlider::setValue(double value) {
    if(mDragging)
        return;
    qreal oldX = positionOf(mValue);
    mValue = value;
    qreal newX = positionOf(mValue);
    // Positions arrive many times a second; most of them do not move the fill by a single device pixel.
    qreal dpr = devicePixelRatioF();
    if(std::lround(oldX * dpr) != std::lround(newX * dpr))
        updateBetween(oldX, newX);
}

double MediaSlider::value() const {
    return mValue;
}

void MediaSlider::setWheelStep(double step) {
    mWheelStep = step;
}

void MediaSlider::setDragInterval(int ms) {
    mDragInterval = std::max(0, ms);
}

void MediaSlider::setColors(QColor const &track, QColor const &fill, QColor const &knob) {
    mTrackColor = track;
    mFillColor = fill;
    mKnobColor = knob;
    update();
}

void MediaSlider::setFillFaded(bool faded) {
    if(faded == mFillFaded)
        return;
    mFillFaded = faded;
    update();
}

void MediaSlider::setToolTipText(std::function<QString(double)> text) {
    mToolTipText = std::move(text);
}

bool MediaSlider::isDragging() const {
    return mDragging;
}

bool MediaSlider::acceptsInput() const {
    return isEnabled() && mMaximum > 0;
}

QRectF MediaSlider::trackRect() const {
    // Inset by the knob so that it fits at both ends.
    qreal thickness = (mHovered || mDragging) && acceptsInput() ? kActiveTrackThickness : kTrackThickness;
    return QRectF(kKnobRadius, (height() - thickness) / 2.0, std::max(0.0, width() - 2 * kKnobRadius), thickness);
}

qreal MediaSlider::positionOf(double value) const {
    qreal span = std::max(0.0, width() - 2 * kKnobRadius);
    qreal fraction = mMaximum > 0 ? std::clamp(value / mMaximum, 0.0, 1.0) : 0.0;
    return kKnobRadius + fraction * span;
}

double MediaSlider::valueAt(qreal x) const {
    qreal span = width() - 2 * kKnobRadius;
    if(span <= 0)
        return 0;
    return std::clamp((x - kKnobRadius) / span, 0.0, 1.0) * mMaximum;
}

double MediaSlider::shownValue() const {
    return mDragging ? mDragValue : mValue;
}

void MediaSlider::updateBetween(qreal x1, qreal x2) {
    int left = static_cast<int>(std::floor(std::min(x1, x2) - kKnobRadius)) - 2;
    int right = static_cast<int>(std::ceil(std::max(x1, x2) + kKnobRadius)) + 2;
    update(QRect(left, 0, right - left, height()));
}

void MediaSlider::setHovered(bool hovered) {
    if(hovered == mHovered)
        return;
    mHovered = hovered;
    update();
}

void MediaSlider::sendDragValue() {
    mDragTimer.stop();
    if(mDragValue == mLastSent)
        return;
    mLastSent = mDragValue;
    mSinceSent.start();
    emit valueRequested(mDragValue);
}

// Ends a drag where it is, as a release there would.
void MediaSlider::finishDrag() {
    if(!mDragging)
        return;
    sendDragValue();
    mDragging = false;
    // Show where the user let go until the owner reports the outcome, rather than the stale value.
    mValue = mDragValue;
    update();
}

// Ends a drag without acting on it: there is nothing left to seek in.
void MediaSlider::cancelDrag() {
    if(!mDragging)
        return;
    mDragging = false;
    mDragTimer.stop();
    update();
}

void MediaSlider::showToolTip(QPoint globalPos) {
    if(!mToolTipText || mMaximum <= 0)
        return;
    double value = mDragging ? mDragValue : valueAt(mapFromGlobal(globalPos).x());
    QToolTip::showText(globalPos, mToolTipText(value), this);
}

void MediaSlider::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    if(!isEnabled())
        p.setOpacity(0.5);
    QRectF track = trackRect();
    qreal radius = track.height() / 2;
    p.setBrush(mTrackColor);
    p.drawRoundedRect(track, radius, radius);
    if(mMaximum <= 0)
        return;
    qreal x = positionOf(shownValue());
    if(x > track.left()) {
        // The track shape clipped at the value: a rounded start and a square end.
        p.save();
        p.setClipRect(QRectF(0, 0, x, height()));
        if(mFillFaded)
            p.setOpacity(p.opacity() * 0.4);
        p.setBrush(mFillColor);
        p.drawRoundedRect(track, radius, radius);
        p.restore();
    }
    if((mHovered || mDragging) && acceptsInput()) {
        p.setBrush(mKnobColor);
        p.drawEllipse(QPointF(x, height() / 2.0), kKnobRadius, kKnobRadius);
    }
}

void MediaSlider::mousePressEvent(QMouseEvent *event) {
    // Anything but a left click goes on to the viewer, so the context menu still opens over the bar. Mid-drag
    // it ends the drag too: the menu it opens takes the left button's release, which would never come here.
    if(event->button() != Qt::LeftButton || !acceptsInput()) {
        finishDrag();
        event->ignore();
        return;
    }
    event->accept();
    mDragging = true;
    mDragValue = valueAt(event->position().x());
    // A click on the value already sent (the bar has moved on since) must still seek.
    mLastSent = -1;
    update();
    sendDragValue();
    showToolTip(event->globalPosition().toPoint());
}

void MediaSlider::mouseMoveEvent(QMouseEvent *event) {
    // The left button's release went elsewhere, to a popup or another window: this is a hover, not a drag.
    if(mDragging && !(event->buttons() & Qt::LeftButton)) {
        finishDrag();
        setHovered(rect().contains(event->position().toPoint()));
    }
    if(!mDragging) {
        showToolTip(event->globalPosition().toPoint());
        // Let the viewer see the movement too: it shows the cursor and the thumbnail panel's hover zone.
        event->ignore();
        return;
    }
    event->accept();
    qreal oldX = positionOf(mDragValue);
    mDragValue = valueAt(event->position().x());
    updateBetween(oldX, positionOf(mDragValue));
    showToolTip(event->globalPosition().toPoint());
    if(!mSinceSent.isValid() || mSinceSent.elapsed() >= mDragInterval)
        sendDragValue();
    else if(!mDragTimer.isActive())
        mDragTimer.start(static_cast<int>(mDragInterval - mSinceSent.elapsed()));
}

void MediaSlider::mouseReleaseEvent(QMouseEvent *event) {
    if(!mDragging || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    event->accept();
    finishDrag();
    if(!rect().contains(event->position().toPoint()))
        setHovered(false);
}

bool MediaSlider::takeWheel(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if(!acceptsInput() || delta == 0)
        return false;
    if(event->inverted())
        delta = -delta;
    // High-resolution wheels and touchpads send fractions of a notch.
    mWheelRemainder += delta;
    int steps = mWheelRemainder / 120;
    mWheelRemainder -= steps * 120;
    if(steps == 0 || mDragging)
        return true;
    double target = std::clamp(mValue + steps * mWheelStep, 0.0, mMaximum);
    setValue(target);
    emit valueRequested(target);
    return true;
}

void MediaSlider::wheelEvent(QWheelEvent *event) {
    event->setAccepted(takeWheel(event));
}

void MediaSlider::enterEvent(QEnterEvent *event) {
    QWidget::enterEvent(event);
    setHovered(true);
}

void MediaSlider::leaveEvent(QEvent *event) {
    QWidget::leaveEvent(event);
    if(!mDragging)
        setHovered(false);
    QToolTip::hideText();
}

void MediaSlider::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    // Hidden mid-drag (the folder view covers the viewer): the release may never come back here.
    finishDrag();
    setHovered(false);
}

void MediaSlider::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        // Another window took the mouse along with the focus, and the release with it.
        finishDrag();
    } else if(event->type() == QEvent::EnabledChange) {
        if(!isEnabled())
            cancelDrag();
        update();
    }
}
