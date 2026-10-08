#pragma once

#include "audioplayer.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <mpv/client.h>

#if defined QIMGV_PLAYER_MPV_LIBRARY
#define QIMGV_AUDIO_DLLSPEC Q_DECL_EXPORT
#else
#define QIMGV_AUDIO_DLLSPEC Q_DECL_IMPORT
#endif

// AudioPlayer on an mpv instance of its own, with no video output at all. GUI thread only.
class AudioPlayerMpv Q_DECL_FINAL : public AudioPlayer {
    Q_OBJECT
public:
    explicit AudioPlayerMpv(QObject *parent = nullptr);
    ~AudioPlayerMpv() override;

    bool open(QString const &file) override;
    void stop() override;
    // stop() without the wait: mpv closes the file once its core gets to it, which opening an audio device
    // can hold up for a second. For leaving a file that is not about to be renamed, moved or deleted.
    void unload() override;
    void setPaused(bool paused) override;
    bool isPaused() const override;
    void seek(double seconds) override;
    void seekRelative(double seconds) override;
    void setVolume(int volume) override;
    void setMuted(bool muted) override;
    void setLoop(bool loop) override;

private:
    // Where the file from the latest open() is. What mpv reports only becomes signals while Loaded; failed()
    // can come at any point before Ended.
    enum class Entry {
        None,    // nothing opened, stopped, or "loadfile" not answered yet
        Queued,  // mpv took the file; it starts once whatever plays now has been torn down
        Started, // MPV_EVENT_START_FILE seen: mpv is opening it
        Loaded,  // MPV_EVENT_FILE_LOADED seen: playing, paused, or held at its end
        Ended    // MPV_EVENT_END_FILE seen; nothing more is said about it
    };
    enum class Bitrate { Wanted, Requested, Known };
    // Volume, mute and seeks come in floods from sliders and the wheel, faster than mpv answers, and every
    // unanswered request holds one of the slots of mpv's event queue -- once they are gone, every request
    // fails. So each has one in flight at a time, and only the latest value waits for its reply.
    struct Coalesced {
        int inFlight = 0;
        bool pending = false;
    };

    static void onWakeup(void *ctx);
    void drainEvents();
    void handleEvent(mpv_event *event);
    void onCommandReply(mpv_event const *event);
    void onGetReply(mpv_event const *event);
    void onStartFile(int64_t entryId);
    void onFileLoaded();
    void onEndFile(mpv_event_end_file const *endFile);
    void onPropertyChange(uint64_t property, mpv_event_property const *data);
    void onPause(bool paused);
    void onEofReached(bool eof);
    void reachedEnd();
    void flushFormat();
    void syncPaused();
    void read(uint64_t property);
    void restart(bool resume);
    void seekingTo(double target);
    void queueSeek(double seconds, bool relative);
    void sendSeek();
    void sendVolume();
    void sendMute();
    void forget();
    bool isOurs(int64_t entryId) const;
    uint64_t replyTag(uint64_t kind) const;
    bool setAsync(char const *name, mpv_format format, void *value, uint64_t reply = 0);
    void setPauseAsync(bool paused);
    bool commandAsync(QVariantList const &args, uint64_t reply);
    // Named arguments: "name" is the command.
    bool commandAsync(QVariantMap const &args, uint64_t reply);

    mpv_handle *mpv = nullptr;
    // One queued drain at a time, however many events mpv announces before it runs.
    std::atomic_bool mDrainQueued{false};

    // Bumped by open() and stop(); a reply to anything asked for an older file carries an older one.
    uint64_t mGeneration = 0;
    Entry mEntry = Entry::None;
    // mpv's playlist entry for the latest open(), from the "loadfile" reply.
    int64_t mEntryId = 0;
    // START_FILEs not yet followed by their END_FILE, whichever file they are for. See stop().
    int mOpenFiles = 0;
    // stop() waits for its own command to be carried out before trusting mOpenFiles.
    bool mStopPending = false;

    // What isPaused() says: the last request, or what mpv reported since. It must not wait for mpv.
    bool mPaused = false;
    // The last pause state asked for, and how many such requests mpv has yet to answer. A pause mpv
    // reports while neither is pending is one it made itself: keep-open's, at the end.
    bool mPauseRequested = false;
    int mPausesInFlight = 0;
    // Last pausedChanged() for this file; empty until FILE_LOADED sends the first.
    std::optional<bool> mPausedSent;
    bool mLoop = false;
    int mVolume = -1;
    bool mMuted = false;
    Coalesced mVolumeSet;
    Coalesced mMuteSet;
    // The seek waiting in mSeek: absolute, or relative to wherever the one in flight lands.
    Coalesced mSeek;
    double mSeekTarget = 0;
    bool mSeekRelative = false;

    // Per file; reset by forget().
    double mDuration = 0;
    double mPosition = 0;
    bool mEofReached = false;
    // playbackFinished() was sent for this arrival at the end. Leaving the end clears it.
    bool mAtEnd = false;
    // The latest seek was to the end or past it.
    bool mSeekToEnd = false;
    QVariantMap mMetadata;
    QVariantMap mFormat;
    bool mFormatDirty = false;
    Bitrate mBitrate = Bitrate::Wanted;
    // First position since playback (re)started; the bitrate is read kBitrateAfter later.
    std::optional<double> mBitrateFrom;
};

extern "C" QIMGV_AUDIO_DLLSPEC AudioPlayer *CreateAudioPlayer();
