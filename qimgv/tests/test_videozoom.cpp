// Video zoom tests, by case id:
//   M  VideoZoomModel: the pure rules and math, device pixels throughout.
//   H  mpvplacement.h: the mpv options behind one placement, and the rotated display size.
//   G  StaleFrameCover: when the previous file's frame may show again.
//   C  VideoZoom: the controller, driven through a FakePlayer with synthetic mouse and wheel events.
// Every video opens fitted to the window, and a zoom or pan lasts until the next file. Gaps in the
// numbering are cases for fit modes, focus points and locks, which videos do not have.
// Unless stated otherwise the viewport is 1000x800 and the video 640x360, with default settings.
#include <QtTest>
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>
#include <cmath>
#include <limits>
#include "mpvplacement.h"
#include "staleframecover.h"
#include "videoplayer.h"
#include "videozoom.h"
#include "videozoommodel.h"

// ---------------------------------------------------------------------------
// Readable failure messages for the types compared below (found by ADL).
// ---------------------------------------------------------------------------
static const char *placementName(VideoPlayer::Placement mode) {
    switch(mode) {
    case VideoPlayer::PLACEMENT_FIT_SHRINK:
        return "FIT_SHRINK";
    case VideoPlayer::PLACEMENT_FIT_GROW:
        return "FIT_GROW";
    case VideoPlayer::PLACEMENT_SCALED:
        return "SCALED";
    }
    return "?";
}

static char *toString(VideoPlayer::Placement mode) {
    return qstrdup(placementName(mode));
}

static char *toString(VideoZoomPlacement const &p) {
    return qstrdup(QStringLiteral("{%1, %2, %3, %4}")
                       .arg(QLatin1String(placementName(p.mode)))
                       .arg(p.scale, 0, 'g', 12)
                       .arg(p.alignX, 0, 'g', 12)
                       .arg(p.alignY, 0, 'g', 12)
                       .toUtf8()
                       .constData());
}

static char *toString(MpvPlacementOptions const &o) {
    return qstrdup(QStringLiteral("{%1, %2, %3, %4}")
                       .arg(QLatin1String(mpvUnscaledName(o.unscaled)))
                       .arg(o.zoom, 0, 'g', 12)
                       .arg(o.alignX, 0, 'g', 12)
                       .arg(o.alignY, 0, 'g', 12)
                       .toUtf8()
                       .constData());
}

