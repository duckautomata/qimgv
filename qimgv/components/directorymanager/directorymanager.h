#pragma once

#include <QObject>
#include <QCollator>
#include <QHash>
#include <QThreadPool>
#include <memory>
#include <QElapsedTimer>
#include <QString>
#include <QSize>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>

#include <vector>
#include <string>
#include <iostream>
#include <filesystem>
#include <algorithm>

#include "settings.h"
#include "watchers/directorywatcher.h"
#include "directoryscanner.h"
#include "utils/stuff.h"
#include "sourcecontainers/fsentry.h"

#ifdef Q_OS_WIN32
#include "windows.h"
#endif

enum FileListSource { // rename? wip
    SOURCE_DIRECTORY,
    SOURCE_DIRECTORY_RECURSIVE,
    SOURCE_LIST
};

class DirectoryManager;

typedef bool (DirectoryManager::*CompareFunction)(const FSEntry &e1, const FSEntry &e2) const;

// TODO: rename? EntrySomething?

class DirectoryManager : public QObject {
    Q_OBJECT
public:
    DirectoryManager();
    ~DirectoryManager() override;

    // Lists the directory on this thread and returns with the entries in place.
    //
    // For the throwaway managers used to answer "what is the next folder along"
    // -- the caller builds one, asks one question and drops it, so waiting is
    // both simpler and what it wants. setDirectory() is the asynchronous one and
    // is what the application uses for the directory it is showing.
    bool setDirectoryBlocking(QString dirPath);
    // ignored if the same dir is already opened
    bool setDirectory(QString);
    bool setDirectoryRecursive(QString);
    QString directoryPath() const;
    int indexOfFile(QString filePath) const;
    int indexOfDir(QString dirPath) const;
    QString filePathAt(int index) const;
    unsigned long fileCount() const;
    unsigned long dirCount() const;
    inline bool isSupportedFile(QString filePath) const;
    bool isEmpty() const;
    bool containsFile(QString filePath) const;
    QString fileNameAt(int index) const;
    QString prevOfFile(QString filePath) const;
    QString nextOfFile(QString filePath) const;
    QString prevOfDir(QString filePath) const;
    QString nextOfDir(QString filePath) const;
    void sortEntryLists();
    QDateTime lastModified(QString filePath) const;

    QString firstFile() const;
    QString lastFile() const;
    void setSortingMode(SortingMode mode);
    SortingMode sortingMode() const;
    bool isFile(QString path) const;
    bool isDir(QString path) const;

    unsigned long totalCount() const;
    bool containsDir(QString dirPath) const;
    const FSEntry &fileEntryAt(int index) const;
    QString dirPathAt(int index) const;
    QString dirNameAt(int index) const;
    bool fileWatcherActive();

    bool insertFileEntry(const QString &filePath);
    bool forceInsertFileEntry(const QString &filePath);
    void removeFileEntry(const QString &filePath);
    void updateFileEntry(const QString &filePath);
    void renameFileEntry(const QString &oldFilePath, const QString &newName);

    bool insertDirEntry(const QString &dirPath);
    // bool forceInsertDirEntry(const QString &dirPath);
    void removeDirEntry(const QString &dirPath);
    // void updateDirEntry(const QString &dirPath);
    void renameDirEntry(const QString &oldDirPath, const QString &newName);

    FileListSource source() const;

    QStringList fileList() const;

private:
    QRegularExpression regex;
    QCollator collator;
    std::vector<FSEntry> fileEntryVec, dirEntryVec;

    // Path -> index, so indexOfFile() is not a linear walk. It was, and
    // DirectoryPresenter::onThumbnailReady() calls it once per delivered
    // thumbnail, which made opening a folder quadratic in the file count.
    //
    // Rebuilt lazily rather than patched at every insert and erase: indices
    // shift under both, and getting that wrong returns a confidently incorrect
    // answer. Invalidation only has to be pessimistic, so a missed call costs a
    // rebuild rather than correctness.
    mutable QHash<QString, int> fileIndexCache, dirIndexCache;
    mutable bool fileIndexCacheValid = false, dirIndexCacheValid = false;
    void invalidateFileIndexCache() const { fileIndexCacheValid = false; }
    void invalidateDirIndexCache() const { dirIndexCacheValid = false; }
    const FSEntry defaultEntry;
    QString mDirectoryPath;

    DirectoryWatcher *watcher;
    void readSettings();
    SortingMode mSortingMode;
    FileListSource mListSource;
    void loadEntryList(QString directoryPath, bool recursive);

    bool path_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool path_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    bool name_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool name_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    bool date_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool date_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    CompareFunction compareFunction();
    bool size_entry_compare(const FSEntry &e1, const FSEntry &e2) const;
    bool size_entry_compare_reverse(const FSEntry &e1, const FSEntry &e2) const;
    void startFileWatcher(QString directoryPath);
    void stopFileWatcher();

    void startScan(QString const &directoryPath, bool recursive);

    // A scan runs on a worker, so its result can arrive after the user has
    // already moved on. Every request carries a generation; a result whose
    // generation is stale belongs to a directory nobody is looking at any more
    // and is dropped. Without this, switching folders while a slow share is
    // being listed installs the wrong listing.
    quint64 scanGeneration = 0;
    // One thread: scans are I/O bound, and running two at once would only make
    // both slower while adding an ordering problem to reason about.
    QThreadPool scanPool;
    bool checkFileRange(int index) const;
    bool checkDirRange(int index) const;

private slots:
    void onScanFinished(std::shared_ptr<DirectoryScanResult> result);
    void onFileAddedExternal(QString fileName);
    void onFileRemovedExternal(QString fileName);
    void onFileModifiedExternal(QString fileName);
    void onFileRenamedExternal(QString oldFileName, QString newFileName);

signals:
    void loaded(const QString &path);
    void sortingChanged();
    void fileRemoved(QString filePath, int);
    void fileModified(QString filePath);
    void fileAdded(QString filePath);
    void fileRenamed(QString fromPath, int indexFrom, QString toPath, int indexTo);

    void dirRemoved(QString dirPath, int);
    void dirAdded(QString dirPath);
    void dirRenamed(QString fromPath, int indexFrom, QString toPath, int indexTo);
};
