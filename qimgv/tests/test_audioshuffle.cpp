// AudioShuffle, the order of the Shuffle playback mode. Every instance is seeded, so a failure reproduces;
// the assertions are about properties of the order, never a particular one, so they hold whichever
// std::uniform_int_distribution the standard library implements.
#include <QElapsedTimer>
#include <QtTest>
#include <algorithm>
#include "utils/audioshuffle.h"

namespace {

constexpr quint32 kSeed = 20261007;

QStringList makePool(int count, QString const &dir = QStringLiteral("C:/Users/someone/Music/Some Artist/")) {
    QStringList pool;
    pool.reserve(count);
    for(int i = 0; i < count; i++)
        pool.append(QStringLiteral("%1%2 - Track.flac").arg(dir).arg(i, 5, 10, QChar(u'0')));
    return pool;
}

// The same paths in a list of their own, as the app passes them: it rebuilds the pool per navigation.
QStringList separateCopy(QStringList const &pool) {
    return QStringList(pool.cbegin(), pool.cend());
}

QSet<QString> toSet(QStringList const &list) {
    return QSet<QString>(list.cbegin(), list.cend());
}

QStringList without(QStringList pool, QStringList const &gone) {
    for(auto const &path : gone)
        pool.removeAll(path);
    return pool;
}

// `start`, then `count` tracks from next(), each result becoming the current track.
QStringList walk(AudioShuffle &shuffle, QStringList const &pool, QString const &start, int count) {
    QStringList played{start};
    for(int i = 0; i < count; i++)
        played.append(shuffle.next(pool, played.constLast()));
    return played;
}

// Steps back with prev() from `current` until it returns empty; what it returned, most recent first.
QStringList walkBack(AudioShuffle &shuffle, QStringList const &pool, QString current) {
    QStringList back;
    for(QString p = shuffle.prev(pool, current); !p.isEmpty(); p = shuffle.prev(pool, current)) {
        back.append(p);
        current = p;
        if(back.size() > AudioShuffle::kMaxHistory)
            break;
    }
    return back;
}

QStringList reversed(QStringList list) {
    std::reverse(list.begin(), list.end());
    return list;
}

} // namespace

class Test_AudioShuffle : public QObject {
    Q_OBJECT

private slots:
    void emptyPool();
    void singleTrack();
    void twoTracksAlternate();
    void roundVisitsEveryTrackOnce_data();
    void roundVisitsEveryTrackOnce();
    void consecutiveRounds();
    void prevRetracesInReverse();
    void nextAfterPrevReplaysHistory();
    void manualJumpBranches();
    void removedTracksAreSkippedInHistory();
    void removedTracksAreNeverPicked();
    void addedTracksJoinTheRound();
    void emptyCurrent();
    void resetForgetsHistoryAndRound();
    void historyIsCapped();
    void sameSeedSameOrder();
    void picksAreUniform();
    void largePoolRoundIsFast();
};

void Test_AudioShuffle::emptyPool() {
    AudioShuffle shuffle(kSeed);
    QCOMPARE(shuffle.next({}, QStringLiteral("C:/a.mp3")), QString());
    QCOMPARE(shuffle.prev({}, QStringLiteral("C:/a.mp3")), QString());
    QCOMPARE(shuffle.next({}, QString()), QString());
}

void Test_AudioShuffle::singleTrack() {
    QStringList const pool = makePool(1);
    AudioShuffle shuffle(kSeed);
    QCOMPARE(shuffle.next(pool, pool[0]), pool[0]);
    QCOMPARE(shuffle.next(pool, pool[0]), pool[0]);
    // Coming back to the one track is not a step anywhere, so there is still nothing before it.
    QCOMPARE(shuffle.prev(pool, pool[0]), QString());
    // From a file outside the pool, the one track is where to go.
    QCOMPARE(shuffle.next(pool, QStringLiteral("C:/elsewhere/x.mp3")), pool[0]);
    QCOMPARE(shuffle.next(pool, QString()), pool[0]);
}

void Test_AudioShuffle::twoTracksAlternate() {
    QStringList const pool = makePool(2);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 5);
    QCOMPARE(played, QStringList({pool[0], pool[1], pool[0], pool[1], pool[0], pool[1]}));
    QCOMPARE(walkBack(shuffle, pool, played.constLast()), reversed(played.first(played.size() - 1)));
}

void Test_AudioShuffle::roundVisitsEveryTrackOnce_data() {
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("start");
    QTest::addRow("3 tracks") << 3 << 0;
    QTest::addRow("10 tracks, from the middle") << 10 << 4;
    QTest::addRow("101 tracks, from the end") << 101 << 100;
}

void Test_AudioShuffle::roundVisitsEveryTrackOnce() {
    QFETCH(int, count);
    QFETCH(int, start);
    QStringList const pool = makePool(count);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[start], count - 1);
    QCOMPARE(played.size(), count);
    QCOMPARE(toSet(played), toSet(pool));
}