// ---------------------------------------------------------------------------
// Tolerant comparisons. Expected values are written as the exact expression, with the rounded value
// in a comment, so the tolerance can stay tight.
// ---------------------------------------------------------------------------
#define COMPARE_NEAR_EPS(actual, expected, eps)                                                                        \
    do {                                                                                                               \
        const double actual_ = (actual);                                                                               \
        const double expected_ = (expected);                                                                           \
        QVERIFY2(std::abs(actual_ - expected_) <= (eps), qPrintable(QStringLiteral("%1 is %2, expected %3")            \
                                                                        .arg(QLatin1String(#actual))                   \
                                                                        .arg(actual_, 0, 'g', 12)                      \
                                                                        .arg(expected_, 0, 'g', 12)));                 \
    } while(false)

#define COMPARE_NEAR(actual, expected) COMPARE_NEAR_EPS(actual, expected, 1e-6)

#define COMPARE_RECT(actual, ex, ey, ew, eh)                                                                           \
    do {                                                                                                               \
        const QRectF rect_ = (actual);                                                                                 \
        COMPARE_NEAR(rect_.x(), ex);                                                                                   \
        COMPARE_NEAR(rect_.y(), ey);                                                                                   \
        COMPARE_NEAR(rect_.width(), ew);                                                                               \
        COMPARE_NEAR(rect_.height(), eh);                                                                              \
    } while(false)

#define COMPARE_PLACEMENT(actual, m, s, ax, ay)                                                                        \
    do {                                                                                                               \
        const VideoZoomPlacement placement_ = (actual);                                                                \
        QCOMPARE(placement_.mode, m);                                                                                  \
        COMPARE_NEAR(placement_.scale, s);                                                                             \
        COMPARE_NEAR(placement_.alignX, ax);                                                                           \
        COMPARE_NEAR(placement_.alignY, ay);                                                                           \
    } while(false)

#define COMPARE_LAST_PLACEMENT(rig, m, s, ax, ay)                                                                      \
    do {                                                                                                               \
        QVERIFY(!(rig).player.placements.isEmpty());                                                                   \
        COMPARE_PLACEMENT((rig).player.placements.last(), m, s, ax, ay);                                               \
    } while(false)

// Controller tests work in logical pixels and expect them to equal device pixels.
#define REQUIRE_DPR1(rig)                                                                                              \
    do {                                                                                                               \
        if((rig).player.devicePixelRatioF() != 1.0)                                                                    \
            QSKIP("the controller tests assume a device pixel ratio of 1");                                            \
    } while(false)

static constexpr VideoPlayer::Placement SHRINK = VideoPlayer::PLACEMENT_FIT_SHRINK;
static constexpr VideoPlayer::Placement GROW = VideoPlayer::PLACEMENT_FIT_GROW;
static constexpr VideoPlayer::Placement SCALED = VideoPlayer::PLACEMENT_SCALED;

// alignForEdge() written out, for expected values: the align that puts a video of `size` at `edge`.
static double alignAt(double view, double size, double edge) {
    return 2 * edge / (view - size) - 1;
}

// ---------------------------------------------------------------------------
// Model fixtures
// ---------------------------------------------------------------------------
static VideoZoomSettings expandSettings(bool expand) {
    VideoZoomSettings settings;
    settings.expandImage = expand;
    return settings;
}

static VideoZoomModel makeModel(QSize video = QSize(640, 360), VideoZoomSettings const &settings = {}) {
    VideoZoomModel model;
    model.setSettings(settings);
    model.setViewport(QSize(1000, 800));
    model.setVideoSize(video);
    return model;
}

static const QPointF kCentre(500, 400);

// ---------------------------------------------------------------------------
// Controller fixtures
// ---------------------------------------------------------------------------

// Every pure virtual is a no-op; records what VideoZoom asks of the player.
class FakePlayer : public VideoPlayer {
public:
    explicit FakePlayer(QWidget *parent = nullptr) : VideoPlayer(parent) {}

    QList<VideoZoomPlacement> placements;
    int pauses = 0;
    int mousePresses = 0;
    int mouseReleases = 0;

    bool showVideo(QString) override { return true; }
    void seek(int) override {}
    void seekRelative(int) override {}
    void pauseResume() override { ++pauses; }
    void frameStep() override {}
    void frameStepBack() override {}
    void stop() override {}
    void setPaused(bool) override {}
    void setMuted(bool) override {}
    bool muted() override { return false; }
    void volumeUp() override {}
    void volumeDown() override {}
    void setVolume(int) override {}
    int volume() override { return 0; }
    void setVideoUnscaled(bool) override {}
    void setLoopPlayback(bool) override {}
    void setPlacement(Placement mode, double scale, double alignX, double alignY) override {
        placements.append(VideoZoomPlacement{mode, scale, alignX, alignY});
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        ++mousePresses;
        QWidget::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        ++mouseReleases;
        QWidget::mouseReleaseEvent(event);
    }
};

// A 1000x800 player whose VideoZoom has seen the viewport and a file of the given size, in the order
// the app delivers them: onNewFile(), then the size the player announces as the file shows. With
// `start` false the size arrives but no file was ever shown.
struct Rig {
    // A child widget, as in the app: QWidget::unsetCursor() leaves WA_SetCursor set on a window.
    QWidget window;
    FakePlayer player{&window};
    VideoZoom zoom{&player};
    QSignalSpy scales{&zoom, &VideoZoom::scaleChanged};

    explicit Rig(QSize video, VideoZoomSettings const &settings = {}, bool start = true) {
        window.resize(1000, 800);
        player.resize(1000, 800);
        zoom.setSettings(settings);
        zoom.onViewportResized(QSize(1000, 800));
        if(start)
            newFile(video);
        else
            zoom.onVideoSizeChanged(video);
    }
    void newFile(QSize video) {
        zoom.onNewFile();
        zoom.onVideoSizeChanged(video);
    }
    double lastScale() const { return scales.isEmpty() ? -1 : scales.last().at(0).toDouble(); }
    bool cursorSet() const { return player.testAttribute(Qt::WA_SetCursor); }
};

// One step in from a fit between 0.5 and 1 to exactly 1:1, through a fixed zoom level at 1 (a multiple
// of zoomStep would miss it). The rig's own settings are back in place afterwards.
static void zoomToOne(Rig &rig) {
    const VideoZoomSettings original = rig.zoom.model().settings();
    VideoZoomSettings levels = original;
    levels.zoomLevels = {0.5, 1};
    rig.zoom.setSettings(levels);
    rig.zoom.zoomIn();
    rig.zoom.setSettings(original);
}

// "Pan-ready": a 1280x720 video zoomed from its fit (0.78125) to 1:1, overflowing
// horizontally with its left edge at -140.
static bool makePanReady(Rig &rig) {
    zoomToOne(rig);
    const QRectF rect = rig.zoom.model().videoRect();
    rig.player.placements.clear();
    rig.scales.clear();
    return !rig.zoom.model().isFitted() && rig.zoom.model().scale() == 1 && rig.zoom.model().overflows() &&
           std::abs(rect.x() + 140) < 1e-9 && std::abs(rect.y() - 40) < 1e-9;
}

static bool mouseEvent(Rig &rig, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, pos, rig.player.mapToGlobal(pos), button, buttons, Qt::NoModifier);
    return rig.zoom.eventFilter(&rig.player, &event);
}

static bool pressAt(Rig &rig, Qt::MouseButton button, QPointF pos) {
    return mouseEvent(rig, QEvent::MouseButtonPress, pos, button, button);
}

static bool moveTo(Rig &rig, QPointF pos, Qt::MouseButtons held) {
    return mouseEvent(rig, QEvent::MouseMove, pos, Qt::NoButton, held);
}

static bool releaseAt(Rig &rig, Qt::MouseButton button, QPointF pos) {
    return mouseEvent(rig, QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
}

static bool doubleClickAt(Rig &rig, QPointF pos) {
    return mouseEvent(rig, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
}

static bool wheelAt(Rig &rig, QPointF pos, QPoint angle, QPoint pixels = QPoint(), Qt::MouseButtons held = Qt::NoButton,
                    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(pos, rig.player.mapToGlobal(pos), pixels, angle, held, modifiers, Qt::NoScrollPhase, false);
    return rig.zoom.eventFilter(&rig.player, &event);
}

// ---------------------------------------------------------------------------

class Test_VideoZoom : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // VideoZoomModel
    void m00_noVideo();
    void m01_fitWindow();
    void m02_fitWindowExpand();
    void m03_fitWindowLargeVideo();
    void m06_minScale();
    void m07_steps();
    void m08_zoomToKeepsAnchor();
    void m09_zoomToSnapsAndClamps();
    void m10_panBy();
    void m11_reset();
    void m14_viewport();
    void m15_settings();
    void m16_parseZoomLevels();
    void m17_isMouseWheel();
    void m18_edgeAndAlign();

    // mpvplacement.h
    void h01_mpvOptionsFor();
    void h02_mpvDisplaySize();

    // StaleFrameCover
    void g00_initiallyUncovered();
    void g01_uncoversOnMatchingRestart();
    void g02_restartBeforeStart();
    void g03_otherEntry();
    void g04_rearmedForNextFile();
    void g05_failedLoad();
    void g06_anyEntry();
    void g07_restartAfterUncover();
    void g08_waitsForSizeReply();
    void g09_replyBeforeRestart();
    void g10_twoRequests();
    void g11_rearmKeepsRequests();
    void g12_replyWithoutRequest();

    // VideoZoom
    void c01_inactiveBeforeNewFile();
    void c02_newFilePlacesOnce();
    void c03_leftClickOnFittingVideo();
    void c04_zoomIn();
    void c05_clickPausesOnRelease();
    void c06_leftDragPans();
    void c07_smallMoveIsAClick();
    void c08_rightDragZooms();
    void c09_rightClick();
    void c10_rightWheelZooms();
    void c11_ctrlWheel();
    void c12_trackpad();
    void c13_wheelScrollsThenNavigates();
    void c14_interactionDisabled();
    void c15_doubleClick();
    void c16_newFileEndsGesture();
    void c17_deactivated();
    void c18_resizeReplacesSynchronously();
    void c19_indicator();
    void c20_scrollSlots();
    void c21_zoomAtCursor();
    void c24_installedFilter();
    void c25_otherButtons();
    void c26_lostRelease();
    void c27_rightDoubleClick();
    void c28_wheelZoomDuringPan();
    void c29_lostRightRelease();
    void c31_newFileResetsZoom();
    void c32_newFileResetsZoomExpand();
    void c33_newFileSameSize();
    void c34_noZoomWhileLoading();
};

void Test_VideoZoom::initTestCase() {
    // C6/C7 are written against a 10 px drag threshold (a 30 px move pans, a 7 px one does not).
    QApplication::setStartDragDistance(10);
}

// ===========================================================================
// VideoZoomModel
// ===========================================================================

// No size (first video, audio only) or no viewport: the player's own fit, scale 1.
void Test_VideoZoom::m00_noVideo() {
    VideoZoomModel model;
    QVERIFY(!model.hasVideo());
    QVERIFY(model.isFitted());
    QCOMPARE(model.scale(), 1.0);
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QVERIFY(model.videoRect().isNull());
    QVERIFY(!model.overflows());
    QVERIFY(!model.zoomTo(2, kCentre));
    QVERIFY(model.isFitted());

    model.setViewport(QSize(1000, 800));
    QVERIFY(!model.hasVideo());
    model.setVideoSize(QSize(640, 360));
    QVERIFY(model.hasVideo());
}

// M1
void Test_VideoZoom::m01_fitWindow() {
    const VideoZoomModel model = makeModel();
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QCOMPARE(model.scale(), 1.0);
    COMPARE_RECT(model.videoRect(), 180, 220, 640, 360);
    QVERIFY(!model.overflows());
}

// M2
void Test_VideoZoom::m02_fitWindowExpand() {
    const VideoZoomModel model = makeModel(QSize(640, 360), expandSettings(true));
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));
    COMPARE_NEAR(model.scale(), 1.5625);
    COMPARE_RECT(model.videoRect(), 0, 118.75, 1000, 562.5);
    QVERIFY(!model.overflows());
}

// M3
void Test_VideoZoom::m03_fitWindowLargeVideo() {
    for(bool expand : {false, true}) {
        const VideoZoomModel model = makeModel(QSize(1920, 1080), expandSettings(expand));
        COMPARE_NEAR(model.scale(), 1000.0 / 1920); // 0.520833
        COMPARE_RECT(model.videoRect(), 0, 118.75, 1000, 562.5);
        QCOMPARE(model.placement().mode, expand ? GROW : SHRINK);
        QVERIFY(!model.overflows());
    }
}

// M6
void Test_VideoZoom::m06_minScale() {
    COMPARE_NEAR(makeModel().minScale(), 10.0 / 360); // 0.0277778

    VideoZoomSettings locked;
    locked.unlockMinZoom = false;
    COMPARE_NEAR(makeModel(QSize(640, 360), locked).minScale(), 1);
    COMPARE_NEAR(makeModel(QSize(1920, 1080), locked).minScale(), 1000.0 / 1920); // 0.520833
}

