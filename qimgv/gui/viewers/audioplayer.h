#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// The audio half of the player plugin's ABI, next to VideoPlayer. qimgv calls through this vtable across
// the DLL boundary, so the app and the plugin must be built from this one header (see
// plugins/player_mpv/CMakeLists.txt) and new virtuals may only be appended.
//
// Connect to the signals with SIGNAL() strings, not pointers to members: the plugin carries its own copy
// of this class's meta-object, so the app's AudioPlayer::staticMetaObject is not the one the plugin's
// object reports.
//
// Unlike VideoPlayer this is not a widget. It owns its own mpv instance with no video output, so it never
// paints and none of the render-thread rules in mpvwidget.cpp apply to it.
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject *parent = nullptr);

    // Starts `file` from the beginning, replacing whatever was loaded. False if it could not even be
    // queued; a file that turns out to be unplayable is reported later through failed().
    virtual bool open(QString const &file) = 0;
    // Unloads the file and closes it, so that it can be renamed, moved or deleted on Windows.
    virtual void stop() = 0;
    // Unpausing at the end of the file starts it again from the beginning.
    virtual void setPaused(bool paused) = 0;
    virtual bool isPaused() const = 0;
    // Seconds. Absolute seeks are clamped to the file; seeking to or past its end counts as reaching it.
    virtual void seek(double seconds) = 0;
    virtual void seekRelative(double seconds) = 0;
    // 0..100.
    virtual void setVolume(int volume) = 0;
    virtual void setMuted(bool muted) = 0;
    // Repeat the current file forever instead of stopping at its end.
    virtual void setLoop(bool loop) = 0;
    // stop() without waiting for the file to be closed, for leaving a file that is not about to be renamed,
    // moved or deleted: opening the audio device can keep mpv busy for a second, and stop() waits that out.
    virtual void unload() = 0;

signals:
    // Every signal describes the file most recently passed to open(); nothing is sent for a file that a
    // later open() replaced. Times are in seconds.

    // The file was opened and is about to play. Duration, format and metadata arrive around it.
    void fileLoaded();
    // 0 when unknown.
    void durationChanged(double seconds);
    void positionChanged(double seconds);
    void pausedChanged(bool paused);
    // mpv's "metadata" property: whatever tags the demuxer found, keys lower-cased.
    void metadataChanged(QVariantMap tags);
    // Keys: "codec" (short name, e.g. "flac"), "codec-description", "container", "samplerate" (Hz),
    // "channels", "channel-layout", "sample-format", "bitrate" (bits per second, 0 if unknown). Any key may
    // be missing.
    void formatChanged(QVariantMap format);
    // Reached the end by itself. Not sent for stop(), open() or a loop wrapping around.
    void playbackFinished();
    // The file could not be played.
    void failed(QString message);
};
