#pragma once

#include <QObject>
#include <QDebug>
#include <QMutex>
#include <QClipboard>
#include <QDrag>
#include <QFileSystemModel>
#include <QDesktopServices>
#include <QTranslator>
#include "appversion.h"
#include "settings.h"
#include "components/directorymodel.h"
#include "components/fileinfo/fileinfoextractor.h"
#include "components/directorypresenter.h"
#include "components/scriptmanager/scriptmanager.h"
#include "components/updatechecker.h"
#include "gui/mainwindow.h"
#include "utils/randomizer.h"
#include "utils/audioshuffle.h"
#include "gui/dialogs/printdialog.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

struct State {
    bool hasActiveImage = false;
    bool delayModel = false;
    QString currentFilePath = "";
    QString directoryPath = "";
    std::shared_ptr<Image> currentImg;
};

enum MimeDataTarget { TARGET_CLIPBOARD, TARGET_DROP };

class Core : public QObject {
    Q_OBJECT
public:
    Core();
    void showGui();

public slots:
    void updateInfoString();
    bool loadPath(QString);

private:
    QElapsedTimer t;

    void initGui();
    void initComponents();
    void connectComponents();
    void initActions();
    void loadTranslation();
    void onUpdate();
    void onFirstRun();
    void initUpdateChecker();
    QString changelogForCurrentVersion();

    // ui stuff
    MW *mw;

    UpdateChecker updateChecker;

    // See Core::modelDelayLoad().
    bool reattachCurrentImageOnLoad = false;
    // From the moment a file opened from another folder is shown until that folder's listing arrives, the
    // model is still the old folder's.
    bool folderListingPending = false;
    FileInfoExtractor fileInfoExtractor;

    // See Core::nextDirectory(). Listing a directory is asynchronous, so the
    // entry those want to open does not exist yet when they ask for it.
    enum PendingSelection { SELECT_NONE, SELECT_FIRST, SELECT_LAST };
    PendingSelection pendingSelection = SELECT_NONE;

    State state;
    bool loopSlideshow, slideshow, shuffle;
    FolderEndAction folderEndAction;

    // components
    std::shared_ptr<DirectoryModel> model;

    DirectoryPresenter thumbPanelPresenter, folderViewPresenter;

    void rotateByDegrees(int degrees);
    void reset();
    bool setDirectory(QString path);

    QDrag *mDrag;
    QMimeData *getMimeDataForImage(std::shared_ptr<Image> img, MimeDataTarget target);
    QTranslator *translator = nullptr;

    Randomizer randomizer;
    void syncRandomizer();

    AudioPlaybackMode audioMode = AUDIO_MODE_SINGLE;
    AudioShuffle audioShuffle;
    // Whether a file the pool's names let in turned out to be audio, for the folder's files; see isAudioFile().
    QHash<QString, bool> audioFiles;
    // Files that would not play, or ended as soon as they started, since a track last played; see
    // onAudioPlaybackFailed().
    QSet<QString> deadAudio;
    // Which way the folder's audio was last gone through: a file that will not play is skipped the same way.
    bool audioForward = true;
    // The track playAudio() is loading; see audioModeNavigates().
    QString pendingAudio;
    // Since the current audio file was shown or started over; see onAudioPlaybackFinished().
    QElapsedTimer audioClock;
    // An audio file that ended or failed before its folder was listed; see deferAudioEnd().
    QString deferredAudioEnd;
    bool deferredAudioFailed = false;
    bool deferAudioEnd(QString const &file, bool failed);
    bool currentIsAudio();
    bool audioModeNavigates();
    QStringList audioPool();
    QString adjacentAudio(QStringList const &pool, bool forward);
    bool isAudioFile(QString const &path);
    QString nextAudio(bool forward);
    void playAudio(QString const &path);
    void forgetAudioFolder();

    // The file the rename overlay was opened for; see showRenameDialog().
    QString renameTarget;
    // The folder view's selection was left where the user put it while the current file changed; see
    // loadFileIndex().
    bool folderViewBehind = false;

