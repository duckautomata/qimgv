#include "directoryscanner.h"

#include <QDebug>
#include <filesystem>

#include "utils/stuff.h"

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

void scanDirectoryEntries(QString const &directoryPath, bool recursive, bool showHidden,
                          QRegularExpression const &formatRegex, std::vector<FSEntry> &files,
                          std::vector<FSEntry> &dirs) {
    // A directory can vanish or become unreadable between the caller's check
    // and this running, which on a network share is not even unlikely. The
    // iterator throws for that; an empty listing is the right answer.
    try {
        if(recursive) { // files only
            for(auto const &entry : fs::recursive_directory_iterator(toStdString(directoryPath))) {
                QString name = QString::fromStdString(entry.path().filename().generic_string());
                // is_directory() first: it is the cheaper test, and it lets us
                // skip the regex entirely for subdirectories.
                if(entry.is_directory())
                    continue;
                if(!formatRegex.match(name).hasMatch())
                    continue;
                QString path = QString::fromStdString(entry.path().generic_string());
                FSEntry newEntry;
                try {
                    newEntry.name = name;
                    newEntry.path = path;
                    newEntry.isDirectory = false;
                    newEntry.size = entry.file_size();
                    newEntry.modifyTime = entry.last_write_time();
                } catch(fs::filesystem_error const &err) {
                    qDebug() << "[DirectoryScanner]" << err.what();
                    continue;
                }
                files.emplace_back(newEntry);
            }
            return;
        }

        for(auto const &entry : fs::directory_iterator(toStdString(directoryPath))) {
            QString name = QString::fromStdString(entry.path().filename().generic_string());
#ifndef Q_OS_WIN32
            // ignore hidden files
            if(!showHidden && name.startsWith("."))
                continue;
#else
            // UNICODE is defined, so the unsuffixed GetFileAttributes resolves
            // to the wide variant; path::c_str() is already wchar_t* here and
            // saves a string copy per entry. INVALID_FILE_ATTRIBUTES has every
            // bit set, so an unreadable entry would otherwise look hidden and
            // vanish.
            DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if(!showHidden && attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN))
                continue;
#endif
            QString path = QString::fromStdString(entry.path().generic_string());
            // The format regex only applies to files; matching before the
            // is_directory() test made every subdirectory pay for a match that
            // was then thrown away.
            if(entry.is_directory()) {
                FSEntry newEntry;
                try {
                    newEntry.name = name;
                    newEntry.path = path;
                    newEntry.isDirectory = true;
                } catch(fs::filesystem_error const &err) {
                    qDebug() << "[DirectoryScanner]" << err.what();
                    continue;
                }
                dirs.emplace_back(newEntry);
            } else if(formatRegex.match(name).hasMatch()) {
                FSEntry newEntry;
                try {
                    newEntry.name = name;
                    newEntry.path = path;
                    newEntry.isDirectory = false;
                    newEntry.size = entry.file_size();
                    newEntry.modifyTime = entry.last_write_time();
                } catch(fs::filesystem_error const &err) {
                    qDebug() << "[DirectoryScanner]" << err.what();
                    continue;
                }
                files.emplace_back(newEntry);
            }
        }
    } catch(fs::filesystem_error const &err) {
        qDebug() << "[DirectoryScanner] could not read" << directoryPath << ":" << err.what();
    }
}

DirectoryScannerRunnable::DirectoryScannerRunnable(QString path, bool recursive, bool showHidden,
                                                   QRegularExpression formatRegex, quint64 generation)
    : mPath(std::move(path)), mRecursive(recursive), mShowHidden(showHidden), mFormatRegex(std::move(formatRegex)),
      mGeneration(generation) {}

void DirectoryScannerRunnable::run() {
    auto result = std::make_shared<DirectoryScanResult>();
    result->path = mPath;
    result->generation = mGeneration;
    scanDirectoryEntries(mPath, mRecursive, mShowHidden, mFormatRegex, result->files, result->dirs);
    emit finished(result);
}
