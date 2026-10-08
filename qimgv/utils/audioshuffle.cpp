#include "audioshuffle.h"

AudioShuffle::AudioShuffle(quint32 seed) : mRng(seed ? seed : std::random_device{}()) {}

QString AudioShuffle::next(QStringList const &pool, QString const &current) {
    if(pool.isEmpty())
        return {};
    syncPool(pool);
    if(!current.isEmpty()) {
        alignTo(current);
        // After a prev(), replay what came next last time rather than inventing a new future.
        for(qsizetype i = mCursor + 1; i < mHistory.size(); i++) {
            if(canStepTo(mHistory.at(i), current)) {
                mCursor = i;
                return mHistory.at(i);
            }
        }
    }
    QString pick = takeUnplayed();
    if(pick.isEmpty()) {
        // `current` counts as played in the new round, so it cannot come straight back.
        mPlayedThisRound = {current};
        mUnplayed = mPool;
        pick = takeUnplayed();
    }
    // Still nothing: `current` is the only track there is.
    if(pick.isEmpty())
        return current;
    append(pick);
    return pick;
}

QString AudioShuffle::prev(QStringList const &pool, QString const &current) {
    if(pool.isEmpty() || current.isEmpty())
        return {};
    syncPool(pool);
    alignTo(current);
    for(qsizetype i = mCursor - 1; i >= 0; i--) {
        if(canStepTo(mHistory.at(i), current)) {
            mCursor = i;
            return mHistory.at(i);
        }
    }
    return {};
}

void AudioShuffle::reset() {
    mHistory.clear();
    mCursor = -1;
    mPlayedThisRound.clear();
    mPool.clear();
    mPoolSet.clear();
    mUnplayed.clear();
}

// QList's operator== is O(1) for lists sharing their data, hence keeping the caller's list even when only
// equal to ours. A changed pool resets mUnplayed to all of it: played tracks drop out as they are drawn,
// and tracks new to the folder join the round in progress.
void AudioShuffle::syncPool(QStringList const &pool) {
    bool const changed = pool != mPool;
    mPool = pool;
    if(!changed)
        return;
    mPoolSet = QSet<QString>(pool.cbegin(), pool.cend());
    mUnplayed = pool;
}

// `current` anywhere but under the cursor was reached some other way -- a manual jump, or this is the first
// call -- so the history branches there.
void AudioShuffle::alignTo(QString const &current) {
    if(mCursor < 0 || mHistory.at(mCursor) != current)
        append(current);
    mPlayedThisRound.insert(current);
}

// Anything ahead of the cursor is a future that is no longer going to happen.
void AudioShuffle::append(QString const &path) {
    mHistory.resize(mCursor + 1);
    mHistory.append(path);
    if(mHistory.size() > kMaxHistory)
        mHistory.removeFirst();
    mCursor = mHistory.size() - 1;
}

// Stepping onto `current` would not move anywhere, so a track that occurs twice in a row once the files in
// between have gone is passed over too.
bool AudioShuffle::canStepTo(QString const &path, QString const &current) const {
    return path != current && mPoolSet.contains(path);
}

// Swap-and-pop keeps each draw O(1). Throwing away an already-played entry and drawing again is still uniform
// over the rest, and each one is thrown away only once.
QString AudioShuffle::takeUnplayed() {
    while(!mUnplayed.isEmpty()) {
        std::uniform_int_distribution<qsizetype> pick(0, mUnplayed.size() - 1);
        mUnplayed.swapItemsAt(pick(mRng), mUnplayed.size() - 1);
        QString path = mUnplayed.takeLast();
        if(!mPlayedThisRound.contains(path)) {
            mPlayedThisRound.insert(path);
            return path;
        }
    }
    return {};
}
