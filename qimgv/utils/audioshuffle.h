#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <random>

// The order the Shuffle playback mode walks a folder's audio files in. Path-based rather than index-based
// (unlike Randomizer), so sorting, adding or removing files does not scramble it: a path that has gone
// missing from the pool is simply skipped.
//
// Every track plays once before any plays again (a "round"); previous retraces what was actually played,
// and next after a previous walks forward through that history again before picking anything new. A
// manual jump to some other track -- clicking a thumbnail -- starts a new branch of the history there.
class AudioShuffle {
public:
    // A seed of 0 seeds from std::random_device. Tests pass a fixed one.
    explicit AudioShuffle(quint32 seed = 0);

    // The track to play after `current`. `pool` is every audio file in the folder, in folder order.
    // Empty only if the pool is empty. A pool of one returns that track.
    //
    // Rebuilding the pool for every call is fine: it is only rescanned when it differs from the last
    // one, and passing the very same list again skips even the comparison.
    QString next(QStringList const &pool, QString const &current);
    // The track played before `current`, or empty at the start of the history.
    QString prev(QStringList const &pool, QString const &current);
    // Forgets the history and the round, e.g. when the folder changes.
    void reset();

    // How far back previous can go.
    static constexpr int kMaxHistory = 1000;

private:
    void syncPool(QStringList const &pool);
    void alignTo(QString const &current);
    void append(QString const &path);
    bool canStepTo(QString const &path, QString const &current) const;
    QString takeUnplayed();

    // Oldest first; mCursor is on the current track.
    QStringList mHistory;
    qsizetype mCursor = -1;
    QSet<QString> mPlayedThisRound;
    // The last pool seen, and what is derived from it.
    QStringList mPool;
    QSet<QString> mPoolSet;
    // The pool's tracks not yet played this round, plus some that have been since it was filled: those are
    // dropped when drawn, so marking a track played never has to search this.
    QStringList mUnplayed;
    std::mt19937 mRng;
};
