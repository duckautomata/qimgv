// The audio view as the user sees it. A scripted player stands in for the plugin and reports what mpv would;
// tags and covers come from the real reader and the fixtures in data/audio.
//
// Set QIMGV_AUDIOVIEW_RENDER_DIR to keep a PNG of every layout the test draws (run again with
// QT_SCALE_FACTOR=1.5 for the high-dpi set). The last test plays a fixture through the real plugin when the
// build has one.
#include <QtTest>
#include <QAccessible>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QLibrary>
#include <QPainter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>

#include "components/actionmanager/actionmanager.h"
#include "components/scriptmanager/scriptmanager.h"
#include "gui/customwidgets/mediaslider.h"
#include "gui/viewers/audioplayer.h"
#include "gui/viewers/audioview.h"
#include "settings.h"
#include "themestore.h"
#include "utils/actions.h"
#include "utils/inputmap.h"

namespace {

const QString kData = QStringLiteral(QIMGV_TEST_DATA_DIR "/audio/");

// Does what it is told and remembers what the view asked of it.
class FakePlayer : public AudioPlayer {
public:
    QString opened;
    int opens = 0, stops = 0, unloads = 0, volume = -1;
    bool paused = false, muted = false, loop = false;
    double seekedTo = -1, seekedBy = 0;

    bool open(QString const &file) override {
        opened = file;
        ++opens;
        paused = false;
        return true;
    }
    void stop() override {
        opened.clear();
        ++stops;
    }
    void unload() override {
        opened.clear();
        ++unloads;
    }
    void setPaused(bool mode) override { paused = mode; }
    bool isPaused() const override { return paused; }
    void seek(double seconds) override { seekedTo = seconds; }
    void seekRelative(double seconds) override { seekedBy += seconds; }
    void setVolume(int value) override { volume = value; }
    void setMuted(bool mode) override { muted = mode; }
    void setLoop(bool mode) override { loop = mode; }

    // What mpv reports once a file has started.
    void start(double duration, QVariantMap const &format = {}, QVariantMap const &tags = {}) {
        emit fileLoaded();
        emit durationChanged(duration);
        if(!format.isEmpty())
            emit formatChanged(format);
        if(!tags.isEmpty())
            emit metadataChanged(tags);
        emit pausedChanged(false);
    }
};

// Stands behind the view in place of ViewerWidget and counts what reaches it.
class EventSink : public QWidget {
public:
    EventSink() { setMouseTracking(true); }
    int presses = 0, releases = 0, doubleClicks = 0, moves = 0;

protected:
    void mouseMoveEvent(QMouseEvent *event) override {
        ++moves;
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override {
        ++presses;
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        ++releases;
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        ++doubleClicks;
        event->accept();
    }
};

QVariantMap flacFormat() {
    return {{"codec", "flac"},        {"samplerate", 44100}, {"channels", 2}, {"channel-layout", "stereo"},
            {"sample-format", "s16"}, {"bitrate", 912000}};
}

// Lets the metadata job finish and its result arrive.
void settle() {
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

// A cover with enough going on to judge the rounding, the shadow and the backdrop by.
QImage artwork() {
    QImage image(600, 600, QImage::Format_RGB32);
    QPainter p(&image);
    QLinearGradient sky(0, 0, 600, 600);
    sky.setColorAt(0, QColor(0x2b, 0x1d, 0x6b));
    sky.setColorAt(0.55, QColor(0xc8, 0x3e, 0x6c));
    sky.setColorAt(1, QColor(0xf6, 0xb2, 0x5a));
    p.fillRect(image.rect(), sky);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xff, 0xe8, 0xa8));
    p.drawEllipse(QPointF(390, 250), 90, 90);
    p.setBrush(QColor(0x14, 0x10, 0x2a));
    QPolygonF hills;
    hills << QPointF(0, 600) << QPointF(0, 430) << QPointF(160, 360) << QPointF(300, 440) << QPointF(450, 330)
          << QPointF(600, 410) << QPointF(600, 600);
    p.drawPolygon(hills);
    return image;
}

// Laid out and on (offscreen) screen, as in the app: a hidden widget gets its resize event only when shown.
void present(QWidget &widget, QSize size) {
    widget.resize(size);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
}

QList<QWidget *> controls(AudioView &view) {
    QList<QWidget *> list;
    for(QWidget *w : view.findChildren<QWidget *>()) {
        if(w->objectName() == "AudioCard" || w->objectName().isEmpty() || !w->isVisibleTo(&view))
            continue;
        list << w;
    }
    return list;
}

QRect geometryIn(AudioView &view, QWidget *w) {
    return QRect(w->mapTo(&view, QPoint(0, 0)), w->size());
}

// What the time labels need for `text`: every digit as wide as the widest one.
int tabularWidth(QLabel *label, QString const &text) {
    QFontMetricsF fm(label->font());
    qreal digit = 0;
    for(char c = '0'; c <= '9'; ++c)
        digit = std::max(digit, fm.horizontalAdvance(QChar(c)));
    qreal width = 0;
    for(QChar c : text)
        width += c.isDigit() ? digit : fm.horizontalAdvance(c);
    return static_cast<int>(std::ceil(width));
}

qreal contrastOf(QColor const &a, QColor const &b) {
    auto channel = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    auto luminance = [&](QColor const &c) {
        return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
    };
    qreal la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void sendMove(QWidget *w, QPoint pos, Qt::MouseButtons buttons) {
    QMouseEvent move(QEvent::MouseMove, pos, w->mapToGlobal(pos), Qt::NoButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &move);
}

// The wheel at `pos` in `w`. Spontaneous, as from the mouse: Qt passes only those on to the parent when a
// widget ignores them. True when someone took it.
bool sendWheel(QWidget *w, QPointF pos, int delta = 120) {
    QWheelEvent wheel(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QSpontaneKeyEvent::setSpontaneous(&wheel);
    qApp->notify(w, &wheel);
    return wheel.isAccepted();
}

bool sendWheel(QWidget *w, int delta = 120) {
    return sendWheel(w, QRectF(w->rect()).center(), delta);
}

} // namespace

class TestAudioView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void everyChildIsNoFocus();
    void showsTagsAndCover();
    void fallsBackToPlayerTags();
    void fallsBackToFileName();
    void namesTheFormat_data();
    void namesTheFormat();
    void formatsTime();
    void keepsTheSessionsVolumeAndMute();
    void seekBarSeeks();
    void wheelSeeksAlongTheRow();
    void dragEndsWithoutItsRelease();
    void reportsFailure();
    void forwardsTheEnd();
    void aPauseHoldsAtTheEnd();
    void closedIsEmpty();
    void unloadDoesNotWait();
    void waitsForTheTags();
    void explainsAMissingPlugin();
    void toolTipsNameAKey();
    void accessible();
    void fillsStandOut();
    void backdropKeepsTheOpacity();
    void layoutHolds_data();
    void layoutHolds();
    void staysClearOfThePanel();
    void unusedMouseEventsPropagate();
    void renders();
    void playsThroughThePlugin();

private:
    FakePlayer *attach(AudioView &view) {
        auto player = new FakePlayer;
        view.setPlayerForTesting(player);
        return player;
    }
    // A copy of a tagless fixture named `name`, with an optional sidecar cover.
    QString trackNamed(QString const &name, bool withCover);
    void setScheme(ColorSchemes scheme);
    void save(AudioView &view, QString const &name);

    QTemporaryDir tmp;
    QString renderDir;
};

void TestAudioView::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-test");
    // In main.cpp's order: the action manager reads the others.
    inputMap = InputMap::getInstance();
    appActions = Actions::getInstance();
    settings = Settings::getInstance();
    scriptManager = ScriptManager::getInstance();
    actionManager = ActionManager::getInstance();
    QVERIFY(tmp.isValid());
    renderDir = qEnvironmentVariable("QIMGV_AUDIOVIEW_RENDER_DIR");
    if(!renderDir.isEmpty())
        QVERIFY(QDir().mkpath(renderDir));
}