// M7
void Test_VideoZoom::m07_steps() {
    const QList<double> none;
    COMPARE_NEAR(VideoZoomModel::stepIn(1, 0.2, none), 1.2);
    COMPARE_NEAR(VideoZoomModel::stepOut(1, 0.2, none), 0.8);

    const QList<double> levels = {0.5, 1, 2};
    COMPARE_NEAR(VideoZoomModel::stepIn(0.3, 0.2, levels), 0.36);
    COMPARE_NEAR(VideoZoomModel::stepIn(0.45, 0.2, levels), 0.5);
    COMPARE_NEAR(VideoZoomModel::stepIn(1, 0.2, levels), 2);
    COMPARE_NEAR(VideoZoomModel::stepIn(2, 0.2, levels), 2.4);
    COMPARE_NEAR(VideoZoomModel::stepOut(3, 0.2, levels), 2.4);
    COMPARE_NEAR(VideoZoomModel::stepOut(2.2, 0.2, levels), 2);
    COMPARE_NEAR(VideoZoomModel::stepOut(1, 0.2, levels), 0.5);
    COMPARE_NEAR(VideoZoomModel::stepOut(0.5, 0.2, levels), 0.4);

    // An empty list behaves as no list.
    for(double s : {0.3, 1.0, 2.0, 7.5}) {
        COMPARE_NEAR(VideoZoomModel::stepIn(s, 0.2, none), s * 1.2);
        COMPARE_NEAR(VideoZoomModel::stepOut(s, 0.2, none), s * 0.8);
    }

    // Through the model: zoomStep uses the settings' step and levels.
    VideoZoomSettings settings;
    settings.zoomLevels = levels;
    VideoZoomModel model = makeModel(QSize(640, 360), settings);
    QVERIFY(model.zoomStep(true, kCentre));
    COMPARE_NEAR(model.scale(), 2);
    QVERIFY(!model.isFitted());
    QVERIFY(model.zoomStep(false, kCentre));
    COMPARE_NEAR(model.scale(), 1);
    // 1 is this video's fit, so that step lands back on it.
    QVERIFY(model.isFitted());
}

// M8
void Test_VideoZoom::m08_zoomToKeepsAnchor() {
    VideoZoomModel model = makeModel();
    QVERIFY(model.zoomTo(2, kCentre));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0, 0);
    COMPARE_RECT(model.videoRect(), -140, 40, 1280, 720);
    QVERIFY(!model.isFitted());
    QVERIFY(model.overflows());

    // Anchors that would pull an edge into view are clamped flush.
    model = makeModel();
    model.zoomTo(2, QPointF(300, 400));
    COMPARE_NEAR(model.placement().alignX, -1);
    COMPARE_NEAR(model.videoRect().x(), 0);

    model = makeModel();
    model.zoomTo(2, QPointF(900, 400));
    COMPARE_NEAR(model.placement().alignX, 1);
    COMPARE_NEAR(model.videoRect().x(), -280);

    // An anchor inside the range keeps the same point of the video under it.
    model = makeModel();
    const double before = (450.0 - model.videoRect().x()) / model.videoRect().width();
    COMPARE_NEAR_EPS(before, 0.421875, 1e-9);
    model.zoomTo(2, QPointF(450, 400));
    COMPARE_NEAR(model.placement().alignX, alignAt(1000, 1280, -90)); // -0.357143
    COMPARE_NEAR(model.videoRect().x(), -90);
    const double after = (450.0 - model.videoRect().x()) / model.videoRect().width();
    COMPARE_NEAR_EPS(after, 0.421875, 1e-9);
}

// M9
void Test_VideoZoom::m09_zoomToSnapsAndClamps() {
    // Zooming onto exactly the fit returns to it: the player fits by itself again.
    VideoZoomModel model = makeModel();
    model.zoomTo(2, kCentre);
    QVERIFY(model.zoomTo(1, kCentre));
    QVERIFY(model.isFitted()); // 1:1 is this video's fit
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));

    // Only exactly: a hair off it is still a zoom.
    VideoZoomModel nearly = makeModel();
    QVERIFY(nearly.zoomTo(1 + 1e-9, kCentre));
    QVERIFY(!nearly.isFitted());
    QCOMPARE(nearly.placement().mode, SCALED);

    // With expandImage the fit is the enlarged one, and GROW comes back.
    VideoZoomModel expanded = makeModel(QSize(640, 360), expandSettings(true));
    expanded.zoomTo(3, kCentre);
    QVERIFY(!expanded.isFitted());
    QVERIFY(expanded.zoomTo(1.5625, kCentre));
    QVERIFY(expanded.isFitted());
    QCOMPARE(expanded.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));

    QVERIFY(model.zoomTo(1000, kCentre));
    COMPARE_NEAR(model.scale(), VideoZoomModel::kMaxScale);
    QVERIFY(!model.isFitted());
    // zoomTo() reports whenever target != current: a request clamped at the bound still reports, so
    // the indicator shows why nothing moved (as ImageViewerV2::doZoom, which always emits). Only a
    // request for the current scale reports nothing.
    QVERIFY(model.zoomTo(1000, kCentre));
    COMPARE_NEAR(model.scale(), VideoZoomModel::kMaxScale);
    QVERIFY(!model.zoomTo(VideoZoomModel::kMaxScale, kCentre));

    QVERIFY(model.zoomTo(1e-4, kCentre));
    COMPARE_NEAR(model.scale(), 10.0 / 360); // 0.0277778
    QVERIFY(!model.isFitted());

    // Clamped at a minScale that is also the fit (unlockMinZoom off, a video larger than the window):
    // that lands on the fit as well.
    VideoZoomSettings locked;
    locked.unlockMinZoom = false;
    model = makeModel(QSize(1920, 1080), locked);
    QVERIFY(model.zoomStep(true, kCentre));
    COMPARE_NEAR(model.scale(), 1000.0 / 1920 * 1.2); // 0.625
    QVERIFY(model.zoomStep(false, kCentre));
    COMPARE_NEAR(model.scale(), 1000.0 / 1920); // 0.520833
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
}

// M10
void Test_VideoZoom::m10_panBy() {
    auto zoomed = [] {
        VideoZoomModel model = makeModel();
        model.zoomTo(2, kCentre); // x0 -140
        return model;
    };
    VideoZoomModel model = zoomed();
    COMPARE_NEAR(model.videoRect().x(), -140);
    model.panBy(QPointF(100, 0));
    COMPARE_NEAR(model.placement().alignX, alignAt(1000, 1280, -40)); // -0.714286
    // Panning keeps the zoom.
    QVERIFY(!model.isFitted());
    COMPARE_NEAR(model.scale(), 2);

    model = zoomed();
    model.panBy(QPointF(200, 0));
    COMPARE_NEAR(model.placement().alignX, -1);

    model = zoomed();
    model.panBy(QPointF(-200, 0));
    COMPARE_NEAR(model.placement().alignX, 1);

    model = zoomed();
    model.panBy(QPointF(0, 50)); // 720 fits in 800
    COMPARE_NEAR(model.placement().alignY, 0);
    COMPARE_NEAR(model.videoRect().y(), 40);

    // Both axes: 640x480 at 2 overflows on both.
    model = makeModel(QSize(640, 480));
    model.zoomTo(2, kCentre);
    model.panBy(QPointF(-70, 20));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0.5, -0.25);
    COMPARE_RECT(model.videoRect(), -210, -60, 1280, 960);

    // A video that fits does not pan...
    model = makeModel();
    model.panBy(QPointF(100, 100));
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    COMPARE_RECT(model.videoRect(), 180, 220, 640, 360);

    // ...nor does a zoomed one that still fits (768x432).
    model = makeModel();
    model.zoomTo(1.2, kCentre);
    model.panBy(QPointF(100, 100));
    COMPARE_PLACEMENT(model.placement(), SCALED, 1.2, 0, 0);
    COMPARE_RECT(model.videoRect(), 116, 184, 768, 432);
}

