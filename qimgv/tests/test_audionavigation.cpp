// The Play folder playback mode's pool and stepping: which files count as the folder's audio, where
// next/previous land, including the wrap-around and the cases Core can hit mid-load, and how a step passes
// over files that turn out not to be audio.
#include <QtTest>
#include "utils/audionavigation.h"

namespace {
// Stands in for Settings::isAudioSuffix().
bool isAudio(QString const &suffix) {
    static QStringList const audio = {"mp3", "flac", "ogg", "opus", "m4a", "wav"};
    return audio.contains(suffix);
}

// What Core passes: audio extensions and video ones, either of which may hold audio only.
bool mayBeAudio(QString const &suffix) {
    static QStringList const video = {"webm", "mkv", "mp4"};
    return isAudio(suffix) || video.contains(suffix);
}

// Play folder's `next` for pick(): a step through what is left of the pool.
std::function<QString(QStringList const &)> stepFrom(QString const &current, int offset) {
    return [current, offset](QStringList const &pool) { return AudioNavigation::step(pool, current, offset); };
}
} // namespace

class Test_AudioNavigation : public QObject {
    Q_OBJECT

private slots:
    void suffix_data();
    void suffix();
    void poolKeepsFolderOrder();
    void poolKeepsCurrent();
    void poolGoesByExtensionOnly();
    void poolOfExtensionlessCurrent();
    void poolEmptyFolder();
    void stepWraps();
    void stepSingleTrack();
    void stepFromOutsidePool();
    void stepEmptyPool();
    void pickSkipsRejected();
    void pickBackwards();
    void pickTestsEachOnce();
    void pickNothingElse();
    void pickFromOutsidePool();
    void pickEmptyFromNext();
    void pickShrinkingPool();
};

void Test_AudioNavigation::suffix_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("expected");
    QTest::newRow("plain") << "/music/track.flac" << "flac";
    QTest::newRow("upper case") << "/music/TRACK.FLAC" << "flac";
    QTest::newRow("several dots") << "/music/a.b.tar.ogg" << "ogg";
    QTest::newRow("none") << "/music/track" << "";
    QTest::newRow("trailing dot") << "/music/track." << "";
    QTest::newRow("dot in folder only") << "/music.d/track" << "";
    QTest::newRow("hidden file") << "/music/.ogg" << "ogg";
    QTest::newRow("backslashes") << "C:\\music.d\\track.Mp3" << "mp3";
    QTest::newRow("bare name") << "track.wav" << "wav";
    QTest::newRow("empty") << "" << "";
}

void Test_AudioNavigation::suffix() {
    QFETCH(QString, path);
    QFETCH(QString, expected);
    QCOMPARE(AudioNavigation::suffix(path), expected);
}

void Test_AudioNavigation::poolKeepsFolderOrder() {
    QStringList const files = {"/d/a.png", "/d/b.mp3", "/d/c.jpg", "/d/d.FLAC", "/d/e.webm", "/d/f.ogg"};
    QStringList const expected = {"/d/b.mp3", "/d/d.FLAC", "/d/f.ogg"};
    QCOMPARE(AudioNavigation::pool(files, "/d/b.mp3", isAudio), expected);
}

void Test_AudioNavigation::poolKeepsCurrent() {
    // Whatever its extension: an audio file opened by its content, under a name the predicate turns down.
    QStringList const files = {"/d/a.webm", "/d/b.mp3", "/d/c.png", "/d/d.WEBM", "/d/e.mkv"};
    QStringList const expected = {"/d/a.webm", "/d/b.mp3"};
    QCOMPARE(AudioNavigation::pool(files, "/d/a.webm", isAudio), expected);
}

void Test_AudioNavigation::poolGoesByExtensionOnly() {
    // The same pool from an .mp3 as from an audio-only .webm: the current file's extension adds nothing, so
    // the audio-only .webm files in a folder of .mp3s are in it too. Videos as well, for Core to sort out.
    QStringList const files = {"/d/a.webm", "/d/b.mp3", "/d/c.png", "/d/d.WEBM", "/d/e.mkv"};
    QStringList const expected = {"/d/a.webm", "/d/b.mp3", "/d/d.WEBM", "/d/e.mkv"};
    QCOMPARE(AudioNavigation::pool(files, "/d/b.mp3", mayBeAudio), expected);
    QCOMPARE(AudioNavigation::pool(files, "/d/a.webm", mayBeAudio), expected);
}

void Test_AudioNavigation::poolOfExtensionlessCurrent() {
    QStringList const files = {"/d/a", "/d/b.mp3", "/d/c", "/d/d.png"};
    QStringList const expected = {"/d/a", "/d/b.mp3"};
    QCOMPARE(AudioNavigation::pool(files, "/d/a", isAudio), expected);
}

void Test_AudioNavigation::poolEmptyFolder() {
    QVERIFY(AudioNavigation::pool({}, "/d/a.mp3", isAudio).isEmpty());
}

void Test_AudioNavigation::stepWraps() {
    QStringList const pool = {"/d/a.mp3", "/d/b.mp3", "/d/c.mp3"};
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", 1), QString("/d/b.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/c.mp3", 1), QString("/d/a.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/b.mp3", -1), QString("/d/a.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", -1), QString("/d/c.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", 4), QString("/d/b.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", -4), QString("/d/c.mp3"));
}

void Test_AudioNavigation::stepSingleTrack() {
    QStringList const pool = {"/d/a.mp3"};
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", 1), QString("/d/a.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/d/a.mp3", -1), QString("/d/a.mp3"));
}