void TestAudioView::init() {
    setScheme(COLORS_DARK);
    settings->setVolume(100);
    settings->setAudioBackdrop(true);
    settings->setBackgroundOpacity(1.0);
    settings->setPanelEnabled(false);
    settings->sendChangeNotification();
}

void TestAudioView::cleanup() {
    settle();
}

QString TestAudioView::trackNamed(QString const &name, bool withCover) {
    QDir dir(tmp.path() + "/" + name);
    dir.mkpath(".");
    QString path = dir.filePath(name + ".mp3");
    if(!QFile::exists(path))
        QFile::copy(kData + "mp3-bare.mp3", path);
    if(withCover)
        artwork().save(dir.filePath("cover.jpg"), "JPG", 92);
    return path;
}

void TestAudioView::setScheme(ColorSchemes scheme) {
    settings->setColorScheme(ThemeStore::colorScheme(scheme));
    settings->sendChangeNotification();
}

void TestAudioView::save(AudioView &view, QString const &name) {
    QImage image = view.grab().toImage();
    QVERIFY(!image.isNull());
    if(renderDir.isEmpty())
        return;
    QString scale = QString::number(view.devicePixelRatioF());
    QVERIFY(image.save(QStringLiteral("%1/%2@%3x.png").arg(renderDir, name, scale)));
}

void TestAudioView::everyChildIsNoFocus() {
    AudioView view;
    attach(view);
    QCOMPARE(view.focusPolicy(), Qt::NoFocus);
    for(QWidget *w : view.findChildren<QWidget *>())
        QVERIFY2(w->focusPolicy() == Qt::NoFocus, qPrintable(w->objectName() + " can take focus"));
}

void TestAudioView::showsTagsAndCover() {
    AudioView view;
    attach(view);
    present(view, QSize(800, 700));
    QVERIFY(view.open(kData + "flac-picture.flac"));
    settle();
    QCOMPARE(view.findChild<QLabel *>("AudioTitle")->text(), QString("Fixture"));
    QCOMPARE(view.findChild<QLabel *>("AudioSubtitle")->text(), QString("qimgv · Test Album"));
    // The fixture's cover is solid red; the cover sits in the middle of the column, above the card.
    QImage image = view.grab().toImage();
    QWidget *card = view.findChild<QWidget *>("AudioCard");
    QPoint coverCentre(card->geometry().center().x(), card->geometry().top() / 2);
    QColor pixel = image.pixelColor(coverCentre * view.devicePixelRatioF());
    QVERIFY2(pixel.red() > 200 && pixel.green() < 60 && pixel.blue() < 60, qPrintable(pixel.name()));
}

void TestAudioView::fallsBackToPlayerTags() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    settle();
    player->start(10, {}, {{"title", "From mpv"}, {"artist", "Somebody"}, {"album", "Somewhere"}});
    QCOMPARE(view.findChild<QLabel *>("AudioTitle")->text(), QString("From mpv"));
    QCOMPARE(view.findChild<QLabel *>("AudioSubtitle")->text(), QString("Somebody · Somewhere"));

    // The reader's tags win over mpv's whenever it found any.
    QVERIFY(view.open(kData + "mp3-id3v24.mp3"));
    player->start(10, {}, {{"title", "From mpv"}});
    settle();
    QTRY_COMPARE(view.findChild<QLabel *>("AudioTitle")->text(), QString("Fixture"));
}

void TestAudioView::fallsBackToFileName() {
    AudioView view;
    attach(view);
    present(view, QSize(800, 600));
    QString path = trackNamed("Just a file.name", false);
    QVERIFY(view.open(path));
    settle();
    QCOMPARE(view.findChild<QLabel *>("AudioTitle")->text(), QString("Just a file.name"));
    QVERIFY(!view.findChild<QLabel *>("AudioSubtitle")->isVisible());
}

