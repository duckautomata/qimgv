#pragma once

#include <cstdint>

// mpv keeps the previous file's last frame across "loadfile" and redraws it on every render until the
// new file has decoded one: ~200 ms normally, for good if the new file fails to load. While covered(),
// MpvWidget paints the background over whatever mpv drew. Kept free of mpv types so the rules can be
// unit-tested.
class StaleFrameCover {
public:
    // mpv hands out playlist entry ids from 1, so 0 can mean "none". kAnyEntry is for a loadfile that
    // succeeded without reporting an id, which a hand-placed libmpv older than its headers could do.
    static constexpr int64_t kNoEntry = 0;
    static constexpr int64_t kAnyEntry = -1;

    // Before "loadfile" or "stop". Events reach MpvWidget queued on the GUI thread, so nothing that
    // belongs to the new file can be handled before this. Size queries still in flight stay counted.
    void arm() {
        mCovered = true;
        mStarted = false;
        mRestarted = false;
        mEntryId = kNoEntry;
    }
    // Only after a successful loadfile: a failed one keeps kNoEntry and never uncovers.
    void setEntryId(int64_t id) { mEntryId = id; }
    void onStartFile(int64_t id) {
        if(mCovered && mEntryId != kNoEntry && (mEntryId == kAnyEntry || id == mEntryId))
            mStarted = true;
    }
    // These return true when the cover has just come down, i.e. the widget needs a repaint.
    bool onPlaybackRestart() {
        if(mCovered && mStarted)
            mRestarted = true;
        return tryUncover();
    }
    void onSizeRequested() { ++mSizeRequests; }
    bool onSizeReply() {
        if(mSizeRequests > 0)
            --mSizeRequests;
        return tryUncover();
    }
    bool covered() const { return mCovered; }

private:
    // PLAYBACK_RESTART is the first sign that mpv draws the new file. A size query still in flight has
    // to be answered too, so the app's zoom has the new file's size by the time anyone can see it.
    bool tryUncover() {
        if(!mCovered || !mRestarted || mSizeRequests > 0)
            return false;
        mCovered = mStarted = mRestarted = false;
        return true;
    }

    bool mCovered = false;
    bool mStarted = false;
    bool mRestarted = false;
    int mSizeRequests = 0;
    int64_t mEntryId = kNoEntry;
};
