#pragma once

#include <QObject>
#include <QRegularExpression>
#include <QRunnable>
#include <QString>
#include <memory>
#include <vector>

#include "sourcecontainers/fsentry.h"

// Enumerating a directory is nearly all of what opening one costs -- on a
// 20,000 file folder here it was about 1.8 of 1.85 seconds, the rest being the
// sort. Run on the GUI thread that is a freeze, and on a slow network share it
// is a freeze long enough that people assume the application has died.
//
// So the enumeration runs on a worker and everything else stays where it was.
// The worker deliberately owns no reference to DirectoryManager: it is handed
// copies of the only two pieces of state it needs, because both can be changed
// from the GUI thread while it runs. QRegularExpression::match() is safe to
// call concurrently on a shared object, but readSettings() reassigns the
// pattern, and QSettings is not safe to read from another thread at all.
struct DirectoryScanResult {
    QString path;
    quint64 generation = 0;
    std::vector<FSEntry> files;
    std::vector<FSEntry> dirs;
};

// Fills files (and, when not recursive, dirs) from directoryPath. Free function
// with no shared state, so it is safe to call from any thread.
void scanDirectoryEntries(QString const &directoryPath, bool recursive, bool showHidden,
                          QRegularExpression const &formatRegex, std::vector<FSEntry> &files,
                          std::vector<FSEntry> &dirs);

class DirectoryScannerRunnable final : public QObject, public QRunnable {
    Q_OBJECT
public:
    DirectoryScannerRunnable(QString path, bool recursive, bool showHidden, QRegularExpression formatRegex,
                             quint64 generation);
    void run() override;

signals:
    void finished(std::shared_ptr<DirectoryScanResult> result);

private:
    QString mPath;
    bool mRecursive;
    bool mShowHidden;
    QRegularExpression mFormatRegex;
    quint64 mGeneration;
};