void TestAudioView::namesTheFormat_data() {
    QTest::addColumn<QVariantMap>("format");
    QTest::addColumn<QString>("expected");
    QTest::newRow("flac") << flacFormat() << "FLAC · 44.1 kHz · 16-bit · Stereo · 912 kbps";
    // A 32-bit decoder output says nothing certain about the source's depth.
    QTest::newRow("hi-res flac") << QVariantMap{{"codec", "flac"},
                                                {"samplerate", 96000},
                                                {"sample-format", "s32"},
                                                {"channels", 2}}
                                 << "FLAC · 96 kHz · Stereo";
    QTest::newRow("pcm") << QVariantMap{{"codec", "pcm_s24le"}, {"samplerate", 48000}, {"channels", 1}}
                         << "PCM · 48 kHz · 24-bit · Mono";
    QTest::newRow("mp3") << QVariantMap{{"codec", "mp3"},
                                        {"samplerate", 22050},
                                        {"channels", 2},
                                        {"sample-format", "floatp"},
                                        {"bitrate", 128000}}
                         << "MP3 · 22.05 kHz · Stereo · 128 kbps";
    QTest::newRow("surround") << QVariantMap{{"codec", "eac3"}, {"channels", 6}, {"channel-layout", "5.1(side)"}}
                              << "E-AC-3 · 5.1";
    QTest::newRow("odd layout") << QVariantMap{{"codec", "opus"}, {"channels", 3}, {"channel-layout", "3.0"}}
                                << "Opus · 3.0";
    QTest::newRow("unknown codec") << QVariantMap{{"codec", "newthing"}, {"channels", 4}} << "NEWTHING · 4 channels";
    // Near-silence measures a few hundred bits per second: no "0 kbps".
    QTest::newRow("tiny bitrate") << QVariantMap{{"codec", "vorbis"}, {"channels", 1}, {"bitrate", 320}}
                                  << "Vorbis · Mono";
    QTest::newRow("nothing") << QVariantMap{{"codec", ""}} << "";
}

void TestAudioView::namesTheFormat() {
    QFETCH(QVariantMap, format);
    QFETCH(QString, expected);
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(1000, 700));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    settle();
    player->start(10, format);
    auto details = view.findChild<QLabel *>("AudioDetails");
    QCOMPARE(details->isVisibleTo(&view) ? details->text() : QString(), expected);
}

void TestAudioView::formatsTime() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    auto elapsed = view.findChild<QLabel *>("AudioElapsed");
    auto total = view.findChild<QLabel *>("AudioDuration");
    player->start(200);
    emit player->positionChanged(5.9);
    QCOMPARE(total->text(), QString("3:20"));
    QCOMPARE(elapsed->text(), QString("0:05"));
    int shortWidth = total->width();
    player->start(3725);
    emit player->positionChanged(61.4);
    QCOMPARE(total->text(), QString("1:02:05"));
    QCOMPARE(elapsed->text(), QString("0:01:01"));
    QVERIFY(total->width() > shortWidth);
    QCOMPARE(elapsed->width(), total->width());
    // Audiobooks run to ten hours and more. A label one digit short would show 11:22:33 as "1:22:33".
    player->start(45296);
    emit player->positionChanged(40953);
    QCOMPARE(total->text(), QString("12:34:56"));
    QCOMPARE(elapsed->text(), QString("11:22:33"));
    QVERIFY(elapsed->width() >= tabularWidth(elapsed, elapsed->text()));
    QVERIFY(total->width() >= tabularWidth(total, total->text()));
    QCOMPARE(elapsed->width(), total->width());
}

void TestAudioView::keepsTheSessionsVolumeAndMute() {
    settings->setVolume(40);
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    QCOMPARE(player->volume, 40);
    QVERIFY(!player->muted);

    view.toggleMute();
    QVERIFY(view.isMuted());
    QVERIFY(player->muted);
    // Mute lasts for the session: the next track starts muted too.
    player->muted = false;
    QVERIFY(view.open(kData + "flac-picture.flac"));
    QVERIFY(player->muted);

    view.volumeDown();
    QCOMPARE(player->volume, 35);
    QCOMPARE(settings->volume(), 35);
    QVERIFY(view.isMuted());
    // Turning it up means wanting to hear it.
    view.volumeUp();
    QCOMPARE(settings->volume(), 40);
    QVERIFY(!view.isMuted());

    auto slider = view.findChild<MediaSlider *>("AudioVolume");
    QVERIFY(slider->isVisible());
    QTest::mouseClick(slider, Qt::LeftButton, {}, QPoint(slider->width() - 2, slider->height() / 2));
    QCOMPARE(settings->volume(), 100);
    QCOMPARE(player->volume, 100);
    for(int i = 0; i < 30; ++i)
        view.volumeUp();
    QCOMPARE(settings->volume(), 100);
}

void TestAudioView::seekBarSeeks() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    player->start(100);
    auto bar = view.findChild<MediaSlider *>("AudioSeekBar");
    QTest::mouseClick(bar, Qt::LeftButton, {}, QPoint(bar->width() / 2, bar->height() / 2));
    QVERIFY2(std::abs(player->seekedTo - 50) < 2, qPrintable(QString::number(player->seekedTo)));
    QCOMPARE(view.findChild<QLabel *>("AudioElapsed")->text(), QString("0:%1").arg(qRound(player->seekedTo)));

    // One wheel notch is five seconds, and the wheel stays with the bar instead of changing the file.
    emit player->positionChanged(50);
    QWheelEvent wheel(QPointF(bar->width() / 2, 5), bar->mapToGlobal(QPointF(bar->width() / 2, 5)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(bar, &wheel);
    QVERIFY(wheel.isAccepted());
    QCOMPARE(player->seekedTo, 55.0);

    view.seekRelative(-10);
    QCOMPARE(player->seekedBy, -10.0);
    view.seek(500);
    QCOMPARE(player->seekedTo, 100.0);
}

void TestAudioView::reportsFailure() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QSignalSpy failed(&view, &AudioView::playbackFailed);
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    settle();
    emit player->failed("unrecognized file format");
    QCOMPARE(failed.count(), 1);
    auto error = view.findChild<QLabel *>("AudioError");
    QVERIFY(error->isVisibleTo(&view));
    QVERIFY(error->text().contains("unrecognized file format"));
    QVERIFY(!view.findChild<QWidget *>("AudioPlayButton")->isEnabled());
    QVERIFY(!view.findChild<QWidget *>("AudioSeekBar")->isEnabled());
    QVERIFY(view.findChild<QWidget *>("AudioNextButton")->isEnabled());
    // Nothing to resume.
    view.togglePaused();
    QVERIFY(view.isPaused());

    // The next file starts clean.
    QVERIFY(view.open(kData + "flac-picture.flac"));
    settle();
    QVERIFY(!error->isVisibleTo(&view));
    QVERIFY(view.findChild<QWidget *>("AudioPlayButton")->isEnabled());
}