// Each round is every track once; the track that ends one round has had its turn in the next, so the
// rounds overlap by one and a track never follows itself.
void Test_AudioShuffle::consecutiveRounds() {
    constexpr int kTracks = 7;
    constexpr int kRounds = 6;
    QStringList const pool = makePool(kTracks);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], (kTracks - 1) * kRounds);
    for(int round = 0; round < kRounds; round++) {
        QStringList const window = played.mid(round * (kTracks - 1), kTracks);
        QVERIFY2(toSet(window) == toSet(pool), qPrintable(QStringLiteral("round %1").arg(round)));
    }
    for(int i = 1; i < played.size(); i++)
        QVERIFY(played[i] != played[i - 1]);
}

void Test_AudioShuffle::prevRetracesInReverse() {
    QStringList const pool = makePool(40);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[7], 15);
    QCOMPARE(walkBack(shuffle, pool, played.constLast()), reversed(played.first(played.size() - 1)));
    // An empty answer at the start leaves the cursor where it was.
    QCOMPARE(shuffle.prev(pool, played[0]), QString());
    QCOMPARE(shuffle.next(pool, played[0]), played[1]);
}

void Test_AudioShuffle::nextAfterPrevReplaysHistory() {
    QStringList const pool = makePool(40);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 10);
    QString current = played.constLast();
    for(int i = 0; i < 6; i++)
        current = shuffle.prev(pool, current);
    QCOMPARE(current, played[4]);

    QCOMPARE(walk(shuffle, pool, current, 6), played.mid(4));
    // Past the end of the history it is new ground again, still within the round.
    QString const fresh = shuffle.next(pool, played.constLast());
    QVERIFY(pool.contains(fresh));
    QVERIFY(!played.contains(fresh));
}

void Test_AudioShuffle::manualJumpBranches() {
    QStringList const pool = makePool(20);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 3);
    QCOMPARE(shuffle.prev(pool, played[3]), played[2]);
    QCOMPARE(shuffle.prev(pool, played[2]), played[1]);

    // Clicked a thumbnail instead of pressing next.
    QString const jumpedTo = without(pool, played).constFirst();
    QString const after = shuffle.next(pool, jumpedTo);
    QVERIFY(pool.contains(after));
    QVERIFY(after != jumpedTo);
    // Everything heard this round is still out, the abandoned branch included.
    QVERIFY(!played.contains(after));

    QCOMPARE(walkBack(shuffle, pool, after), QStringList({jumpedTo, played[1], played[0]}));
    QCOMPARE(walk(shuffle, pool, played[0], 3), QStringList({played[0], played[1], jumpedTo, after}));
}

void Test_AudioShuffle::removedTracksAreSkippedInHistory() {
    QStringList const pool = makePool(20);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 4);
    QStringList const smaller = without(pool, {played[2]});

    QCOMPARE(walkBack(shuffle, smaller, played[4]), QStringList({played[3], played[1], played[0]}));
    QCOMPARE(walk(shuffle, smaller, played[0], 3), QStringList({played[0], played[1], played[3], played[4]}));
    QString const fresh = shuffle.next(smaller, played[4]);
    QVERIFY(smaller.contains(fresh));
    QVERIFY(!played.contains(fresh));

    // A file that comes back is part of the history again.
    QCOMPARE(walkBack(shuffle, pool, fresh), QStringList({played[4], played[3], played[2], played[1], played[0]}));
}

void Test_AudioShuffle::removedTracksAreNeverPicked() {
    QStringList const pool = makePool(12);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 2);
    QStringList const gone = without(pool, played).first(4);
    QStringList const smaller = without(pool, gone);

    // The rest of the round is exactly what is left of it.
    QStringList const rest = walk(shuffle, smaller, played.constLast(), smaller.size() - played.size()).mid(1);
    QCOMPARE(rest.size(), 5);
    QCOMPARE(toSet(rest), toSet(without(smaller, played)));

    QStringList const later = walk(shuffle, smaller, rest.constLast(), 50);
    for(auto const &path : gone)
        QVERIFY(!later.contains(path));
}

void Test_AudioShuffle::addedTracksJoinTheRound() {
    QStringList const pool = makePool(10);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 4);

    // New files sort in among the old ones.
    QStringList larger = pool;
    larger.insert(3, QStringLiteral("C:/Users/someone/Music/Some Artist/00002a - Bonus.flac"));
    larger.append(QStringLiteral("C:/Users/someone/Music/Some Artist/zz - Hidden.flac"));

    QStringList const rest = walk(shuffle, larger, played.constLast(), 7).mid(1);
    QCOMPARE(toSet(rest), toSet(without(larger, played)));
    QCOMPARE(toSet(rest).size(), 7);
    // That was the whole round; the next one starts anywhere but here.
    QString const nextRound = shuffle.next(larger, rest.constLast());
    QVERIFY(larger.contains(nextRound));
    QVERIFY(nextRound != rest.constLast());
}

