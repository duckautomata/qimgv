#pragma once

#include <QWidget>
#include <QColor>
#include <QPixmap>
#include <QSize>

class VideoPlayer : public QWidget {
    Q_OBJECT
public:
    explicit VideoPlayer(QWidget *parent = nullptr);
    virtual bool showVideo(QString file) = 0;
    virtual void seek(int pos) = 0;
    virtual void seekRelative(int pos) = 0;
    virtual void pauseResume() = 0;
    virtual void frameStep() = 0;
    virtual void frameStepBack() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool mode) = 0;
    virtual void setMuted(bool) = 0;
    virtual bool muted() = 0;
    virtual void volumeUp() = 0;
    virtual void volumeDown() = 0;
    virtual void setVolume(int) = 0;
    virtual int volume() = 0;
    virtual void setVideoUnscaled(bool mode) = 0;
    virtual void setLoopPlayback(bool mode) = 0;
    // The colour the player must composite transparent video onto. Not pure:
    // a backend without alpha support can simply ignore it.
    virtual void setBackgroundColor(QColor) {}
    // The tile to show through transparent video, so that alpha video gets the
    // same chequerboard the image viewer draws. A null pixmap turns it off.
    // The tile is passed in rather than named so the plugin does not need a
    // copy of the application's resources.
    virtual void setTransparencyGrid(QPixmap const &) {}

signals:
    void durationChanged(int value);
    void positionChanged(int value);
    void videoPaused(bool);
    void playbackFinished();
    // The picture's size at 100%, in video pixels: aspect ratio applied, and swapped for 90/270 degree
    // rotation because the player rotates when it draws. Empty when there is no picture. Sent for every
    // new file just before its first frame shows -- even if unchanged -- and on changes after that.
    void videoSizeChanged(QSize size);
    // What the player renders into changed size, in the device pixels it renders at.
    void viewportResized(QSize devicePixels);

public slots:
    virtual void show();
    virtual void hide();

public:
    // Declared after show()/hide() so the existing vtable entries keep their slots. That does not make
    // an older plugin usable: the app calls this for every video, so the app and the plugin must be
    // built from this same header (see plugins/player_mpv/CMakeLists.txt).
    enum Placement {
        PLACEMENT_FIT_SHRINK, // fit the widget, never above 100% (the default)
        PLACEMENT_FIT_GROW,   // fit the widget, enlarging small video too
        PLACEMENT_SCALED      // `scale` device pixels per video pixel, positioned by the aligns
    };
    // An align is per axis in [-1, 1] and only matters on an axis the video overflows: -1 shows its
    // left/top edge, 1 its right/bottom edge. An axis the video fits is centred. Not pure: a backend
    // without zoom support simply keeps fitting.
    virtual void setPlacement(Placement /*mode*/, double /*scale*/, double /*alignX*/, double /*alignY*/) {}
};