void TestAudioView::forwardsTheEnd() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QSignalSpy finished(&view, &AudioView::playbackFinished);
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    player->start(30);
    QVERIFY(!view.isPaused());
    emit player->positionChanged(29.7);
    emit player->playbackFinished();
    QCOMPARE(finished.count(), 1);
    QVERIFY(view.isPaused());
    QCOMPARE(view.findChild<MediaSlider *>("AudioSeekBar")->value(), 30.0);
    // Play starts it over; the plugin restarts a finished file when unpaused.
    view.togglePaused();
    QVERIFY(!player->paused);

    view.setLoop(true);
    QVERIFY(player->loop);
    view.stop();
    QCOMPARE(player->stops, 1);
    QVERIFY(view.currentFile().isEmpty());
}

void TestAudioView::explainsAMissingPlugin() {
    // The test is built without a plugin file name to look for (see CMakeLists.txt).
    AudioView view;
    present(view, QSize(640, 480));
    QVERIFY(!view.open(kData + "mp3-bare.mp3"));
    auto message = view.findChild<QLabel *>("AudioPluginError");
    QVERIFY(message->isVisibleTo(&view));
    QVERIFY(message->text().contains("Could not load"));
    QVERIFY(!view.findChild<QWidget *>("AudioCard")->isVisibleTo(&view));
    save(view, "plugin-missing-640x480");
}

void TestAudioView::layoutHolds_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("cover");
    // The first three are below what the layout is made for, but a window can be that small.
    for(QSize size : {QSize(220, 160), QSize(260, 180), QSize(400, 150), QSize(320, 240), QSize(360, 640),
                      QSize(640, 480), QSize(900, 360), QSize(1280, 720), QSize(1920, 1080), QSize(3840, 2160)}) {
        for(bool cover : {true, false}) {
            QString name =
                QStringLiteral("%1x%2 %3").arg(size.width()).arg(size.height()).arg(cover ? "cover" : "none");
            QTest::newRow(qPrintable(name)) << size << cover;
        }
    }
}

void TestAudioView::layoutHolds() {
    QFETCH(QSize, size);
    QFETCH(bool, cover);
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, size);
    QString longName = "An exceedingly long track title that cannot possibly fit on one line of any reasonable window";
    QVERIFY(view.open(trackNamed(cover ? "Layout with cover" : "Layout without cover", cover)));
    settle();
    player->start(4000, flacFormat(),
                  {{"title", longName}, {"artist", "Someone with a long name"}, {"album", "And an album too"}});

    QList<QWidget *> parts = controls(view);
    // Title, times, seek bar, previous, play and next, whatever the size; the rest where there is room.
    QVERIFY(parts.size() >= (size.width() >= 320 ? 9 : 7));
    QRect area = view.rect();
    for(QWidget *a : parts) {
        QRect ra = geometryIn(view, a);
        QVERIFY2(area.contains(ra), qPrintable(a->objectName() + " leaves the view"));
        for(QWidget *b : parts) {
            if(a != b && a->parentWidget() == b->parentWidget())
                QVERIFY2(!ra.intersects(geometryIn(view, b)),
                         qPrintable(a->objectName() + " overlaps " + b->objectName()));
        }
    }
    auto title = view.findChild<QLabel *>("AudioTitle");
    QVERIFY(title->text().endsWith(QChar(0x2026)));
    QCOMPARE(title->toolTip(), longName);
    QVERIFY(title->fontMetrics().horizontalAdvance(title->text()) <= title->width());
    QWidget *card = view.findChild<QWidget *>("AudioCard");
    QVERIFY(area.contains(card->geometry()));
    // FloatingMessage's strip at the bottom stays free whenever the window is tall enough to spare it.
    if(size.height() >= 480)
        QVERIFY(card->geometry().bottom() < size.height() - 60);
}

void TestAudioView::staysClearOfThePanel() {
    settings->setPanelEnabled(true);
    settings->setPanelPinned(false);
    settings->setPanelFullscreenOnly(false);
    settings->setPanelPosition(PANEL_BOTTOM);
    settings->sendChangeNotification();
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(1280, 720));
    QVERIFY(view.open(trackNamed("Panel", true)));
    settle();
    player->start(200, flacFormat());
    int panelTop = 720 - settings->panelPreviewsSize();
    QVERIFY(view.findChild<QWidget *>("AudioCard")->geometry().bottom() < panelTop);
    save(view, "dark-1280x720-panel-bottom");

    // Fullscreen-only panels leave the windowed view alone.
    settings->setPanelFullscreenOnly(true);
    settings->sendChangeNotification();
    QVERIFY(view.findChild<QWidget *>("AudioCard")->geometry().bottom() >= panelTop);
    view.onFullscreenModeChanged(true);
    QVERIFY(view.findChild<QWidget *>("AudioCard")->geometry().bottom() < panelTop);
}