void Test_AudioShuffle::emptyCurrent() {
    QStringList const pool = makePool(5);
    AudioShuffle shuffle(kSeed);
    QCOMPARE(shuffle.prev(pool, QString()), QString());
    QString const first = shuffle.next(pool, QString());
    QVERIFY(pool.contains(first));
    QCOMPARE(shuffle.prev(pool, first), QString());
}

void Test_AudioShuffle::resetForgetsHistoryAndRound() {
    QStringList const pool = makePool(10);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], 8);
    shuffle.reset();

    QCOMPARE(shuffle.prev(pool, played.constLast()), QString());
    // A whole new round from here, the tracks already heard included.
    QStringList const again = walk(shuffle, pool, played.constLast(), 9);
    QCOMPARE(toSet(again), toSet(pool));
}

void Test_AudioShuffle::historyIsCapped() {
    constexpr int kSteps = AudioShuffle::kMaxHistory + 500;
    QStringList const pool = makePool(50);
    AudioShuffle shuffle(kSeed);
    QStringList const played = walk(shuffle, pool, pool[0], kSteps);

    QStringList const back = walkBack(shuffle, pool, played.constLast());
    QCOMPARE(back.size(), AudioShuffle::kMaxHistory - 1);
    QCOMPARE(back, reversed(played.mid(played.size() - AudioShuffle::kMaxHistory, AudioShuffle::kMaxHistory - 1)));
    // The trimmed history still replays forward.
    QCOMPARE(walk(shuffle, pool, back.constLast(), 10), played.mid(played.size() - AudioShuffle::kMaxHistory, 11));
}

void Test_AudioShuffle::sameSeedSameOrder() {
    QStringList const pool = makePool(50);
    auto run = [&](quint32 seed) {
        AudioShuffle shuffle(seed);
        QStringList played = walk(shuffle, pool, pool[0], 120);
        QString current = played.constLast();
        for(int i = 0; i < 30; i++)
            current = shuffle.prev(pool, current);
        played += walk(shuffle, separateCopy(pool), current, 60);
        return played;
    };
    QCOMPARE(run(kSeed), run(kSeed));
    QVERIFY(run(kSeed) != run(kSeed + 1));
}

// 5000 first picks among four tracks: 1250 each expected, sigma about 31, so +-125 is four sigma.
void Test_AudioShuffle::picksAreUniform() {
    QStringList const pool = makePool(5);
    AudioShuffle shuffle(kSeed);
    QHash<QString, int> counts;
    for(int i = 0; i < 5000; i++) {
        shuffle.reset();
        counts[shuffle.next(pool, pool[0])]++;
    }
    QCOMPARE(counts.value(pool[0]), 0);
    for(int i = 1; i < pool.size(); i++) {
        int const n = counts.value(pool[i]);
        QVERIFY2(n > 1125 && n < 1375, qPrintable(QStringLiteral("track %1 picked %2 times").arg(i).arg(n)));
    }
}

// The round itself must not be quadratic in the pool, nor a single call with a pool that has to be
// compared or rebuilt. The bounds are loose enough for a debug build on a slow CI runner.
void Test_AudioShuffle::largePoolRoundIsFast() {
    constexpr int kTracks = 20000;
    QStringList const pool = makePool(kTracks);
    AudioShuffle shuffle(kSeed);

    QElapsedTimer timer;
    timer.start();
    QStringList const played = walk(shuffle, pool, pool[0], kTracks - 1);
    qint64 const roundMs = timer.elapsed();
    QCOMPARE(toSet(played).size(), kTracks);

    QStringList const copy = separateCopy(pool);
    QStringList grown = pool;
    grown.append(QStringLiteral("C:/Users/someone/Music/Some Artist/zz - New.flac"));
    timer.restart();
    shuffle.next(copy, played.constLast());
    qint64 const equalMs = timer.elapsed();
    timer.restart();
    shuffle.next(grown, played.constLast());
    qint64 const grownMs = timer.elapsed();

    qInfo("%d-track round: %lld ms; one call with an equal pool: %lld ms, with a changed one: %lld ms", kTracks,
          roundMs, equalMs, grownMs);
    QVERIFY2(roundMs < 5000, qPrintable(QStringLiteral("round took %1 ms").arg(roundMs)));
    QVERIFY2(equalMs < 500, qPrintable(QStringLiteral("equal pool took %1 ms").arg(equalMs)));
    QVERIFY2(grownMs < 500, qPrintable(QStringLiteral("changed pool took %1 ms").arg(grownMs)));
}

QTEST_APPLESS_MAIN(Test_AudioShuffle)
#include "test_audioshuffle.moc"