    std::shared_ptr<Image> releaseFile(QString const &path);
    DocumentType documentType(QString const &path);
    bool hasPixels(QString const &path);

    void attachModel(DirectoryModel *_model);
    QString selectedPath();
    void guiSetImage(std::shared_ptr<Image> img);
    QTimer slideshowTimer;

    void startSlideshowTimer();
    void startSlideshow();
    void stopSlideshow();

    bool saveFile(const QString &filePath, const QString &newPath);
    bool saveFile(const QString &filePath);

    std::shared_ptr<ImageStatic> getEditableImage(const QString &filePath);
    QList<QString> currentSelection();

    template<typename... Args>
    void edit_template(bool save, QString actionName,
                       const std::function<QImage *(std::shared_ptr<const QImage>, Args...)> &func, Args &&...as);

    void doInteractiveCopy(QString path, QString destDirectory, DialogResult &overwriteAllFiles);
    void doInteractiveMove(QString path, QString destDirectory, DialogResult &overwriteAllFiles);

private slots:
    void readSettings();
    void nextImage();
    void prevImage();
    void nextImageSlideshow();
    void jumpToFirst();
    void jumpToLast();
    void onModelItemReady(std::shared_ptr<Image>, const QString &);
    void onModelItemUpdated(QString fileName);
    void onModelSortingChanged(SortingMode mode);
    void onLoadFailed(const QString &path);
    void rotateLeft();
    void rotateRight();
    void close();
    void scalingRequest(QSize, ScalingFilter);
    void onScalingFinished(QPixmap *scaled, ScalerRequest req);
    void copyCurrentFile(QString destDirectory);
    void moveCurrentFile(QString destDirectory);
    void copyPathsTo(QList<QString> paths, QString destDirectory);
    void interactiveCopy(QList<QString> paths, QString destDirectory);
    void interactiveMove(QList<QString> paths, QString destDirectory);
    void movePathsTo(QList<QString> paths, QString destDirectory);
    FileOpResult removeFile(QString fileName, bool trash);
    void onFileRemoved(QString filePath, int index);
    void onFileRenamed(QString fromPath, int indexFrom, QString toPath, int indexTo);
    void onFileAdded(QString filePath);
    void onFileModified(QString filePath);
    void showResizeDialog();
    void resize(QSize size);
    void flipH();
    void flipV();
    void crop(QRect rect);
    void cropAndSave(QRect rect);
    void discardEdits();
    void toggleCropPanel();
    void toggleFullscreenInfoBar();
    void requestSavePath();
    void saveCurrentFile();
    void saveCurrentFileAs(QString);
    void runScript(const QString &);
    void setWallpaper();
    void removePermanent();
    void moveToTrash();
    void reloadImage();
    void reloadImage(QString fileName);
    void copyFileClipboard();
    void copyPathClipboard();
    void openFromClipboard();
    void renameCurrentSelection(QString newName);
    void sortBy(SortingMode mode);
    void sortByName();
    void sortByTime();
    void sortBySize();
    void showRenameDialog();
    void onDraggedOut();
    void onDraggedOut(QList<QString> paths);
    void onDropIn(const QMimeData *mimeData, QObject *source);
    void toggleShuffle();
    void cycleAudioMode();
    void onModelLoaded();
    void onFileInfoReady(QString path, QVector<FileInfoSection> sections);
    void requestFileInfo();
    void outputError(const FileOpResult &error) const;
    void showOpenDialog();
    void showInDirectory();
    void onDirectoryViewFileActivated(QString filePath);
    // followInFolderView false: see playAudio().
    bool loadFileIndex(int index, bool async, bool preload, bool followInFolderView = true);
    void enableDocumentView();
    void enableFolderView();
    void toggleFolderView();
    void toggleSlideshow();
    void onPlaybackFinished();
    void onAudioPlaybackFinished(QString file);
    void onAudioPlaybackFailed(QString file);
    void setFoldersDisplay(bool mode);
    void loadParentDir();
    void nextDirectory();
    void prevDirectory(bool selectLast);
    void prevDirectory();
    void print();
    void modelDelayLoad();
};