void TestAudioView::unusedMouseEventsPropagate() {
    EventSink sink;
    sink.resize(800, 600);
    auto view = new AudioView(&sink);
    FakePlayer *player = attach(*view);
    view->resize(sink.size());
    sink.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sink));
    QVERIFY(view->open(kData + "flac-picture.flac"));
    settle();
    player->start(60, flacFormat());

    // The background, the cover and the text: the context menu and double-click fullscreen work there.
    auto title = view->findChild<QLabel *>("AudioTitle");
    QTest::mousePress(view, Qt::RightButton, {}, QPoint(5, 5));
    QTest::mouseRelease(view, Qt::RightButton, {}, QPoint(5, 5));
    QTest::mousePress(title, Qt::RightButton, {}, title->rect().center());
    QTest::mouseRelease(title, Qt::RightButton, {}, title->rect().center());
    QCOMPARE(sink.presses, 2);
    QTest::mouseDClick(view, Qt::LeftButton, {}, QPoint(5, 5));
    QVERIFY(sink.doubleClicks >= 1);
    // Hover moves too, over the card's text and the bars: the cursor and the panel's trigger run on them.
    auto bar = view->findChild<QWidget *>("AudioSeekBar");
    for(QWidget *w :
        {static_cast<QWidget *>(view), static_cast<QWidget *>(title), bar, view->findChild<QWidget *>("AudioCard"),
         view->findChild<QWidget *>("AudioElapsed"), view->findChild<QWidget *>("AudioPlayButton"),
         view->findChild<QWidget *>("AudioNextButton"), view->findChild<QWidget *>("AudioVolume")}) {
        int moves = sink.moves;
        QTest::mouseMove(w, w->rect().center() + QPoint(1, 0));
        QTest::mouseMove(w, w->rect().center());
        QVERIFY2(sink.moves > moves, qPrintable(w->objectName() + " swallows hover moves"));
        // QTest's moves carry the last button it pressed. Qt drops a move with none at the first widget that
        // does not track the mouse.
        moves = sink.moves;
        sendMove(w, w->rect().center(), Qt::NoButton);
        QVERIFY2(sink.moves > moves, qPrintable(w->objectName() + " does not pass on hover moves"));
    }
    // Wheel = previous/next file, except over the seek bar and the volume, which use it themselves.
    QWheelEvent wheel(QPointF(5, 5), view->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view, &wheel);
    QVERIFY(!wheel.isAccepted());

    // A button keeps its own clicks.
    int before = sink.presses;
    auto next = view->findChild<QWidget *>("AudioNextButton");
    QTest::mousePress(next, Qt::LeftButton, {}, next->rect().center());
    QTest::mouseRelease(next, Qt::LeftButton, {}, next->rect().center());
    QCOMPARE(sink.presses, before);
    // But only those of the left button. The context menu opens on the right one's release, and the thumb
    // buttons go to the previous and next file, over the buttons as anywhere else.
    for(QWidget *button : {next, view->findChild<QWidget *>("AudioPlayButton")}) {
        for(Qt::MouseButton other : {Qt::RightButton, Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton}) {
            int presses = sink.presses, releases = sink.releases;
            QTest::mousePress(button, other, {}, button->rect().center());
            QVERIFY2(!button->property("pressed").toBool(), qPrintable(button->objectName()));
            QTest::mouseRelease(button, other, {}, button->rect().center());
            QVERIFY2(sink.presses == presses + 1 && sink.releases == releases + 1,
                     qPrintable(button->objectName() + " keeps button " + QString::number(other)));
        }
    }
}

void TestAudioView::wheelSeeksAlongTheRow() {
    EventSink sink;
    sink.resize(800, 600);
    auto view = new AudioView(&sink);
    FakePlayer *player = attach(*view);
    view->resize(sink.size());
    sink.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sink));
    QVERIFY(view->open(kData + "mp3-bare.mp3"));
    settle();
    player->start(100);
    emit player->positionChanged(50);
    // A little off the bar, or over the times beside it, the wheel still seeks instead of changing the file.
    auto elapsed = view->findChild<QWidget *>("AudioElapsed");
    QVERIFY(sendWheel(elapsed));
    QCOMPARE(player->seekedTo, 55.0);
    QVERIFY(sendWheel(view->findChild<QWidget *>("AudioDuration"), -120));
    QCOMPARE(player->seekedTo, 50.0);
    auto bar = view->findChild<QWidget *>("AudioSeekBar");
    auto card = view->findChild<QWidget *>("AudioCard");
    QVERIFY(sendWheel(card, QPointF(bar->geometry().center().x(), bar->geometry().top() - 3)));
    QCOMPARE(player->seekedTo, 55.0);
    // Elsewhere on the card it is the viewer's.
    QVERIFY(!sendWheel(view->findChild<QWidget *>("AudioTitle")));
    QVERIFY(!sendWheel(view->findChild<QWidget *>("AudioNextButton")));
    QCOMPARE(player->seekedTo, 55.0);
    // So it is beside a bar with nothing to seek in.
    emit player->failed("unrecognized file format");
    QVERIFY(!sendWheel(elapsed));
}

