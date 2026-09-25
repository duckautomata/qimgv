#include "mpvwidget.h"
#include <QResizeEvent>
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
        qDebug() << "[mpv] option" << name << "=" << value << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

// Same rationale as setMpvOption(), for properties set after mpv_initialize().
static bool setMpvProperty(mpv_handle *mpv, const char *name, const QString &value) {
    const int rc = mpv::qt::set_property(mpv, QString::fromLatin1(name), value);
    if(rc < 0) {
        qDebug() << "[mpv] property" << name << "=" << value << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

// Same again for numbers, without waiting for mpv's core; the replies come back as
// MPV_EVENT_SET_PROPERTY_REPLY. A float QVariant would reach mpv as MPV_FORMAT_NONE and fail, so these
// always go over as a double.
static bool setMpvDoubleAsync(mpv_handle *mpv, uint64_t reply, const char *name, double value) {
    const int rc = mpv_set_property_async(mpv, reply, name, MPV_FORMAT_DOUBLE, &value);
    if(rc < 0) {
        qDebug() << "[mpv] property" << name << "=" << value << "rejected:" << mpv_error_string(rc);
        return false;
    }
    return true;
}

// reply_userdata of the asynchronous video-out-params query, and of placement sets.
static constexpr uint64_t kVideoSizeReply = 1;
static constexpr uint64_t kPlacementReply = 2;

static void wakeup(void *ctx) {
    QMetaObject::invokeMethod((MpvWidget *)ctx, "on_mpv_events", Qt::QueuedConnection);
}

static void *get_proc_address(void *ctx, const char *name) {
    Q_UNUSED(ctx);
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if(!glctx)
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

MpvWidget::MpvWidget(QWidget *parent, Qt::WindowFlags f) : QOpenGLWidget(parent, f) {
    setFormat(surfaceFormat());

    mpv = mpv_create();
    if(!mpv)
        throw std::runtime_error("could not create mpv context");

    this->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // setMpvOption(mpv, "terminal", "yes");
    // setMpvOption(mpv, "msg-level", "all=v");
    setMpvOption(mpv, "vo", "libmpv");

    // Preserve the alpha channel when the codec provides one, so ProRes 4444,
    // VP9/AV1 with alpha and transparent WebM composite against the app
    // background instead of rendering over black. No cost for opaque video.
    //
    // mpv renamed this in 0.38: "alpha=yes" became "background=none". Try the
    // current name first and fall back, so we work against old and new libmpv.
    if(!setMpvOption(mpv, "background", "none"))
        setMpvOption(mpv, "alpha", "yes");

    // Centres an axis the video fits whatever its align, which keeps a zoomed video centred when a
    // resize makes it fit before the app has re-placed it. The app sends align 0 for such an axis
    // anyway, because libmpv only has this from 0.40. Changes nothing at the default align of 0.
    setMpvOption(mpv, "video-recenter", "yes");

    if(mpv_initialize(mpv) < 0)
        throw std::runtime_error("could not initialize mpv context");

    // Hardware decoding, with a safe fallback to software. "auto-safe" only
    // picks a hwdec backend that is known-good on the current platform, which
    // matters for H.265 / AV1 where a broken driver path is worse than CPU
    // decoding.
    mpv::qt::set_property(mpv, "hwdec", "auto-safe");

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
    if(mpv_gl)
        mpv_render_context_free(mpv_gl);
    mpv_terminate_destroy(mpv);
}

void MpvWidget::command(const QVariant &params) {
    mpv::qt::command(mpv, params);
}

void MpvWidget::loadFile(const QString &file) {
    // Armed before the command, and events only reach us through the queued on_mpv_events(), so no
    // event for the new file can slip in first. update() repaints the retained framebuffer now rather
    // than on mpv's next frame.
    mCover.arm();
    mHoldCover = false;
    update();
    const QVariant result = mpv::qt::command(mpv, QStringList() << "loadfile" << file);
    // On failure stay covered: there is nothing of this file to show, and mpv does not ask for a
    // repaint when it drops the old frame.
    if(!mpv::qt::is_error(result)) {
        const QVariant anyEntry = QVariant::fromValue(qlonglong(StaleFrameCover::kAnyEntry));
        mCover.setEntryId(result.toMap().value("playlist_entry_id", anyEntry).toLongLong());
    }
}

void MpvWidget::stop() {
    // mpv drops its frame on stop without asking for a repaint, so the widget would keep showing it.
    mCover.arm();
    mHoldCover = false;
    update();
    command(QVariantList() << "stop");
}

void MpvWidget::setVideoPlacement(MpvPlacementOptions const &placement) {
    // A drag sends dozens of these a second, and every set is a round trip to mpv's core.
    if(mPlacementApplied && placement == mPlacement)
        return;
    mPlacement = placement;
    mPlacementApplied = true;
    // Asynchronous: a synchronous set waits for mpv's core, and after every file start, loop and seek
    // the core waits in turn for us to render its first frame -- the two stall each other until mpv's
    // 200 ms render timeout. The render after the last reply applies them; see paintGL().
    const char *unscaled = mpvUnscaledName(placement.unscaled);
    const int rc = mpv_set_property_async(mpv, kPlacementReply, "video-unscaled", MPV_FORMAT_STRING, &unscaled);
    if(rc < 0)
        qDebug() << "[mpv] property video-unscaled =" << unscaled << "rejected:" << mpv_error_string(rc);
    const bool sent[] = {rc >= 0, setMpvDoubleAsync(mpv, kPlacementReply, "video-zoom", placement.zoom),
                         setMpvDoubleAsync(mpv, kPlacementReply, "video-align-x", placement.alignX),
                         setMpvDoubleAsync(mpv, kPlacementReply, "video-align-y", placement.alignY)};
    for(bool s : sent) {
        if(s)
            ++mPlacementReplies;
        else
            mPlacementApplied = false;
    }
}

// The app needs the size mpv renders at to zoom around a point and to tell when the video overflows.
void MpvWidget::resizeEvent(QResizeEvent *event) {
    emit viewportResized(QSize(width(), height()));
    QOpenGLWidget::resizeEvent(event);
}

bool MpvWidget::event(QEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // The size in device pixels changes with no resize event.
    if(event->type() == QEvent::DevicePixelRatioChange)
        emit viewportResized(QSize(width(), height()));
#endif
    return QOpenGLWidget::event(event);
}

void MpvWidget::setProperty(const QString &name, const QVariant &value) {
    mpv::qt::set_property(mpv, name, value);
}

QVariant MpvWidget::getProperty(const QString &name) const {
    return mpv::qt::get_property(mpv, name);
}

void MpvWidget::initializeGL() {
    mpv_opengl_init_params gl_init_params{get_proc_address, nullptr};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};

    if(mpv_render_context_create(&mpv_gl, mpv, params) < 0)
        throw std::runtime_error("failed to initialize mpv GL context");
    mpv_render_context_set_update_callback(mpv_gl, MpvWidget::on_update, reinterpret_cast<void *>(this));
}

void MpvWidget::setTransparencyGrid(QPixmap const &tile) {
    mTransparencyGrid = tile;
    update();
}

void MpvWidget::setBackgroundColor(QColor color) {
    if(mBackgroundColor == color)
        return;
    mBackgroundColor = color;
    // The composite in paintGL() only reaches pixels mpv left transparent. The
    // letterbox bars mpv draws around a video whose aspect does not match the
    // widget are opaque, so they stay black however we composite underneath --
    // a visible seam against the rest of the window. Hand mpv the same colour.
    if(mpv)
        setMpvProperty(mpv, "background-color", color.name(QColor::HexRgb));
    update();
}

// Draws the current frame. Split out of paintGL() because the minimized-window
// path in maybeUpdate() drives it directly, with no QPainter in scope.
void MpvWidget::renderMpv(QSize target) {
    // Tell mpv the target has 8 bits of alpha so it emits transparent pixels
    // rather than compositing onto black itself.
    mpv_opengl_fbo mpfbo{static_cast<int>(defaultFramebufferObject()), target.width(), target.height(), GL_RGBA8};
    int flip_y{1};

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo}, {MPV_RENDER_PARAM_FLIP_Y, &flip_y}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    // See render_gl.h on what OpenGL environment mpv expects, and
    // other API details.
    mpv_render_context_render(mpv_gl, params);
}

void MpvWidget::paintGL() {
    QPainter painter(this);

    // mpv has to render even while we cover it: it waits (up to 200 ms) for each
    // frame it hands over to be consumed, so skipping the render would hold back
    // the new file's first frame by that much.
    painter.beginNativePainting();
    if(mRelayout && height() > 1) {
        // mpv re-reads its placement options when the target size changes, or once its VO thread has
        // seen the change -- which that thread cannot do while a frame waits for this render. After a
        // placement lands, and before the first uncovered frame (the new file was placed while
        // covered), a throwaway render at another size makes sure this one uses it. The real render
        // below overwrites all of it.
        renderMpv(QSize(width(), height() - 1));
    }
    mRelayout = false;
    renderMpv(QSize(width(), height()));
    painter.endNativePainting();

    if(mCover.covered() || mHoldCover) {
        // Source, not SourceOver: the background may be translucent, and the old
        // frame must not show through it.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), mBackgroundColor);
        return;
    }

    // With "background=none" mpv leaves genuinely transparent pixels wherever
    // the video carries alpha (ProRes 4444, VP9/AV1 alpha, transparent WebM).
    // Qt hands this widget's alpha straight to the translucent top-level
    // window, so those pixels show the user's desktop rather than qimgv --
    // and painting a background on an ancestor widget does not help, because
    // the GL content is not blended against the backing store. It has to be
    // composited here, under what mpv just drew, while we still own the alpha.
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    // Each DestinationOver pass goes underneath everything already drawn, so
    // the grid slides between mpv's output and the background: it appears only
    // where the video is actually transparent. The letterbox bars mpv fills in
    // background-color are opaque and keep covering it, which is what the image
    // viewer does too -- the grid marks the picture, not the padding around it.
    if(!mTransparencyGrid.isNull())
        painter.drawTiledPixmap(rect(), mTransparencyGrid);
    painter.fillRect(rect(), mBackgroundColor);
}

void MpvWidget::on_mpv_events() {
    // Process all events, until the event queue is empty.
    while(mpv) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if(event->event_id == MPV_EVENT_NONE) {
            break;
        }
        handle_mpv_event(event);
    }
}