// M11, now reset(): back to the fit a video opens in, with nothing of the zoom or the pan left.
void Test_VideoZoom::m11_reset() {
    VideoZoomModel model = makeModel(QSize(640, 480));
    model.zoomTo(3, QPointF(300, 200));
    model.panBy(QPointF(-70, 20));
    QVERIFY(!model.isFitted());
    model.reset();
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    COMPARE_NEAR(model.scale(), 1);
    COMPARE_RECT(model.videoRect(), 180, 160, 640, 480);
    QVERIFY(!model.overflows());

    // Zooming again starts from the fit and the anchor, not from the old scale or pan.
    QVERIFY(model.zoomStep(true, kCentre));
    COMPARE_PLACEMENT(model.placement(), SCALED, 1.2, 0, 0);
    COMPARE_RECT(model.videoRect(), 116, 112, 768, 576);
    model.reset();
    QVERIFY(model.zoomTo(2, kCentre));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0, 0);
    COMPARE_RECT(model.videoRect(), -140, -80, 1280, 960);

    // Fitted again, it follows the window.
    model.reset();
    model.setViewport(QSize(500, 300));
    COMPARE_NEAR(model.scale(), 300.0 / 480); // 0.625

    // With expandImage the fit is the enlarged one, and so is the step from it.
    model = makeModel(QSize(640, 360), expandSettings(true));
    model.zoomTo(3, kCentre);
    model.panBy(QPointF(100, 0));
    model.reset();
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));
    COMPARE_NEAR(model.scale(), 1.5625);
    QVERIFY(model.zoomStep(true, kCentre));
    COMPARE_PLACEMENT(model.placement(), SCALED, 1.5625 * 1.2, 0, 0); // 1.875

    // A next file the same size as the last: the size changes nothing, reset() alone decides.
    model = makeModel();
    model.zoomTo(2, kCentre);
    model.panBy(QPointF(100, 0));
    model.reset();
    model.setVideoSize(QSize(640, 360));
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));

    // Without a video it is simply the player's own fit.
    VideoZoomModel empty;
    empty.reset();
    QVERIFY(empty.isFitted());
    QCOMPARE(empty.placement(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
}

// M14
void Test_VideoZoom::m14_viewport() {
    // The fit follows the window, and is still the player's own.
    VideoZoomModel model = makeModel(QSize(640, 360), expandSettings(true));
    COMPARE_NEAR(model.scale(), 1.5625);
    model.setViewport(QSize(1200, 800));
    COMPARE_NEAR(model.scale(), 1.875);
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));

    // An empty viewport falls back to the player's own fit.
    model.setViewport(QSize(1000, 0));
    QVERIFY(!model.hasVideo());
    QCOMPARE(model.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));

    // A zoom keeps its scale and align; the player re-centres and clamps against the new width.
    model = makeModel();
    model.zoomTo(2, kCentre);
    model.panBy(QPointF(-70, 0));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0.5, 0);
    model.setViewport(QSize(1200, 800));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0.5, 0);
    COMPARE_NEAR(model.videoRect().x(), -60);
    QVERIFY(!model.isFitted());

    // A window it now fits is sent centred on that axis; a narrower one brings the pan back.
    model.setViewport(QSize(1400, 800));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0, 0);
    COMPARE_NEAR(model.videoRect().x(), 60);
    model.setViewport(QSize(1000, 800));
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0.5, 0);
    COMPARE_NEAR(model.videoRect().x(), -210);
}

// M15
void Test_VideoZoom::m15_settings() {
    VideoZoomModel model = makeModel();
    model.setSettings(expandSettings(true));
    QVERIFY(model.isFitted());
    QCOMPARE(model.placement(), (VideoZoomPlacement{GROW, 1, 0, 0}));
    COMPARE_NEAR(model.scale(), 1.5625);

    // A zoom survives a settings change.
    model = makeModel();
    model.zoomTo(2, kCentre);
    model.setSettings(expandSettings(true));
    QVERIFY(!model.isFitted());
    COMPARE_PLACEMENT(model.placement(), SCALED, 2, 0, 0);
}

// M16
void Test_VideoZoom::m16_parseZoomLevels() {
    QCOMPARE(VideoZoomModel::parseZoomLevels(QStringLiteral(" 2,0.5,abc,1,-1")), (QList<double>{0.5, 1, 2}));
    QVERIFY(VideoZoomModel::parseZoomLevels(QString()).isEmpty());
    QCOMPARE(VideoZoomModel::parseZoomLevels(QStringLiteral("0,3")), (QList<double>{3}));
}

// M17
void Test_VideoZoom::m17_isMouseWheel() {
    auto wheel = [](QPoint angle, qint64 ms, bool detection = true, bool wayland = false,
                    Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        return VideoZoomModel::isMouseWheel(angle, phase, ms, detection, wayland);
    };
    QVERIFY(wheel(QPoint(0, 120), 1000));
    QVERIFY(!wheel(QPoint(0, 15), 1000));
    QVERIFY(wheel(QPoint(0, -240), 1000));
    QVERIFY(wheel(QPoint(0, 180), 1000));
    QVERIFY(!wheel(QPoint(0, -150), 1000));
    QVERIFY(!wheel(QPoint(30, 0), 1000));
    QVERIFY(!wheel(QPoint(0, 120), 100));
    QVERIFY(!wheel(QPoint(0, 120), 250));
    QVERIFY(wheel(QPoint(0, 120), 251));
    QVERIFY(wheel(QPoint(0, 120), std::numeric_limits<qint64>::max()));
    QVERIFY(wheel(QPoint(0, 15), 0, false));
    QVERIFY(wheel(QPoint(0, 15), 0, true, true, Qt::NoScrollPhase));
    QVERIFY(!wheel(QPoint(0, 120), 1000, true, true, Qt::ScrollUpdate));
}

// M18
void Test_VideoZoom::m18_edgeAndAlign() {
    COMPARE_NEAR(VideoZoomModel::edge(1000, 1280, -1), 0);
    COMPARE_NEAR(VideoZoomModel::edge(1000, 1280, 0), -140);
    COMPARE_NEAR(VideoZoomModel::edge(1000, 1280, 1), -280);
    COMPARE_NEAR(VideoZoomModel::edge(1000, 640, -1), 180);
    COMPARE_NEAR(VideoZoomModel::edge(1000, 640, 1), 180);
    // Half a pixel of overflow still counts as fitting.
    COMPARE_NEAR(VideoZoomModel::edge(1000, 1000.4, 1), -0.2);

    COMPARE_NEAR(VideoZoomModel::alignForEdge(1000, 1280, -40), alignAt(1000, 1280, -40)); // -0.714286
    COMPARE_NEAR(VideoZoomModel::alignForEdge(1000, 1280, 60), -1);
    COMPARE_NEAR(VideoZoomModel::alignForEdge(1000, 1280, -400), 1);
    COMPARE_NEAR(VideoZoomModel::alignForEdge(1000, 640, 50), 0);

    for(double a : {-1.0, -0.5, 0.0, 0.3, 1.0})
        COMPARE_NEAR_EPS(VideoZoomModel::alignForEdge(1000, 1280, VideoZoomModel::edge(1000, 1280, a)), a, 1e-12);
}

// ===========================================================================
// mpvplacement.h
// ===========================================================================

static MpvPlacementOptions options(MpvPlacementOptions::Unscaled unscaled, double zoom, double alignX, double alignY) {
    MpvPlacementOptions o;
    o.unscaled = unscaled;
    o.zoom = zoom;
    o.alignX = alignX;
    o.alignY = alignY;
    return o;
}