void TestAudioView::dragEndsWithoutItsRelease() {
    EventSink sink;
    sink.resize(800, 600);
    auto view = new AudioView(&sink);
    FakePlayer *player = attach(*view);
    view->resize(sink.size());
    sink.show();
    QVERIFY(QTest::qWaitForWindowExposed(&sink));
    QVERIFY(view->open(kData + "mp3-bare.mp3"));
    settle();
    player->start(100);
    auto bar = view->findChild<MediaSlider *>("AudioSeekBar");
    auto elapsed = view->findChild<QLabel *>("AudioElapsed");
    QPoint middle(bar->width() / 2, bar->height() / 2);
    QPoint further(bar->width() * 4 / 5, bar->height() / 2);

    // The release went elsewhere (another window), so the next move comes with no button held: a hover. It
    // must not seek, and the bar follows the player again.
    QTest::mousePress(bar, Qt::LeftButton, {}, middle);
    QVERIFY(bar->isDragging());
    double seeked = player->seekedTo;
    QVERIFY(std::abs(seeked - 50) < 2);
    sendMove(bar, further, Qt::NoButton);
    QVERIFY(!bar->isDragging());
    QCOMPARE(player->seekedTo, seeked);
    emit player->positionChanged(40);
    QCOMPARE(bar->value(), 40.0);
    QCOMPARE(elapsed->text(), QString("0:40"));
    QTest::mouseRelease(bar, Qt::LeftButton, {}, middle);
    QCOMPARE(player->seekedTo, seeked);

    // The right button mid-drag: it opens the context menu, a popup that takes the left button's release.
    // The drag ends where it was, and the press goes on to the viewer.
    int presses = sink.presses;
    QTest::mousePress(bar, Qt::LeftButton, {}, middle);
    sendMove(bar, further, Qt::LeftButton);
    QVERIFY(bar->isDragging());
    QTest::mousePress(bar, Qt::RightButton, {}, further);
    QVERIFY(!bar->isDragging());
    QCOMPARE(sink.presses, presses + 1);
    QVERIFY2(std::abs(player->seekedTo - 80) < 2, qPrintable(QString::number(player->seekedTo)));
    QTest::mouseRelease(bar, Qt::RightButton, {}, further);
    QTest::mouseRelease(bar, Qt::LeftButton, {}, further);
    emit player->positionChanged(30);
    QCOMPARE(bar->value(), 30.0);

    // Hidden mid-drag, by the folder view.
    QTest::mousePress(bar, Qt::LeftButton, {}, middle);
    QVERIFY(bar->isDragging());
    view->hide();
    QVERIFY(!bar->isDragging());
    view->show();
    QTest::mouseRelease(bar, Qt::LeftButton, {}, middle);
    emit player->positionChanged(20);
    QCOMPARE(bar->value(), 20.0);

    // Likewise for the volume: hovering over it afterwards does not set it.
    auto volume = view->findChild<MediaSlider *>("AudioVolume");
    QPoint half(volume->width() / 2, volume->height() / 2);
    QTest::mousePress(volume, Qt::LeftButton, {}, half);
    int set = settings->volume();
    QVERIFY(std::abs(set - 50) <= 2);
    sendMove(volume, QPoint(volume->width() - 1, volume->height() / 2), Qt::NoButton);
    QVERIFY(!volume->isDragging());
    QCOMPARE(settings->volume(), set);
    QTest::mouseRelease(volume, Qt::LeftButton, {}, half);
}

void TestAudioView::aPauseHoldsAtTheEnd() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QSignalSpy finished(&view, &AudioView::playbackFinished);
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    player->start(30);
    emit player->positionChanged(29.85);
    // Paused in the last moment: mpv plays out its buffer and stops at the end all the same.
    view.togglePaused();
    QVERIFY(player->paused);
    emit player->pausedChanged(true);
    emit player->playbackFinished();
    QCOMPARE(finished.count(), 0);
    QVERIFY(view.isPaused());
    QCOMPARE(view.findChild<MediaSlider *>("AudioSeekBar")->value(), 30.0);
    // Play again: from here on the end is the end.
    view.togglePaused();
    QVERIFY(!player->paused);
    emit player->pausedChanged(false);
    emit player->playbackFinished();
    QCOMPARE(finished.count(), 1);

    // The player's own pause, the one keep-open makes at the end, is not the user's.
    QVERIFY(view.open(kData + "flac-picture.flac"));
    player->start(30);
    emit player->pausedChanged(true);
    emit player->playbackFinished();
    QCOMPARE(finished.count(), 2);
    // Nor does a pause carry over to the next track.
    view.togglePaused();
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    player->start(30);
    emit player->playbackFinished();
    QCOMPARE(finished.count(), 3);
}

// Moving on to another file must not wait for mpv to close this one; only stop() does, for renames and
// deletes.
void TestAudioView::unloadDoesNotWait() {
    AudioView view;
    FakePlayer *player = attach(view);
    QVERIFY(view.open(kData + "flac-picture.flac"));
    view.unload();
    QCOMPARE(player->unloads, 1);
    QCOMPARE(player->stops, 0);
    QVERIFY(view.currentFile().isEmpty());
}

void TestAudioView::closedIsEmpty() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "flac-picture.flac"));
    settle();
    player->start(60, flacFormat());
    auto card = view.findChild<QWidget *>("AudioCard");
    QVERIFY(card->isVisibleTo(&view));
    // Closed under the user, e.g. the file was deleted: like a closed image, nothing is left to control.
    view.stop();
    QVERIFY(!card->isVisibleTo(&view));
    QImage image = view.grab().toImage();
    QPoint centre = view.rect().center() * view.devicePixelRatioF();
    QCOMPARE(image.pixelColor(centre).rgb(), settings->colorScheme().background.rgb());
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    QVERIFY(card->isVisibleTo(&view));
}

void TestAudioView::waitsForTheTags() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    auto title = view.findChild<QLabel *>("AudioTitle");
    // Opened from an image: nothing in the column until the tags are in, rather than the file name and a
    // placeholder tile that they replace a moment later.
    QVERIFY(view.open(kData + "flac-picture.flac"));
    QVERIFY(title->text().isEmpty());
    QImage before = view.grab().toImage();
    settle();
    QCOMPARE(title->text(), QString("Fixture"));
    QWidget *card = view.findChild<QWidget *>("AudioCard");
    QPoint coverCentre(card->geometry().center().x(), card->geometry().top() / 2);
    QCOMPARE(before.pixelColor(coverCentre * view.devicePixelRatioF()).rgb(), settings->colorScheme().background.rgb());

    // The format line comes from mpv a little later. Its slot is kept meanwhile, so the title stays put.
    QVERIFY(view.open(trackNamed("Untagged", false)));
    settle();
    int titleY = title->y();
    player->start(60, flacFormat());
    QCOMPARE(title->y(), titleY);
    QVERIFY(view.findChild<QLabel *>("AudioDetails")->isVisible());
}

