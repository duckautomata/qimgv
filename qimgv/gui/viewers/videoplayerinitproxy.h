// performs lazy initialization

#pragma once

#include <memory>
#include <QVBoxLayout>
#include "videoplayer.h"
#include "settings.h"
#include <QPainter>
#include <QLibrary>
#include <QLabel>
#include <QFileInfo>
#include <QDebug>
#include <QOpenGLWidget>

class VideoPlayerInitProxy : public VideoPlayer {
public:
    VideoPlayerInitProxy(QWidget *parent = nullptr);
    ~VideoPlayerInitProxy();
    bool showVideo(QString file);
    void seek(int pos);
    void seekRelative(int pos);
    void pauseResume();
    void frameStep();
    void frameStepBack();
    void stop();
    void setPaused(bool mode);
    void setMuted(bool);
    bool muted();
    void volumeUp();
    void volumeDown();
    void setVolume(int);
    int volume();
    void setVideoUnscaled(bool mode);
    void setLoopPlayback(bool mode);
    std::shared_ptr<VideoPlayer> getPlayer();
    bool isInitialized();

    void installEventFilter(QObject *filterObj);
    void removeEventFilter(QObject *filterObj);

    // Called by ViewerWidget so the video background matches the image one.
    void onFullscreenModeChanged(bool mode);
    // The same temporary, unsaved override the image viewer applies.
    void toggleTransparencyGrid();

public slots:
    void show();
    void hide();

protected:
    void paintEvent(QPaintEvent *event);

private:
    QLibrary playerLib;
    std::shared_ptr<VideoPlayer> player;
    bool initPlayer();
    QVBoxLayout layout;
    QLabel *errorLabel = nullptr;
    QObject *eventFilterObj = nullptr;

    QString libFile;
    QStringList libDirs;

    void updateBackgroundColor();
    QColor bgColor;
    bool mIsFullscreen = false;

    void updateTransparencyGrid();
    QPixmap checkboard;
    bool mTransparencyGrid = false;

    // See the constructor: keeps the window's backing store texture-composited
    // from startup so loading the player later does not recreate the window.
    QOpenGLWidget *glBackingStorePin = nullptr;

private slots:
    void onSettingsChanged();

signals:
    void playbackFinished();
};