// H1
void Test_VideoZoom::h01_mpvOptionsFor() {
    using O = MpvPlacementOptions;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    QCOMPARE(mpvOptionsFor(SHRINK, 1, 0, 0), options(O::UNSCALED_DOWNSCALE_BIG, 0, 0, 0));
    QCOMPARE(mpvOptionsFor(SHRINK, 3, 0.5, 0.5), options(O::UNSCALED_DOWNSCALE_BIG, 0, 0, 0));
    QCOMPARE(mpvOptionsFor(GROW, 1, 0, 0), options(O::UNSCALED_NO, 0, 0, 0));
    // SCALED carries log2(scale), nudged up by 1e-6; see the truncation check below.
    const MpvPlacementOptions oneToOne = mpvOptionsFor(SCALED, 1, 0, 0);
    QCOMPARE(oneToOne.unscaled, O::UNSCALED_YES);
    COMPARE_NEAR_EPS(oneToOne.zoom, 0, 1e-5);
    QCOMPARE(oneToOne.alignX, 0.0);
    QCOMPARE(oneToOne.alignY, 0.0);
    const MpvPlacementOptions doubled = mpvOptionsFor(SCALED, 2, -1, 1);
    QCOMPARE(doubled.unscaled, O::UNSCALED_YES);
    COMPARE_NEAR_EPS(doubled.zoom, 1, 1e-5);
    QCOMPARE(doubled.alignX, -1.0);
    QCOMPARE(doubled.alignY, 1.0);
    COMPARE_NEAR_EPS(mpvOptionsFor(SCALED, 1.5625, 0, 0).zoom, std::log2(1.5625), 1e-5); // 0.643856

    // mpv keeps video-zoom as a float and truncates the scaled size (aspect.c: S = (int64)((float)dw *
    // powf(2, zoom))). An exact fit has to land on the whole pixel, not one short of it, or a line of
    // background shows along the edge. Unnudged, 1920 px at 1000/1920 comes out 999.
    for(int dw : {640, 853, 1080, 1280, 1920, 2560, 3840}) {
        for(int view = 200; view <= 4000; view++) {
            const float zoom = float(mpvOptionsFor(SCALED, double(view) / dw, 0, 0).zoom);
            const auto scaled = static_cast<int64_t>(float(dw) * std::pow(2.0f, zoom));
            if(scaled != view)
                QFAIL(qPrintable(QStringLiteral("%1 px at %2/%1 is %3 px").arg(dw).arg(view).arg(scaled)));
        }
    }
    QCOMPARE(mpvOptionsFor(SCALED, 1e9, 0, 0).zoom, 20.0);
    QCOMPARE(mpvOptionsFor(SCALED, 1e-9, 0, 0).zoom, -20.0);

    // A nonsensical scale falls back to the default fit.
    for(double scale : {0.0, nan, -1.0, inf})
        QCOMPARE(mpvOptionsFor(SCALED, scale, 0.5, 0.5), MpvPlacementOptions());

    QCOMPARE(mpvOptionsFor(SCALED, 1, 1.5, -3).alignX, 1.0);
    QCOMPARE(mpvOptionsFor(SCALED, 1, 1.5, -3).alignY, -1.0);
    QCOMPARE(mpvOptionsFor(SCALED, 1, nan, inf).alignX, 0.0);
    QCOMPARE(mpvOptionsFor(SCALED, 1, nan, inf).alignY, 0.0);

    QCOMPARE(QByteArray(mpvUnscaledName(O::UNSCALED_NO)), QByteArray("no"));
    QCOMPARE(QByteArray(mpvUnscaledName(O::UNSCALED_YES)), QByteArray("yes"));
    QCOMPARE(QByteArray(mpvUnscaledName(O::UNSCALED_DOWNSCALE_BIG)), QByteArray("downscale-big"));
    QCOMPARE(QByteArray(mpvUnscaledName(MpvPlacementOptions().unscaled)), QByteArray("downscale-big"));
}

// H2
void Test_VideoZoom::h02_mpvDisplaySize() {
    QCOMPARE(mpvDisplaySize(400, 200, 270), QSize(200, 400));
    QCOMPARE(mpvDisplaySize(400, 200, 90), QSize(200, 400));
    QCOMPARE(mpvDisplaySize(400, 200, 180), QSize(400, 200));
    QCOMPARE(mpvDisplaySize(853, 480, 0), QSize(853, 480));
    QVERIFY(mpvDisplaySize(0, 480, 0).isEmpty());
    QVERIFY(mpvDisplaySize(853, 0, 90).isEmpty());
    QVERIFY(mpvDisplaySize(-1, 480, 0).isEmpty());
}

// ===========================================================================
// StaleFrameCover
// ===========================================================================

// arm() + a successful loadfile that reported `id`.
static StaleFrameCover loaded(int64_t id) {
    StaleFrameCover cover;
    cover.arm();
    cover.setEntryId(id);
    return cover;
}

void Test_VideoZoom::g00_initiallyUncovered() {
    StaleFrameCover cover;
    QVERIFY(!cover.covered());
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(!cover.onSizeReply());
    cover.arm();
    QVERIFY(cover.covered());
}

// G1
void Test_VideoZoom::g01_uncoversOnMatchingRestart() {
    StaleFrameCover cover = loaded(5);
    QVERIFY(cover.covered());
    cover.onStartFile(5);
    QVERIFY(cover.covered());
    QVERIFY(cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
}

// G2
void Test_VideoZoom::g02_restartBeforeStart() {
    StaleFrameCover cover = loaded(5);
    QVERIFY(!cover.onPlaybackRestart()); // the previous file's restart
    QVERIFY(cover.covered());
    // It is not remembered: the file's own restart is still needed.
    cover.onStartFile(5);
    QVERIFY(cover.covered());
    QVERIFY(cover.onPlaybackRestart());
}

// G3
void Test_VideoZoom::g03_otherEntry() {
    StaleFrameCover cover = loaded(5);
    cover.onStartFile(4);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
}

// G4
void Test_VideoZoom::g04_rearmedForNextFile() {
    StaleFrameCover cover = loaded(5);
    cover.arm();
    cover.setEntryId(6);
    cover.onStartFile(5);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
    cover.onStartFile(6);
    QVERIFY(cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
}

// G5: a failed loadfile never sets an id, and the cover stays up for good.
void Test_VideoZoom::g05_failedLoad() {
    StaleFrameCover cover;
    cover.arm();
    cover.onStartFile(1);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
    cover.onStartFile(StaleFrameCover::kNoEntry);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
}

// G6
void Test_VideoZoom::g06_anyEntry() {
    StaleFrameCover cover = loaded(StaleFrameCover::kAnyEntry);
    cover.onStartFile(42);
    QVERIFY(cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
}

// G7: loops and seeks restart too, and find the cover down.
void Test_VideoZoom::g07_restartAfterUncover() {
    StaleFrameCover cover = loaded(5);
    cover.onStartFile(5);
    QVERIFY(cover.onPlaybackRestart());
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
    cover.onStartFile(5);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
}

// G8
void Test_VideoZoom::g08_waitsForSizeReply() {
    StaleFrameCover cover = loaded(5);
    cover.onStartFile(5);
    cover.onSizeRequested();
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
    QVERIFY(cover.onSizeReply());
    QVERIFY(!cover.covered());
}

// G9
void Test_VideoZoom::g09_replyBeforeRestart() {
    StaleFrameCover cover = loaded(5);
    cover.onSizeRequested();
    QVERIFY(!cover.onSizeReply());
    QVERIFY(cover.covered());
    cover.onStartFile(5);
    QVERIFY(cover.onPlaybackRestart());
    QVERIFY(!cover.covered());
}

// G10
void Test_VideoZoom::g10_twoRequests() {
    StaleFrameCover cover = loaded(5);
    cover.onStartFile(5);
    cover.onSizeRequested();
    cover.onSizeRequested();
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(!cover.onSizeReply());
    QVERIFY(cover.covered());
    QVERIFY(cover.onSizeReply());
    QVERIFY(!cover.covered());
}

// G11
void Test_VideoZoom::g11_rearmKeepsRequests() {
    StaleFrameCover cover = loaded(5);
    cover.onSizeRequested();
    cover.arm();
    cover.setEntryId(6);
    cover.onStartFile(6);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
    QVERIFY(cover.onSizeReply());
    QVERIFY(!cover.covered());
}

// G12: a stray reply must not leave the counter below zero, where the next request would not count.
void Test_VideoZoom::g12_replyWithoutRequest() {
    StaleFrameCover cover = loaded(5);
    QVERIFY(!cover.onSizeReply());
    cover.onSizeRequested();
    cover.onStartFile(5);
    QVERIFY(!cover.onPlaybackRestart());
    QVERIFY(cover.covered());
    QVERIFY(cover.onSizeReply());
    QVERIFY(!cover.covered());
}

// ===========================================================================
// VideoZoom
// ===========================================================================

// C1: sizes are only stored until the player is on screen; input passes through untouched.
void Test_VideoZoom::c01_inactiveBeforeNewFile() {
    Rig rig(QSize(1280, 720), {}, false);
    REQUIRE_DPR1(rig);
    rig.zoom.zoomIn();
    rig.zoom.scrollLeft();
    QVERIFY(rig.player.placements.isEmpty());
    QVERIFY(!pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::RightButton));
    QCOMPARE(rig.scales.count(), 0);
    QVERIFY(rig.zoom.model().isFitted());
    QCOMPARE(rig.player.pauses, 0);
}

// C2
void Test_VideoZoom::c02_newFilePlacesOnce() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    // Once at onNewFile() and again when the size arrives -- the same fit, which the player skips.
    QVERIFY(!rig.player.placements.isEmpty());
    for(VideoZoomPlacement const &placement : rig.player.placements)
        QCOMPARE(placement, (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QCOMPARE(rig.scales.count(), 0);
}

// C3: the plugin pauses on press, as before.
void Test_VideoZoom::c03_leftClickOnFittingVideo() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    QVERIFY(!pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 0);
    QVERIFY(!rig.cursorSet());
}

// C4
void Test_VideoZoom::c04_zoomIn() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.zoom.zoomIn();
    QCOMPARE(rig.scales.count(), 1);
    COMPARE_NEAR(rig.lastScale(), 1.2);
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1.2, 0, 0);
    QVERIFY(!rig.zoom.model().isFitted());
    // 768x432 still fits: the left button still belongs to the player.
    QVERIFY(!pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));

    rig.zoom.zoomOut();
    COMPARE_NEAR(rig.lastScale(), 1.2 * 0.8);

    // Zooming back onto the fit exactly (a fixed zoom level at 1 here) hands the fit back to the player.
    VideoZoomSettings settings;
    settings.zoomLevels = {1};
    Rig levels(QSize(640, 360), settings);
    levels.zoom.zoomIn();
    COMPARE_LAST_PLACEMENT(levels, SCALED, 1.2, 0, 0);
    levels.zoom.zoomOut();
    QCOMPARE(levels.player.placements.last(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QVERIFY(levels.zoom.model().isFitted());
    QCOMPARE(levels.scales.count(), 2);
    COMPARE_NEAR(levels.lastScale(), 1);
}

// C5
void Test_VideoZoom::c05_clickPausesOnRelease() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 0);
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 1);
    QVERIFY(rig.player.placements.isEmpty());
    QVERIFY(!rig.cursorSet());
}