void TestAudioView::toolTipsNameAKey() {
    // In alphabetical order, which is ActionManager's, the media key and the thumb button would come first.
    actionManager->removeAllShortcuts("nextImage");
    for(QString keys : {"MediaNext", "Right", "WheelDown", "XButton2", "Ctrl+Right"})
        actionManager->addShortcut(keys, "nextImage");
    actionManager->removeAllShortcuts("prevImage");
    actionManager->addShortcut("XButton1", "prevImage");
    AudioView view;
    attach(view);
    QCOMPARE(view.findChild<QWidget *>("AudioNextButton")->toolTip(), QString("Next (Right)"));
    // Better a mouse button than nothing.
    QCOMPARE(view.findChild<QWidget *>("AudioPrevButton")->toolTip(), QString("Previous (XButton1)"));
    actionManager->resetDefaults("nextImage");
    actionManager->resetDefaults("prevImage");
}

void TestAudioView::accessible() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(kData + "mp3-bare.mp3"));
    settle();
    player->start(100);
    emit player->positionChanged(50);

    // The buttons are buttons, named by their labels rather than by the stylesheet's hook.
    QStringList names;
    for(QString name :
        {"AudioModeButton", "AudioPrevButton", "AudioPlayButton", "AudioNextButton", "AudioMuteButton"}) {
        QAccessibleInterface *button = QAccessible::queryAccessibleInterface(view.findChild<QWidget *>(name));
        QVERIFY(button);
        QCOMPARE(button->role(), QAccessible::Button);
        QVERIFY(button->actionInterface());
        QVERIFY(button->actionInterface()->actionNames().contains(QAccessibleActionInterface::pressAction()));
        names << button->text(QAccessible::Name);
    }
    QCOMPARE(names, QStringList({"Playback: Single file", "Previous", "Pause", "Next", "Mute"}));
    // Pressed, it does what a click does: the action, which ViewerWidget would carry out.
    QSignalSpy toggleMute(actionManager, &ActionManager::toggleMute);
    auto mute = QAccessible::queryAccessibleInterface(view.findChild<QWidget *>("AudioMuteButton"));
    mute->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    QCOMPARE(toggleMute.count(), 1);
    view.toggleMute();
    QCOMPARE(mute->text(QAccessible::Name), QString("Unmute"));

    // The sliders are sliders, with a value to read and set.
    auto seek = QAccessible::queryAccessibleInterface(view.findChild<QWidget *>("AudioSeekBar"));
    QVERIFY(seek);
    QCOMPARE(seek->role(), QAccessible::Slider);
    QCOMPARE(seek->text(QAccessible::Name), QString("Position"));
    QCOMPARE(seek->text(QAccessible::Value), QString("0:50"));
    QAccessibleValueInterface *value = seek->valueInterface();
    QVERIFY(value);
    QCOMPARE(value->currentValue().toDouble(), 50.0);
    QCOMPARE(value->maximumValue().toDouble(), 100.0);
    value->setCurrentValue(20.0);
    QCOMPARE(player->seekedTo, 20.0);
    auto volume = QAccessible::queryAccessibleInterface(view.findChild<QWidget *>("AudioVolume"));
    QCOMPARE(volume->role(), QAccessible::Slider);
    QCOMPARE(volume->text(QAccessible::Name), QString("Volume"));
    QCOMPARE(volume->text(QAccessible::Value), QString("Volume: 100%"));
}

void TestAudioView::fillsStandOut() {
    for(ColorSchemes scheme : {COLORS_DARK, COLORS_LIGHT, COLORS_BLACK}) {
        setScheme(scheme);
        settings->setVolume(50);
        AudioView view;
        FakePlayer *player = attach(view);
        present(view, QSize(800, 600));
        QVERIFY(view.open(kData + "mp3-bare.mp3"));
        settle();
        player->start(100);
        emit player->positionChanged(50);
        // Played from unplayed, and the volume from the rest, at 3:1 or better (less a little for rounding).
        for(QString name : {"AudioSeekBar", "AudioVolume"}) {
            QImage image = view.findChild<MediaSlider *>(name)->grab().toImage();
            int y = image.height() / 2;
            QColor fill = image.pixelColor(image.width() / 5, y);
            QColor track = image.pixelColor(image.width() * 4 / 5, y);
            qreal ratio = contrastOf(fill, track);
            QVERIFY2(ratio >= 2.95, qPrintable(QStringLiteral("%1, scheme %2: %3 on %4 is %5:1")
                                                   .arg(name)
                                                   .arg(scheme)
                                                   .arg(fill.name(), track.name())
                                                   .arg(ratio)));
        }
    }
}

void TestAudioView::backdropKeepsTheOpacity() {
    AudioView view;
    FakePlayer *player = attach(view);
    present(view, QSize(800, 600));
    QVERIFY(view.open(trackNamed("Backdrop", true)));
    settle();
    player->start(60);
    // What the window shows of the view, over nothing: a transparent window stays transparent.
    auto corner = [&] {
        QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        view.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
        return image.pixelColor(2, 2);
    };
    settings->setBackgroundOpacity(0);
    settings->sendChangeNotification();
    QCOMPARE(corner().alpha(), 0);
    settings->setBackgroundOpacity(0.5);
    settings->sendChangeNotification();
    QVERIFY2(std::abs(corner().alpha() - 128) <= 1, qPrintable(QString::number(corner().alpha())));
    // Opaque, the backdrop still tints it.
    settings->setBackgroundOpacity(1);
    settings->sendChangeNotification();
    QColor tinted = corner();
    QCOMPARE(tinted.alpha(), 255);
    QVERIFY(tinted.rgb() != settings->colorScheme().background.rgb());
}