void MpvWidget::handle_mpv_event(mpv_event *event) {
    switch(event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = reinterpret_cast<mpv_event_property *>(event->data);
        if(strcmp(prop->name, "time-pos") == 0) {
            if(prop->format == MPV_FORMAT_DOUBLE) {
                double time = *reinterpret_cast<double *>(prop->data);
                emit positionChanged(static_cast<int>(time));
            }
        } else if(strcmp(prop->name, "duration") == 0) {
            if(prop->format == MPV_FORMAT_DOUBLE) {
                double time = *reinterpret_cast<double *>(prop->data);
                emit durationChanged(static_cast<int>(time));
            } else if(prop->format == MPV_FORMAT_NONE) {
                emit playbackFinished();
            }
        } else if(strcmp(prop->name, "pause") == 0) {
            int mode = *reinterpret_cast<int *>(prop->data);
            emit videoPaused(mode == 1);
        }
        break;
    }
    case MPV_EVENT_START_FILE: {
        // No payload before libmpv 0.33; that is also when loadfile reports no entry id (kAnyEntry).
        auto startFile = reinterpret_cast<mpv_event_start_file *>(event->data);
        mCover.onStartFile(startFile ? startFile->playlist_entry_id : StaleFrameCover::kAnyEntry);
        break;
    }
    case MPV_EVENT_VIDEO_RECONFIG:
        requestVideoSize();
        break;
    case MPV_EVENT_GET_PROPERTY_REPLY:
        if(event->reply_userdata == kVideoSizeReply) {
            onVideoSizeReply(event);
            if(mCover.onSizeReply())
                onVideoSettled();
        }
        break;
    case MPV_EVENT_SET_PROPERTY_REPLY:
        if(event->reply_userdata == kPlacementReply) {
            if(event->error < 0) {
                qDebug() << "[mpv] placement rejected:" << mpv_error_string(event->error);
                // Send all four again next time.
                mPlacementApplied = false;
            }
            if(mPlacementReplies > 0 && --mPlacementReplies == 0) {
                mHoldCover = false;
                mRelayout = true;
                update();
            }
        }
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
        // Only reached once mpv has queued the new file's first frame (or has
        // no video to show). Earlier signals are too early: FILE_LOADED comes
        // before any decoding, and VIDEO_RECONFIG fires while tearing down the
        // old file -- and not at all when both files share their parameters.
        // Loops and seeks also restart playback, but by then the cover is off.
        if(mCover.onPlaybackRestart())
            onVideoSettled();
        break;
    default:;
        // Ignore uninteresting or unknown events.
    }
}