// C6
void Test_VideoZoom::c06_leftDragPans() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(rig.cursorSet());
    QCOMPARE(rig.player.cursor().shape(), Qt::ClosedHandCursor);
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, alignAt(1000, 1280, -110), 0); // -0.214286
    COMPARE_NEAR(rig.zoom.model().videoRect().x(), -110);
    QVERIFY(!releaseAt(rig, Qt::LeftButton, QPointF(530, 400)));
    QCOMPARE(rig.player.pauses, 0);
    QVERIFY(!rig.cursorSet());
    // A pan keeps the zoom.
    QVERIFY(!rig.zoom.model().isFitted());
    COMPARE_NEAR(rig.zoom.model().scale(), 1);
}

// C7: below the drag distance it is still a click.
void Test_VideoZoom::c07_smallMoveIsAClick() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(505, 402), Qt::LeftButton));
    QVERIFY(rig.player.placements.isEmpty());
    QVERIFY(!rig.cursorSet());
    QVERIFY(!releaseAt(rig, Qt::LeftButton, QPointF(505, 402)));
    QCOMPARE(rig.player.pauses, 1);
}

// C8
void Test_VideoZoom::c08_rightDragZooms() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.player.placements.clear();
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!moveTo(rig, QPointF(500, 397), Qt::RightButton));
    QVERIFY(!moveTo(rig, QPointF(500, 396), Qt::RightButton)); // 4 px is still jitter
    QVERIFY(rig.player.placements.isEmpty());
    QVERIFY(!rig.cursorSet());

    // The first zoom step covers the whole distance from the press.
    QVERIFY(moveTo(rig, QPointF(500, 350), Qt::RightButton));
    QCOMPARE(rig.player.cursor().shape(), Qt::SizeVerCursor);
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1.15, 0, 0);
    QCOMPARE(rig.scales.count(), 1);
    COMPARE_NEAR(rig.lastScale(), 1.15);

    // Swallowed, so no context menu opens.
    QVERIFY(releaseAt(rig, Qt::RightButton, QPointF(500, 350)));
    QVERIFY(!rig.cursorSet());
}

// C9
void Test_VideoZoom::c09_rightClick() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.player.placements.clear();
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::RightButton, kCentre));
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.scales.count(), 0);
}

// C10
void Test_VideoZoom::c10_rightWheelZooms() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::RightButton));
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1.2, 0, 0);
    COMPARE_NEAR(rig.lastScale(), 1.2);
    QVERIFY(releaseAt(rig, Qt::RightButton, kCentre));

    // A right press made elsewhere (a panel, the controls) is released there, so the wheel zooms
    // but claims no release: a later plain right-click here still opens the context menu.
    QVERIFY(wheelAt(rig, kCentre, QPoint(0, -120), QPoint(), Qt::RightButton));
    COMPARE_NEAR(rig.zoom.model().scale(), 1.2 * 0.8);
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::RightButton, kCentre));
}

// C11: Ctrl+wheel reaches zoomInCursor() through the action manager instead.
void Test_VideoZoom::c11_ctrlWheel() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::NoButton, Qt::ControlModifier));
    QVERIFY(makePanReady(rig));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::NoButton, Qt::ControlModifier));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(30, 0), QPoint(), Qt::NoButton, Qt::ControlModifier));
    QVERIFY(rig.player.placements.isEmpty());
}

// C12
void Test_VideoZoom::c12_trackpad() {
    Rig fitting(QSize(640, 360));
    REQUIRE_DPR1(fitting);
    QVERIFY(!wheelAt(fitting, kCentre, QPoint(30, 0)));

    Rig rig(QSize(1280, 720));
    QVERIFY(makePanReady(rig));
    QVERIFY(wheelAt(rig, kCentre, QPoint(30, 0)));
    COMPARE_NEAR(rig.zoom.model().videoRect().x(), -119);
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, -0.15, 0);

    // A notch right after trackpad input is taken for the trackpad too, and swallowed while zoomed.
    Rig recent(QSize(1280, 720));
    QVERIFY(makePanReady(recent));
    QVERIFY(!wheelAt(recent, kCentre, QPoint(0, -120))); // a plain notch navigates
    QElapsedTimer timer;
    timer.start();
    QVERIFY(wheelAt(recent, kCentre, QPoint(30, 0)));
    const bool swallowed = wheelAt(recent, kCentre, QPoint(0, -120));
    if(timer.elapsed() > 200)
        QSKIP("too slow to stay inside the 250 ms trackpad window");
    QVERIFY(swallowed);
}

// C13
void Test_VideoZoom::c13_wheelScrollsThenNavigates() {
    VideoZoomSettings settings;
    settings.scrolling = SCROLL_BY_TRACKPAD_AND_WHEEL;
    settings.trackpadDetection = false;
    Rig rig(QSize(360, 1280), settings);
    REQUIRE_DPR1(rig);
    zoomToOne(rig); // from the fit, 0.625
    COMPARE_NEAR(rig.zoom.model().videoRect().y(), -240);

    QVERIFY(wheelAt(rig, kCentre, QPoint(0, -120)));
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, 0, 1);
    COMPARE_NEAR(rig.zoom.model().videoRect().y(), -480);
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, -120))); // at the bottom: on to the next file
    QVERIFY(wheelAt(rig, kCentre, QPoint(0, 120)));
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, 0, 0);

    // The default SCROLL_BY_TRACKPAD navigates with the wheel even while zoomed.
    settings.scrolling = SCROLL_BY_TRACKPAD;
    rig.zoom.setSettings(settings);
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, -120)));
}

// C14
void Test_VideoZoom::c14_interactionDisabled() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));

    // Disabling mid-drag ends the gesture.
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(rig.cursorSet());
    rig.zoom.setInteractionEnabled(false);
    QVERIFY(!rig.cursorSet());
    QVERIFY(!releaseAt(rig, Qt::LeftButton, QPointF(530, 400)));
    rig.player.placements.clear();

    QVERIFY(!pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::RightButton));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(30, 0)));
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.player.pauses, 0);

    rig.zoom.setInteractionEnabled(true);
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
}

