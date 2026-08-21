#include "mpvwidget.h"
#include <stdexcept>

// Qt does not pull in a GL header that guarantees this constant on every
// platform/driver combination, and we only need the one value.
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// mpv_set_option_string() returns an error code that is easy to drop on the
// floor, which hides typos and renamed options. Always route through this.
static bool setMpvOption(mpv_handle *mpv, const char *name, const char *value) {
    const int rc = mpv_set_option_string(mpv, name, value);
    if(rc < 0) {
        qDebug() << "[mpv] option" << name << "=" << value
                 << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

static void wakeup(void *ctx) {
    QMetaObject::invokeMethod((MpvWidget*)ctx, "on_mpv_events", Qt::QueuedConnection);
}

static void *get_proc_address(void *ctx, const char *name) {
    Q_UNUSED(ctx);
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (!glctx)
        return nullptr;
    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

QSurfaceFormat MpvWidget::surfaceFormat() {
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    // An alpha channel in the framebuffer is what lets mpv hand us
    // premultiplied transparent pixels for ProRes 4444 / VP9 / AV1 alpha.
    fmt.setAlphaBufferSize(8);
    fmt.setRedBufferSize(8);
    fmt.setGreenBufferSize(8);
    fmt.setBlueBufferSize(8);
    // No depth or stencil: we only ever blit mpv's output.
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    return fmt;
}

MpvWidget::MpvWidget(QWidget *parent, Qt::WindowFlags f)
    : QOpenGLWidget(parent, f)
{
    setFormat(surfaceFormat());

    mpv = mpv_create();
    if(!mpv)
        throw std::runtime_error("could not create mpv context");

    this->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    //setMpvOption(mpv, "terminal", "yes");
    //setMpvOption(mpv, "msg-level", "all=v");
    setMpvOption(mpv, "vo", "libmpv");

    // Preserve the alpha channel when the codec provides one, so ProRes 4444,
    // VP9/AV1 with alpha and transparent WebM composite against the app
    // background instead of rendering over black. No cost for opaque video.
    //
    // mpv renamed this in 0.38: "alpha=yes" became "background=none". Try the
    // current name first and fall back, so we work against old and new libmpv.
    if(!setMpvOption(mpv, "background", "none"))
        setMpvOption(mpv, "alpha", "yes");

    if (mpv_initialize(mpv) < 0)
        throw std::runtime_error("could not initialize mpv context");

    // Hardware decoding, with a safe fallback to software. "auto-safe" only
    // picks a hwdec backend that is known-good on the current platform, which
    // matters for H.265 / AV1 where a broken driver path is worse than CPU
    // decoding.
    mpv::qt::set_property(mpv, "hwdec", "auto-safe");

    //mpv::qt::set_property(mpv, "video-unscaled", "downscale-big");

    // Loop video
    setRepeat(true);

    // Unmute
    setMuted(false);

    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(mpv, wakeup, this);
}

MpvWidget::~MpvWidget() {
    makeCurrent();
    if (mpv_gl)
        mpv_render_context_free(mpv_gl);
    mpv_terminate_destroy(mpv);
}

void MpvWidget::command(const QVariant& params) {
    mpv::qt::command(mpv, params);
}

void MpvWidget::setProperty(const QString& name, const QVariant& value) {
    mpv::qt::set_property(mpv, name, value);
}

QVariant MpvWidget::getProperty(const QString &name) const {
    return mpv::qt::get_property(mpv, name);
}

void MpvWidget::setOption(const QString& name, const QVariant& value) {
    mpv::qt::set_property(mpv, name, value);
}

void MpvWidget::initializeGL() {
    mpv_opengl_init_params gl_init_params{get_proc_address, nullptr};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if(mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        throw std::runtime_error("failed to initialize mpv GL context");
    mpv_render_context_set_update_callback(mpv_gl, MpvWidget::on_update, reinterpret_cast<void *>(this));
}

void MpvWidget::paintGL() {
    // Tell mpv the target has 8 bits of alpha so it emits transparent pixels
    // rather than compositing onto black itself.
    mpv_opengl_fbo mpfbo{static_cast<int>(defaultFramebufferObject()),
                         width(), height(), GL_RGBA8};
    int flip_y{1};

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    // See render_gl.h on what OpenGL environment mpv expects, and
    // other API details.
    mpv_render_context_render(mpv_gl, params);
}

void MpvWidget::on_mpv_events() {
    // Process all events, until the event queue is empty.
    while (mpv) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handle_mpv_event(event);
    }
}

void MpvWidget::handle_mpv_event(mpv_event *event) {
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = reinterpret_cast<mpv_event_property*>(event->data);
        if(strcmp(prop->name, "time-pos") == 0) {
            if (prop->format == MPV_FORMAT_DOUBLE) {
                double time = *reinterpret_cast<double*>(prop->data);
                emit positionChanged(static_cast<int>(time));
            }
        } else if(strcmp(prop->name, "duration") == 0) {
            if(prop->format == MPV_FORMAT_DOUBLE) {
                double time = *reinterpret_cast<double*>(prop->data);
                emit durationChanged(static_cast<int>(time));
            } else if(prop->format == MPV_FORMAT_NONE) {
                emit playbackFinished();
            }
        } else if(strcmp(prop->name, "pause") == 0) {
            int mode = *reinterpret_cast<int*>(prop->data);
            emit videoPaused(mode == 1);
        }
        break;
    }
    default: ;
        // Ignore uninteresting or unknown events.
    }
}

// Make Qt invoke mpv_render_context_render() to draw a new/updated video frame.
void MpvWidget::maybeUpdate() {
    // If the Qt window is not visible, Qt's update() will just skip rendering.
    // This confuses mpv's render API, and may lead to small occasional
    // freezes due to video rendering timing out.
    // Handle this by manually redrawing.
    // Note: Qt doesn't seem to provide a way to query whether update() will
    //       be skipped, and the following code still fails when e.g. switching
    //       to a different workspace with a reparenting window manager.
    if(window()->isMinimized()) {
        makeCurrent();
        paintGL();
        context()->swapBuffers(context()->surface());
        doneCurrent();
    } else {
        update();
    }
}

void MpvWidget::on_update(void *ctx) {
    QMetaObject::invokeMethod((MpvWidget*)ctx, "maybeUpdate");
}

void MpvWidget::setMuted(bool mode) {
    if(mode)
        mpv::qt::set_property(mpv, "mute", "yes");
    else
        mpv::qt::set_property(mpv, "mute", "no");
}

bool MpvWidget::muted() {
    return mpv::qt::get_property_variant(mpv, "mute").toBool();
}

int MpvWidget::volume() {
    return mpv::qt::get_property_variant(mpv, "volume").toInt();
}

void MpvWidget::setVolume(int vol) {
    mpv::qt::set_property_variant(mpv, "volume", qBound(0, vol, 100));
}

void MpvWidget::setRepeat(bool mode) {
    if(mode)
        mpv::qt::set_property(mpv, "loop-file", "inf");
    else
        mpv::qt::set_property(mpv, "loop-file", "no");
}