// The cover is down: the new file's first frame is about to show, and its size is known.
void MpvWidget::onVideoSettled() {
    // Every file announces its size here, once, even when it matches the last one: the app forgets
    // the size on a new file so that nothing can be zoomed against the previous file's while this one
    // loads. Sent before the render below, which is the first one anyone sees.
    emit videoSizeChanged(mVideoSize);
    // The app placed this file when it was loaded (back to the fit), and the sets are asynchronous: if
    // they are still in flight, a render now would show the previous file's zoom. Stay covered until
    // the replies are in; the last one lifts this.
    if(mPlacementReplies > 0)
        mHoldCover = true;
    mRelayout = true;
    update();
}

void MpvWidget::requestVideoSize() {
    // Asynchronous on purpose. For a new file's first frame mpv emits VIDEO_RECONFIG and then waits
    // until we have rendered that frame, on this thread; a synchronous read waits for mpv's core in
    // turn, and neither side moves until mpv's 200 ms render timeout.
    if(mpv_get_property_async(mpv, kVideoSizeReply, "video-out-params", MPV_FORMAT_NODE) >= 0)
        mCover.onSizeRequested();
}

void MpvWidget::onVideoSizeReply(mpv_event *event) {
    // Unavailable means there is no video output, so no picture.
    QSize size;
    auto property = reinterpret_cast<mpv_event_property *>(event->data);
    if(event->error >= 0 && property->format == MPV_FORMAT_NODE) {
        const QVariantMap params = mpv::qt::node_to_variant(reinterpret_cast<mpv_node *>(property->data)).toMap();
        size = mpvDisplaySize(params.value("dw").toInt(), params.value("dh").toInt(), params.value("rotate").toInt());
    }
    if(size == mVideoSize)
        return;
    mVideoSize = size;
    // While covered, onVideoSettled() sends it: a reply now may still describe the previous file.
    if(!mCover.covered())
        emit videoSizeChanged(size);
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
        renderMpv(QSize(width(), height())); // nothing is visible; skip the QPainter composite
        context()->swapBuffers(context()->surface());
        doneCurrent();
    } else {
        update();
    }
}

void MpvWidget::on_update(void *ctx) {
    QMetaObject::invokeMethod((MpvWidget *)ctx, "maybeUpdate");
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