// C15: the double-click goes on to the main window (fullscreen); only the first release paused.
void Test_VideoZoom::c15_doubleClick() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!doubleClickAt(rig, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 1);

    Rig fitting(QSize(640, 360));
    QVERIFY(!doubleClickAt(fitting, kCentre));
}

// C16: the next file opens fitted and is then made pan-ready again, so only the reset gesture explains
// a pass-through.
void Test_VideoZoom::c16_newFileEndsGesture() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(rig.cursorSet());

    rig.newFile(QSize(1280, 720));
    QVERIFY(!rig.cursorSet());
    QVERIFY(rig.zoom.model().isFitted());
    QVERIFY(makePanReady(rig));
    QVERIFY(!moveTo(rig, QPointF(560, 400), Qt::LeftButton));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, QPointF(560, 400)));
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.player.pauses, 0);
}

// C17
void Test_VideoZoom::c17_deactivated() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.zoom.deactivate();
    rig.player.placements.clear();
    rig.zoom.onViewportResized(QSize(1200, 800));
    rig.zoom.onVideoSizeChanged(QSize(1280, 720));
    rig.zoom.zoomIn();
    rig.zoom.zoomOutCursor();
    rig.zoom.scrollDown();
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.scales.count(), 0);
    QVERIFY(rig.zoom.model().isFitted());
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::RightButton));
    QVERIFY(!releaseAt(rig, Qt::RightButton, kCentre));

    // The viewport stored meanwhile is used once the player is back; the size comes with the file.
    rig.newFile(QSize(1280, 720));
    COMPARE_NEAR(rig.zoom.model().scale(), 1200.0 / 1280);
    QCOMPARE(rig.zoom.model().viewport(), QSize(1200, 800));
}

// C18: the placement for the new size is handed to the player before onViewportResized() returns.
void Test_VideoZoom::c18_resizeReplacesSynchronously() {
    // A zoom the window now fits is sent centred on that axis, and the pan returns with the old width.
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    rig.zoom.scrollLeft();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, -1, 0);
    rig.zoom.onViewportResized(QSize(1400, 800));
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, 0, 0);
    rig.zoom.onViewportResized(QSize(1000, 800));
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, -1, 0);

    // A fitted video stays with the player's own fit, which follows the window by itself.
    Rig fitted(QSize(640, 360), expandSettings(true));
    fitted.player.placements.clear();
    fitted.zoom.onViewportResized(QSize(1200, 800));
    QCOMPARE(fitted.player.placements.size(), 1);
    QCOMPARE(fitted.player.placements.last(), (VideoZoomPlacement{GROW, 1, 0, 0}));
    COMPARE_NEAR(fitted.zoom.model().scale(), 1.875);
}

// C19
void Test_VideoZoom::c19_indicator() {
    // A zoom reports, and keeps its scale on resize: nothing new to report.
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.zoom.zoomIn();
    QCOMPARE(rig.scales.count(), 1);
    rig.zoom.onViewportResized(QSize(1200, 800));
    QCOMPARE(rig.scales.count(), 1);

    // An untouched video never shows the indicator, even when its scale changes.
    Rig untouched(QSize(640, 360), expandSettings(true));
    untouched.zoom.onViewportResized(QSize(1200, 800));
    COMPARE_NEAR(untouched.zoom.model().scale(), 1.875);
    untouched.zoom.onVideoSizeChanged(QSize(1920, 1080));
    untouched.zoom.setSettings(expandSettings(false));
    QCOMPARE(untouched.scales.count(), 0);

    // Nor does the next file after a zoom: it opens fitted, as any other.
    Rig next(QSize(640, 360), expandSettings(true));
    next.zoom.zoomIn();
    QCOMPARE(next.scales.count(), 1);
    next.zoom.onNewFile();
    next.zoom.onViewportResized(QSize(1200, 800));
    next.zoom.onVideoSizeChanged(QSize(1920, 1080));
    QCOMPARE(next.scales.count(), 1);
}

void Test_VideoZoom::c20_scrollSlots() {
    // 240 logical px per step; the content moves, so scrolling left shows more of the left.
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    rig.zoom.scrollLeft();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, -1, 0);
    rig.zoom.scrollRight();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, alignAt(1000, 1280, -240), 0); // 0.714286
    rig.zoom.scrollUp();
    rig.zoom.scrollDown();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, alignAt(1000, 1280, -240), 0); // 720 fits in 800
    QCOMPARE(rig.scales.count(), 0);

    Rig tall(QSize(360, 1280));
    zoomToOne(tall); // y0 -240
    tall.zoom.scrollDown();
    COMPARE_LAST_PLACEMENT(tall, SCALED, 1, 0, 1);
    tall.zoom.scrollUp();
    COMPARE_LAST_PLACEMENT(tall, SCALED, 1, 0, 0);
    tall.zoom.scrollUp();
    COMPARE_LAST_PLACEMENT(tall, SCALED, 1, 0, -1);

    // A video that fits does not move.
    Rig fitting(QSize(640, 360));
    fitting.zoom.scrollDown();
    fitting.zoom.scrollRight();
    COMPARE_LAST_PLACEMENT(fitting, SHRINK, 1, 0, 0);
    COMPARE_RECT(fitting.zoom.model().videoRect(), 180, 220, 640, 360);
}

// zoomInCursor() anchors at the cursor only while it is over the player, else at the centre.
void Test_VideoZoom::c21_zoomAtCursor() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(!rig.player.underMouse());
    rig.zoom.zoomInCursor();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1.2, 0, 0);

    Rig over(QSize(1280, 720));
    QVERIFY(makePanReady(over));
    QCursor::setPos(over.player.mapToGlobal(QPoint(300, 400)));
    if(over.player.mapFromGlobal(QCursor::pos()) != QPoint(300, 400))
        QSKIP("this platform cannot place the cursor");
    over.player.setAttribute(Qt::WA_UnderMouse, true);
    over.zoom.zoomInCursor();
    // The video point under x 300 (0.34375 of the way across) stays there: 300 - 0.34375 * 1536.
    COMPARE_LAST_PLACEMENT(over, SCALED, 1.2, alignAt(1000, 1536, -228), 0); // -0.149254
    COMPARE_NEAR(over.lastScale(), 1.2);
}

// Installed as an event filter, as the proxy does: consumed events never reach the player.
void Test_VideoZoom::c24_installedFilter() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    rig.player.installEventFilter(&rig.zoom);
    QVERIFY(makePanReady(rig));

    auto send = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, kCentre, rig.player.mapToGlobal(kCentre), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(&rig.player, &event);
    };
    send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(rig.player.mousePresses, 0);
    send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(rig.player.mouseReleases, 1);
    QCOMPARE(rig.player.pauses, 1);

    // A video that fits (the next file, back in the fit): the press reaches the player, which pauses
    // on press as before.
    rig.newFile(QSize(1280, 720));
    send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(rig.player.mousePresses, 1);
    send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(rig.player.mouseReleases, 2);
    QCOMPARE(rig.player.pauses, 1);
}

// Middle and extra buttons are never taken, zoomed or not.
void Test_VideoZoom::c25_otherButtons() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    for(Qt::MouseButton button : {Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton}) {
        QVERIFY(!pressAt(rig, button, kCentre));
        QVERIFY(!moveTo(rig, QPointF(600, 400), button));
        QVERIFY(!releaseAt(rig, button, QPointF(600, 400)));
    }
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.player.pauses, 0);

    // A left press during a right-button gesture goes on to the player.
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonPress, kCentre, Qt::LeftButton, Qt::LeftButton | Qt::RightButton));
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonRelease, kCentre, Qt::LeftButton, Qt::RightButton));
    QCOMPARE(rig.player.pauses, 0);
}