void TestAudioView::renders() {
    if(renderDir.isEmpty())
        QSKIP("Set QIMGV_AUDIOVIEW_RENDER_DIR to keep the renders.");
    QString withCover = trackNamed("Midnight Drive", true);
    QString without = trackNamed("A recording with a remarkably long file name that keeps going and going", false);
    QVariantMap tags{{"artist", "The Example Band"}, {"album", "Night Sessions"}};
    QVariantMap longTags{{"title", "Variations on a Theme That Goes On for Rather Longer Than Anyone Expected"},
                         {"artist", "The London Symphony Orchestra and Chorus with Several Guest Soloists"},
                         {"album", "The Complete Recordings, Volume Twelve"}};
    QVariantMap mp3{{"codec", "mp3"},
                    {"samplerate", 44100},
                    {"channels", 2},
                    {"channel-layout", "stereo"},
                    {"sample-format", "floatp"},
                    {"bitrate", 320000}};

    struct Shot {
        QString name;
        QSize size;
        QString file;
        QVariantMap tags, format;
        AudioPlaybackMode mode;
        bool paused, fail, hour;
    };
    QList<Shot> shots = {
        {"cover-260x180", {260, 180}, withCover, tags, mp3, AUDIO_MODE_SINGLE, false, false, false},
        {"cover-320x240", {320, 240}, withCover, tags, mp3, AUDIO_MODE_SINGLE, false, false, false},
        {"cover-480x360", {480, 360}, withCover, tags, mp3, AUDIO_MODE_SINGLE, false, false, false},
        {"cover-640x480", {640, 480}, withCover, tags, mp3, AUDIO_MODE_FOLDER, false, false, false},
        {"cover-900x360", {900, 360}, withCover, tags, mp3, AUDIO_MODE_REPEAT, true, false, false},
        {"cover-1280x720", {1280, 720}, withCover, tags, mp3, AUDIO_MODE_FOLDER, false, false, false},
        {"cover-1920x1080", {1920, 1080}, withCover, tags, mp3, AUDIO_MODE_SHUFFLE, true, false, true},
        {"long-640x480", {640, 480}, without, longTags, flacFormat(), AUDIO_MODE_SHUFFLE, true, false, true},
        {"long-1280x720", {1280, 720}, without, longTags, flacFormat(), AUDIO_MODE_SINGLE, false, false, false},
        {"error-640x480", {640, 480}, without, {}, {}, AUDIO_MODE_SINGLE, false, true, false},
    };
    for(ColorSchemes scheme : {COLORS_DARK, COLORS_LIGHT, COLORS_BLACK}) {
        setScheme(scheme);
        QString schemeName = scheme == COLORS_DARK ? "dark" : scheme == COLORS_LIGHT ? "light" : "black";
        for(Shot const &shot : shots) {
            AudioView view;
            FakePlayer *player = attach(view);
            present(view, shot.size);
            view.setPlaybackMode(shot.mode);
            QVERIFY(view.open(shot.file));
            settle();
            if(shot.fail) {
                emit player->failed("unrecognized file format");
            } else {
                double duration = shot.hour ? 4210.6 : 245.3;
                player->start(duration, shot.format, shot.tags);
                emit player->positionChanged(duration * 0.3);
                emit player->pausedChanged(shot.paused);
            }
            save(view, schemeName + "-" + shot.name);
        }
    }

    setScheme(COLORS_DARK);
    {
        // The fixture's own embedded cover, a 16x16 red square, scaled up.
        AudioView view;
        FakePlayer *player = attach(view);
        present(view, QSize(640, 480));
        QVERIFY(view.open(kData + "flac-picture.flac"));
        settle();
        player->start(0.25, flacFormat());
        save(view, "dark-embedded-640x480");
    }
    {
        AudioView view;
        FakePlayer *player = attach(view);
        present(view, QSize(1280, 720));
        settings->setAudioBackdrop(false);
        settings->sendChangeNotification();
        QVERIFY(view.open(withCover));
        settle();
        player->start(245.3, mp3, tags);
        emit player->positionChanged(100);
        save(view, "dark-1280x720-no-backdrop");
        settings->setAudioBackdrop(true);
        settings->sendChangeNotification();
        view.onFullscreenModeChanged(true);
        view.toggleMute();
        save(view, "dark-1280x720-fullscreen-muted");
    }
}

void TestAudioView::playsThroughThePlugin() {
#ifndef QIMGV_TEST_PLAYER_PLUGIN
    QSKIP("Built without the player plugin.");
#else
    QLibrary plugin(QStringLiteral(QIMGV_TEST_PLAYER_PLUGIN));
    using CreateFn = AudioPlayer *(*)();
    auto create = reinterpret_cast<CreateFn>(plugin.resolve("CreateAudioPlayer"));
    if(!create)
        QSKIP(qPrintable("No CreateAudioPlayer in the plugin: " + plugin.errorString()));
    AudioView view;
    AudioPlayer *player = create();
    view.setPlayerForTesting(player);
    QSignalSpy positions(player, SIGNAL(positionChanged(double)));
    present(view, QSize(800, 600));
    // Nobody wants a test to make noise.
    view.toggleMute();
    QSignalSpy finished(&view, &AudioView::playbackFinished);
    QSignalSpy failed(&view, &AudioView::playbackFailed);
    if(!view.open(kData + "flac-picture.flac"))
        QSKIP("The plugin did not take the file.");
    auto bar = view.findChild<MediaSlider *>("AudioSeekBar");
    // CI machines have no sound card: mpv may then refuse the file, or drop the audio and end at once.
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0 || failed.count() > 0, 15000);
    if(failed.count() > 0)
        QSKIP(qPrintable("mpv could not play here: " + view.findChild<QLabel *>("AudioError")->text()));
    if(bar->maximum() <= 0)
        QSKIP("mpv played nothing here; no audio output?");
    QCOMPARE(finished.count(), 1);
    QVERIFY(std::abs(bar->maximum() - 0.25) < 0.05);
    QVERIFY(view.isPaused());
    // The end shows the whole bar, though mpv's last position is that of the last packet.
    QCOMPARE(bar->value(), bar->maximum());
    QVERIFY(positions.count() > 0);
    QCOMPARE(view.findChild<QLabel *>("AudioDuration")->text(), QString("0:00"));
    QString details = view.findChild<QLabel *>("AudioDetails")->text();
    QVERIFY2(details.startsWith("FLAC · 22.05 kHz"), qPrintable(details));
    QVERIFY2(details.contains("Mono"), qPrintable(details));

    // Play at the end starts the track over.
    view.togglePaused();
    QVERIFY(!view.isPaused());
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 2, 15000);
    view.stop();
#endif
}

QTEST_MAIN(TestAudioView)
#include "test_audioview.moc"