void Test_AudioNavigation::stepFromOutsidePool() {
    QStringList const pool = {"/d/a.mp3", "/d/b.mp3", "/d/c.mp3"};
    QCOMPARE(AudioNavigation::step(pool, "/elsewhere/x.mp3", 1), QString("/d/a.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "/elsewhere/x.mp3", -1), QString("/d/c.mp3"));
    QCOMPARE(AudioNavigation::step(pool, "", 1), QString("/d/a.mp3"));
}

void Test_AudioNavigation::stepEmptyPool() {
    QVERIFY(AudioNavigation::step({}, "/d/a.mp3", 1).isEmpty());
    QVERIFY(AudioNavigation::step({}, "/d/a.mp3", -1).isEmpty());
}

void Test_AudioNavigation::pickSkipsRejected() {
    // A Theora .ogg in an album: the step passes over it to the next track.
    QStringList const pool = {"/d/1.ogg", "/d/2-video.ogg", "/d/3.ogg", "/d/4-video.ogg"};
    auto isAudioFile = [](QString const &path) { return !path.contains("video"); };
    QCOMPARE(AudioNavigation::pick(pool, "/d/1.ogg", stepFrom("/d/1.ogg", 1), isAudioFile), QString("/d/3.ogg"));
    // ...wrapping around past the end.
    QCOMPARE(AudioNavigation::pick(pool, "/d/3.ogg", stepFrom("/d/3.ogg", 1), isAudioFile), QString("/d/1.ogg"));
}

void Test_AudioNavigation::pickBackwards() {
    QStringList const pool = {"/d/1.ogg", "/d/2-video.ogg", "/d/3.ogg"};
    auto isAudioFile = [](QString const &path) { return !path.contains("video"); };
    QCOMPARE(AudioNavigation::pick(pool, "/d/3.ogg", stepFrom("/d/3.ogg", -1), isAudioFile), QString("/d/1.ogg"));
}

void Test_AudioNavigation::pickTestsEachOnce() {
    QStringList const pool = {"/d/a.mp3", "/d/b.webm", "/d/c.webm", "/d/d.webm"};
    QStringList tested;
    auto none = [&](QString const &path) {
        tested.append(path);
        return false;
    };
    // Nothing else is audio: back to where it started, after one look at each of the others.
    QCOMPARE(AudioNavigation::pick(pool, "/d/a.mp3", stepFrom("/d/a.mp3", 1), none), QString("/d/a.mp3"));
    QStringList const expected = {"/d/b.webm", "/d/c.webm", "/d/d.webm"};
    QCOMPARE(tested, expected);
}

void Test_AudioNavigation::pickNothingElse() {
    int calls = 0;
    auto count = [&](QString const &) {
        calls++;
        return true;
    };
    // The only track: itself, never tested -- it is already playing.
    QCOMPARE(AudioNavigation::pick({"/d/a.mp3"}, "/d/a.mp3", stepFrom("/d/a.mp3", 1), count), QString("/d/a.mp3"));
    QCOMPARE(calls, 0);
    QVERIFY(AudioNavigation::pick({}, "/d/a.mp3", stepFrom("/d/a.mp3", 1), count).isEmpty());
    QCOMPARE(calls, 0);
}

void Test_AudioNavigation::pickFromOutsidePool() {
    // The current file gone from the folder, and nothing left that plays: nowhere to go, and no endless loop.
    QStringList const pool = {"/d/a.webm", "/d/b.webm"};
    auto none = [](QString const &) { return false; };
    QVERIFY(AudioNavigation::pick(pool, "/d/gone.mp3", stepFrom("/d/gone.mp3", 1), none).isEmpty());
}

void Test_AudioNavigation::pickEmptyFromNext() {
    // Shuffle going back past the start of its history offers nothing; that is passed on as it is.
    QStringList const pool = {"/d/a.mp3", "/d/b.mp3"};
    auto nothing = [](QStringList const &) { return QString(); };
    auto all = [](QString const &) { return true; };
    QVERIFY(AudioNavigation::pick(pool, "/d/a.mp3", nothing, all).isEmpty());
}

void Test_AudioNavigation::pickShrinkingPool() {
    // Each call to `next` sees the pool without what was turned down before, as Shuffle needs: it must not
    // draw the same video again.
    QStringList const pool = {"/d/a.mp3", "/d/v1.mp4", "/d/v2.mp4", "/d/b.mp3"};
    QList<QStringList> seen;
    auto record = [&](QStringList const &candidates) {
        seen.append(candidates);
        // The last candidate other than the current one, like a draw that keeps landing on videos first.
        for(qsizetype i = candidates.size() - 1; i >= 0; i--) {
            if(candidates.at(i) != "/d/a.mp3" && candidates.at(i) != "/d/b.mp3")
                return candidates.at(i);
        }
        return QString("/d/b.mp3");
    };
    auto isAudioFile = [](QString const &path) { return path.endsWith(".mp3"); };
    QCOMPARE(AudioNavigation::pick(pool, "/d/a.mp3", record, isAudioFile), QString("/d/b.mp3"));
    QCOMPARE(seen.size(), 3);
    QCOMPARE(seen.at(0), pool);
    QStringList const withoutV2 = {"/d/a.mp3", "/d/v1.mp4", "/d/b.mp3"};
    QCOMPARE(seen.at(1), withoutV2);
    QStringList const withoutBoth = {"/d/a.mp3", "/d/b.mp3"};
    QCOMPARE(seen.at(2), withoutBoth);
}

QTEST_APPLESS_MAIN(Test_AudioNavigation)
#include "test_audionavigation.moc"