// A release that never arrives (a popup or a modal dialog took it) must not leave a gesture running.
void Test_VideoZoom::c26_lostRelease() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));

    // Moving with no button held ends a pan whose release went elsewhere.
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(rig.cursorSet());
    const qsizetype placed = rig.player.placements.count();
    QVERIFY(!moveTo(rig, QPointF(560, 400), Qt::NoButton));
    QVERIFY(!rig.cursorSet());
    QCOMPARE(rig.player.placements.count(), placed);
    // The next click is a click again: held back, then pauses on release.
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 1);

    // So does a press with no other button held.
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!rig.cursorSet());
    QVERIFY(!releaseAt(rig, Qt::RightButton, kCentre));

    // A right-click in the middle of a pan opens no menu (its popup would take the left release).
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(530, 400), Qt::LeftButton));
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonPress, kCentre, Qt::RightButton, Qt::LeftButton | Qt::RightButton));
    QVERIFY(mouseEvent(rig, QEvent::MouseButtonRelease, kCentre, Qt::RightButton, Qt::LeftButton));
    QVERIFY(moveTo(rig, QPointF(540, 400), Qt::LeftButton));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, QPointF(540, 400)));
    QCOMPARE(rig.player.pauses, 1);
}

// A quick second right press arrives as a double-click; it still starts a drag zoom.
void Test_VideoZoom::c27_rightDoubleClick() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonDblClick, kCentre, Qt::RightButton, Qt::RightButton));
    QVERIFY(moveTo(rig, QPointF(500, 350), Qt::RightButton));
    QVERIFY(rig.zoom.model().scale() > 1);
    QVERIFY(releaseAt(rig, Qt::RightButton, QPointF(500, 350)));
    // A left double-click is still left to the main window (fullscreen).
    QVERIFY(!doubleClickAt(rig, kCentre));
}

// Right button + wheel in the middle of a left press: a zoom, so neither the left release pauses nor the
// right one opens the context menu.
void Test_VideoZoom::c28_wheelZoomDuringPan() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    QVERIFY(pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonPress, kCentre, Qt::RightButton, Qt::LeftButton | Qt::RightButton));
    QVERIFY(wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::LeftButton | Qt::RightButton));
    COMPARE_NEAR(rig.zoom.model().scale(), 1.2);
    QVERIFY(!mouseEvent(rig, QEvent::MouseButtonRelease, kCentre, Qt::LeftButton, Qt::RightButton));
    QCOMPARE(rig.player.pauses, 0);
    QVERIFY(releaseAt(rig, Qt::RightButton, kCentre));
    QCOMPARE(rig.player.pauses, 0);
}

// The right release after a drag-and-wheel zoom goes missing: the next move without the button ends the
// gesture, so its cursor does not stay on over the video.
void Test_VideoZoom::c29_lostRightRelease() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(moveTo(rig, QPointF(500, 350), Qt::RightButton));
    QVERIFY(rig.cursorSet());
    QVERIFY(wheelAt(rig, QPointF(500, 350), QPoint(0, 120), QPoint(), Qt::RightButton));
    QVERIFY(!moveTo(rig, QPointF(520, 360), Qt::NoButton));
    QVERIFY(!rig.cursorSet());
    // A plain right-click afterwards is a right-click again.
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::RightButton, kCentre));
}

// The next file opens fitted whatever the last one was zoomed and panned to, and reports no zoom level.
void Test_VideoZoom::c31_newFileResetsZoom() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    rig.zoom.scrollLeft();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1, -1, 0);

    rig.newFile(QSize(1280, 720));
    QCOMPARE(rig.player.placements.last(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QVERIFY(rig.zoom.model().isFitted());
    COMPARE_NEAR(rig.zoom.model().scale(), 1000.0 / 1280); // 0.78125
    COMPARE_RECT(rig.zoom.model().videoRect(), 0, 118.75, 1000, 562.5);
    QCOMPARE(rig.scales.count(), 0);
    // It fits, so the left button is the player's again.
    QVERIFY(!pressAt(rig, Qt::LeftButton, kCentre));
    QVERIFY(!releaseAt(rig, Qt::LeftButton, kCentre));
    QCOMPARE(rig.player.pauses, 0);

    // Zooming again starts from the fit, centred, and reports.
    rig.zoom.zoomIn();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1000.0 / 1280 * 1.2, 0, 0); // 0.9375
    QCOMPARE(rig.scales.count(), 1);
    COMPARE_NEAR(rig.lastScale(), 0.9375);
}

// With expandImage the fit a file opens in is the enlarged one; a settings change reaches the player.
void Test_VideoZoom::c32_newFileResetsZoomExpand() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    COMPARE_LAST_PLACEMENT(rig, SHRINK, 1, 0, 0);
    rig.zoom.setSettings(expandSettings(true));
    COMPARE_LAST_PLACEMENT(rig, GROW, 1, 0, 0);
    QCOMPARE(rig.scales.count(), 0);

    rig.zoom.zoomIn(); // 1.875: 1200 px wide
    rig.zoom.scrollRight();
    COMPARE_LAST_PLACEMENT(rig, SCALED, 1.5625 * 1.2, 1, 0);
    QCOMPARE(rig.scales.count(), 1);

    rig.newFile(QSize(640, 360));
    QCOMPARE(rig.player.placements.last(), (VideoZoomPlacement{GROW, 1, 0, 0}));
    QVERIFY(rig.zoom.model().isFitted());
    COMPARE_NEAR(rig.zoom.model().scale(), 1.5625);
    QCOMPARE(rig.scales.count(), 1);
}

// A new file never inherits the last one's zoom, even at the same size (which the player announces
// unchanged), and a new size is fitted as it arrives.
void Test_VideoZoom::c33_newFileSameSize() {
    Rig rig(QSize(1280, 720));
    REQUIRE_DPR1(rig);
    QVERIFY(makePanReady(rig));
    rig.zoom.scrollRight();
    rig.zoom.onNewFile();
    rig.zoom.onVideoSizeChanged(QSize(1280, 720));
    QVERIFY(rig.zoom.model().isFitted());
    QCOMPARE(rig.player.placements.last(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    COMPARE_NEAR(rig.zoom.model().scale(), 1000.0 / 1280); // 0.78125
    QCOMPARE(rig.scales.count(), 0);

    rig.zoom.zoomIn();
    QCOMPARE(rig.scales.count(), 1);
    rig.zoom.onNewFile();
    rig.zoom.onVideoSizeChanged(QSize(1920, 1080));
    QVERIFY(rig.zoom.model().isFitted());
    QCOMPARE(rig.player.placements.last(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    COMPARE_NEAR(rig.zoom.model().scale(), 1000.0 / 1920); // 0.520833
    QCOMPARE(rig.scales.count(), 1);
}

// Between onNewFile() and the new file's size, the model has no video: zoom, pan and gestures are
// ignored rather than worked out against the previous file, and nothing carries over.
void Test_VideoZoom::c34_noZoomWhileLoading() {
    Rig rig(QSize(640, 360));
    REQUIRE_DPR1(rig);
    rig.zoom.onNewFile();
    QVERIFY(!rig.zoom.model().hasVideo());
    rig.player.placements.clear();
    rig.zoom.zoomOut();
    rig.zoom.zoomInCursor();
    rig.zoom.scrollDown();
    QVERIFY(!pressAt(rig, Qt::RightButton, kCentre));
    QVERIFY(!moveTo(rig, QPointF(500, 300), Qt::RightButton));
    QVERIFY(!wheelAt(rig, kCentre, QPoint(0, 120), QPoint(), Qt::RightButton));
    QVERIFY(!releaseAt(rig, Qt::RightButton, QPointF(500, 300)));
    QVERIFY(rig.player.placements.isEmpty());
    QCOMPARE(rig.scales.count(), 0);

    // The new file then opens fitted to its own size.
    rig.zoom.onVideoSizeChanged(QSize(3840, 2160));
    QVERIFY(rig.zoom.model().isFitted());
    COMPARE_NEAR(rig.zoom.model().scale(), 1000.0 / 3840); // 0.260417
    QCOMPARE(rig.player.placements.last(), (VideoZoomPlacement{SHRINK, 1, 0, 0}));
    QCOMPARE(rig.scales.count(), 0);
}

QTEST_MAIN(Test_VideoZoom)
#include "test_videozoom.moc"
