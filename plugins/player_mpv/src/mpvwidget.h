#pragma once

#include <QtCore/QMetaObject>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include "qthelper.hpp"

#include <QDebug>
#include <QColor>
#include <QPainter>
#include <ctime>
#include <QSurfaceFormat>
#include <QTimer>

class MpvWidget Q_DECL_FINAL : public QOpenGLWidget {
    Q_OBJECT
public:
    MpvWidget(QWidget *parent = nullptr, Qt::WindowFlags f = Qt::Widget);
    ~MpvWidget() override;

    void command(const QVariant &params);
    void setOption(const QString &name, const QVariant &value);
    void setProperty(const QString &name, const QVariant &value);
    QVariant getProperty(const QString &name) const;
    // Related to this:
    // https://github.com/gnome-mpv/gnome-mpv/issues/245
    // Let's hope this wont break more than it fixes
    int width() const { return static_cast<int>(QOpenGLWidget::width() * this->devicePixelRatioF()); }
    int height() const { return static_cast<int>(QOpenGLWidget::height() * this->devicePixelRatioF()); }
    void setMuted(bool mode);
    void setRepeat(bool mode);
    // Colour transparent video is composited onto. See paintGL().
    void setBackgroundColor(QColor color);

    // Returns the QSurfaceFormat qimgv must install before the first
    // QOpenGLWidget is created, so that video with an alpha channel
    // (ProRes 4444, VP9/AV1 with alpha, transparent WebM) composites
    // against the app background instead of rendering on black.
    static QSurfaceFormat surfaceFormat();
    bool muted();
    int volume();
    void setVolume(int vol);

signals:
    void durationChanged(int value);
    void positionChanged(int value);
    void videoPaused(bool);
    void playbackFinished();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void renderMpv();
    QColor mBackgroundColor = Qt::black;

private slots:
    void on_mpv_events();
    void maybeUpdate();

private:
    void handle_mpv_event(mpv_event *event);
    static void on_update(void *ctx);

    // Both must be null-initialized: the destructor may run without
    // initializeGL() ever having been called (plugin loaded, never shown).
    mpv_handle *mpv = nullptr;
    mpv_render_context *mpv_gl = nullptr;
};
