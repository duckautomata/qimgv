#include "videoplayerinitproxy.h"
#include "videozoom.h"

#ifdef _QIMGV_PLAYER_PLUGIN
#define QIMGV_PLAYER_PLUGIN _QIMGV_PLAYER_PLUGIN
#else
#define QIMGV_PLAYER_PLUGIN ""
#endif

static VideoZoomSettings videoZoomSettings() {
    VideoZoomSettings s;
    s.expandImage = settings->expandImage();
    s.zoomStep = settings->zoomStep();
    if(settings->useFixedZoomLevels())
        s.zoomLevels = VideoZoomModel::parseZoomLevels(settings->zoomLevels());
    s.unlockMinZoom = settings->unlockMinZoom();
    s.scrolling = settings->imageScrolling();
    s.scrollingSpeed = settings->mouseScrollingSpeed();
    s.trackpadDetection = settings->trackpadDetection();
    s.wayland = QGuiApplication::platformName() == QLatin1String("wayland");
    return s;
}

VideoPlayerInitProxy::VideoPlayerInitProxy(QWidget *parent) : VideoPlayer(parent), player(nullptr) {
    setAccessibleName("VideoPlayerInitProxy");
    setMouseTracking(true);
    layout.setContentsMargins(0, 0, 0, 0);
    setLayout(&layout);
    connect(settings, &Settings::settingsChanged, this, &VideoPlayerInitProxy::onSettingsChanged);
    mZoom = new VideoZoom(this, this);
    mZoom->setSettings(videoZoomSettings());
    // Pointers to members work here: this object's meta-object is the app's copy of VideoPlayer. The
    // player's signals reach these through the SIGNAL() forwards in initPlayer().
    connect(this, &VideoPlayer::videoSizeChanged, mZoom, &VideoZoom::onVideoSizeChanged);
    connect(this, &VideoPlayer::viewportResized, mZoom, &VideoZoom::onViewportResized);
    updateBackgroundColor();
    // initPlayer() reads this when the plugin is eventually loaded, which is
    // long before the first settingsChanged.
    mTransparencyGrid = settings->transparencyGrid();

#ifdef USE_MPV
    // Adding the first QOpenGLWidget to a window that is already on screen makes
    // Qt switch the backing store to a texture-composited one, and on Windows
    // that destroys and recreates the native window -- qimgv visibly blinks shut
    // and reopens the first time you land on a video. Parking an empty hidden
    // QOpenGLWidget here means the window is already texture-backed before it is
    // ever shown. It is never laid out and never painted, so it never creates a
    // GL context; the cost is one QWidget.
    glBackingStorePin = new QOpenGLWidget(this);
    glBackingStorePin->hide();
#endif

    libFile = QIMGV_PLAYER_PLUGIN;
#ifdef _WIN32
    libDirs << QApplication::applicationDirPath() + "/plugins";
#else
    QDir libPath(QApplication::applicationDirPath() + "/../lib/qimgv");
    libDirs << (libPath.makeAbsolute() ? libPath.path() : ".") << "/usr/lib/qimgv" << "/usr/lib64/qimgv";
#endif
}

VideoPlayerInitProxy::~VideoPlayerInitProxy() {}

void VideoPlayerInitProxy::onSettingsChanged() {
    // Background first: it must track the theme even before a video is loaded.
    updateBackgroundColor();
    mTransparencyGrid = settings->transparencyGrid();
    updateTransparencyGrid();
    // Also where "expand images" reaches the player now, as part of its placement.
    mZoom->setSettings(videoZoomSettings());
    if(!player)
        return;
    player->setMuted(!settings->playVideoSounds());
}

// Mirrors ImageViewerV2::onFullscreenModeChanged so that switching between an
// image and a video does not change the backdrop.
void VideoPlayerInitProxy::onFullscreenModeChanged(bool mode) {
    mIsFullscreen = mode;
    updateBackgroundColor();
}

void VideoPlayerInitProxy::updateBackgroundColor() {
    if(mIsFullscreen) {
        bgColor = settings->colorScheme().background_fullscreen;
        bgColor.setAlphaF(1.0);
    } else {
        bgColor = settings->colorScheme().background;
        bgColor.setAlphaF(settings->backgroundOpacity());
    }
    // The player has to do the actual compositing -- see MpvWidget::paintGL().
    if(player)
        player->setBackgroundColor(bgColor);
    update();
}

// Mirrors ImageViewerV2::toggleTransparencyGrid(): a temporary override that is
// deliberately not written back to settings.
void VideoPlayerInitProxy::toggleTransparencyGrid() {
    mTransparencyGrid = !mTransparencyGrid;
    updateTransparencyGrid();
}

void VideoPlayerInitProxy::updateTransparencyGrid() {
    if(!player)
        return;
    if(mTransparencyGrid && checkboard.isNull())
        checkboard.load(QStringLiteral(":res/icons/common/other/checkerboard.png"));
    player->setTransparencyGrid(mTransparencyGrid ? checkboard : QPixmap());
}

std::shared_ptr<VideoPlayer> VideoPlayerInitProxy::getPlayer() {
    return player;
}

bool VideoPlayerInitProxy::isInitialized() {
    return (player != nullptr);
}

