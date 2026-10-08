#pragma once

#include <QVariantMap>
#include <QWidget>
#include <memory>
#include "settings.h"

class AudioPlayer;

// What ViewerWidget shows in place of the image viewer while an audio file is current: the cover art,
// title, artist and album, the stream format, a seek bar, and the transport controls (previous, play/pause,
// next, the playback-mode toggle, mute and volume).
//
// It owns the plugin's AudioPlayer, created on first use. Previous, next, play/pause, mute and the mode
// toggle go through actionManager like every other control in qimgv, so a shortcut and a click do the
// same thing and Core stays in charge of navigation.
//
// The view and every child are NoFocus: keyboard focus stays on ViewerWidget, whose Left/Right/Space
// bindings must keep working. Mouse events the controls do not use propagate, so the context menu, wheel
// navigation and double-click fullscreen behave as they do over an image. The buttons take only the left
// button, and the wheel seeks along the whole of the seek bar's row.
class AudioView : public QWidget {
    Q_OBJECT
public:
    explicit AudioView(QWidget *parent = nullptr);
    ~AudioView() override;

    // Opens `file` and starts playing it. False when the player plugin is unavailable; the view then
    // explains why instead of showing controls.
    bool open(QString const &file);
    // Unloads the file and waits for its handle to be released. Nothing plays until the next open().
    void stop();
    // stop() without the wait, for moving on to another file: see AudioPlayer::unload().
    void unload();
    QString currentFile() const;

    void setPaused(bool paused);
    void togglePaused();
    bool isPaused() const;
    void seek(double seconds);
    void seekRelative(double seconds);
    void toggleMute();
    bool isMuted() const;
    void volumeUp();
    void volumeDown();
    // Repeat the track at its end. ViewerWidget decides: the Repeat mode, outside a slideshow.
    void setLoop(bool loop);
    // Updates the mode button's icon and tooltip. Does not change any behaviour by itself.
    void setPlaybackMode(AudioPlaybackMode mode);
    void onFullscreenModeChanged(bool fullscreen);

signals:
    // The track reached its end by itself. Not after a pause of the user's, which mpv can follow with the
    // end when it comes in the last moment: the track then stays at its end, paused.
    void playbackFinished();
    // The file could not be played. The view already says so; this lets the continuous modes and the
    // slideshow move on instead of stopping on it.
    void playbackFailed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // The player's signals, connected by name (see AudioPlayer).
    void onFileLoaded();
    void onDurationChanged(double seconds);
    void onPositionChanged(double seconds);
    void onPausedChanged(bool paused);
    void onMetadataChanged(QVariantMap tags);
    void onFormatChanged(QVariantMap format);
    void onPlaybackFinished();
    void onFailed(QString message);

private:
    friend class TestAudioView;
    // For tests: play through `player` instead of the plugin's, which a test cannot rely on. Takes ownership.
    void setPlayerForTesting(AudioPlayer *player);

    struct Private;
    std::unique_ptr<Private> d;
};