inline bool VideoPlayerInitProxy::initPlayer() {
#ifndef USE_MPV
    return false;
#endif
    if(player)
        return true;

    QFileInfo pluginFile;
    for(auto dir : libDirs) {
        pluginFile.setFile(dir + "/" + libFile);
        if(pluginFile.isFile() && pluginFile.isReadable()) {
            playerLib.setFileName(pluginFile.absoluteFilePath());
            break;
        }
    }
    if(playerLib.fileName().isEmpty()) {
        qDebug() << "Could not find" << libFile << "in the following directories:" << libDirs;
        return false;
    }

    // load lib
    typedef VideoPlayer *(*createPlayerWidgetFn)();
    createPlayerWidgetFn fn = (createPlayerWidgetFn)playerLib.resolve("CreatePlayerWidget");
    if(fn) {
        VideoPlayer *pl = fn();
        player.reset(pl);
    }
    if(!player) {
        qDebug() << "Could not load:" << playerLib.fileName() << ". Wrong plugin version?";
        return false;
    }

    player->setMuted(!settings->playVideoSounds());
    player->setVolume(settings->volume());
    player->setBackgroundColor(bgColor);
    updateTransparencyGrid();

    player->setParent(this);
    layout.addWidget(player.get());
    player->hide();
    setFocusProxy(player.get());
    connect(player.get(), SIGNAL(durationChanged(int)), this, SIGNAL(durationChanged(int)));
    connect(player.get(), SIGNAL(positionChanged(int)), this, SIGNAL(positionChanged(int)));
    connect(player.get(), SIGNAL(videoPaused(bool)), this, SIGNAL(videoPaused(bool)));
    connect(player.get(), SIGNAL(playbackFinished()), this, SIGNAL(playbackFinished()));
    connect(player.get(), SIGNAL(videoSizeChanged(QSize)), this, SIGNAL(videoSizeChanged(QSize)));
    connect(player.get(), SIGNAL(viewportResized(QSize)), this, SIGNAL(viewportResized(QSize)));

    // Before the click-zone filter below: the filter installed last runs first, and the edge zones
    // must keep priority over pan and zoom.
    player->installEventFilter(mZoom);
    if(eventFilterObj)
        player.get()->installEventFilter(eventFilterObj);

    return true;
}

bool VideoPlayerInitProxy::showVideo(QString file) {
    if(!initPlayer() || !player->showVideo(file))
        return false;
    // Only now: the player has covered the previous file's frame, which mpv would otherwise visibly
    // re-lay out at the new file's zoom.
    mZoom->onNewFile();
    return true;
}

void VideoPlayerInitProxy::seek(int pos) {
    if(!player)
        return;
    player->seek(pos);
}

void VideoPlayerInitProxy::seekRelative(int pos) {
    if(!player)
        return;
    player->seekRelative(pos);
}

void VideoPlayerInitProxy::pauseResume() {
    if(!player)
        return;
    player->pauseResume();
}

void VideoPlayerInitProxy::frameStep() {
    if(!player)
        return;
    player->frameStep();
}

void VideoPlayerInitProxy::frameStepBack() {
    if(!player)
        return;
    player->frameStepBack();
}

void VideoPlayerInitProxy::stop() {
    if(!player)
        return;
    player->stop();
}

void VideoPlayerInitProxy::setPaused(bool mode) {
    if(!player)
        return;
    player->setPaused(mode);
}

void VideoPlayerInitProxy::setMuted(bool mode) {
    if(!player)
        return;
    player->setMuted(mode);
}

bool VideoPlayerInitProxy::muted() {
    if(!player)
        return true;
    return player->muted();
}

void VideoPlayerInitProxy::volumeUp() {
    if(!player)
        return;
    player->volumeUp();
    settings->setVolume(player->volume());
}

void VideoPlayerInitProxy::volumeDown() {
    if(!player)
        return;
    player->volumeDown();
    settings->setVolume(player->volume());
}

void VideoPlayerInitProxy::setVolume(int vol) {
    if(!player)
        return;
    player->setVolume(vol);
}

int VideoPlayerInitProxy::volume() {
    if(!player)
        return 0;
    return player->volume();
}

void VideoPlayerInitProxy::setVideoUnscaled(bool mode) {
    if(!player)
        return;
    player->setVideoUnscaled(mode);
}

void VideoPlayerInitProxy::setLoopPlayback(bool mode) {
    if(!player)
        return;
    player->setLoopPlayback(mode);
}

void VideoPlayerInitProxy::setPlacement(Placement mode, double scale, double alignX, double alignY) {
    if(!player)
        return;
    player->setPlacement(mode, scale, alignX, alignY);
}

VideoZoom *VideoPlayerInitProxy::zoom() const {
    return mZoom;
}

void VideoPlayerInitProxy::show() {
    if(initPlayer()) {
        layout.removeWidget(errorLabel);
        player->show();
    } else if(!errorLabel) {
        errorLabel = new QLabel(this);
        errorLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        errorLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        // errorLabel->setAlignment(Qt::AlignVCenter);
        QString errString = "Could not load " + libFile + " from:";
        for(auto path : libDirs)
            errString.append("\n" + path + "/");
        errorLabel->setText(errString);
        layout.addWidget(errorLabel);
    }
    VideoPlayer::show();
}

void VideoPlayerInitProxy::hide() {
    if(player)
        player->hide();
    VideoPlayer::hide();
}

// Nothing else in the video path paints a background: this widget, VideoPlayerMpv
// and MpvWidget are all WA_TranslucentBackground, and with mpv's "background=none"
// the decoder now hands us genuinely transparent pixels for ProRes 4444 / VP9 /
// AV1 alpha. Without this fill those pixels go all the way through the window and
// you see the desktop. The image path gets the equivalent from the graphics
// scene's background brush.
void VideoPlayerInitProxy::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter p(this);
    p.fillRect(rect(), bgColor);
}

void VideoPlayerInitProxy::installEventFilter(QObject *filterObj) {
    eventFilterObj = filterObj;
    if(player)
        player->installEventFilter(eventFilterObj);
}

void VideoPlayerInitProxy::removeEventFilter(QObject *filterObj) {
    if(player)
        player->removeEventFilter(filterObj);
}
