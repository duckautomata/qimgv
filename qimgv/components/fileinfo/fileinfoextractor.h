#pragma once

#include <QObject>
#include <QRunnable>
#include <QString>
#include <QThreadPool>
#include <QVector>
#include <memory>

// One row of the file info panel.
struct FileInfoField {
    QString name;
    QString value;
};

// A titled group of rows. The panel renders sections in the order given, which
// is the point of using a vector here rather than the QMap this replaces --
// a map sorts by key, so a curated order was impossible.
struct FileInfoSection {
    QString title;
    QVector<FileInfoField> fields;
};

struct FileInfoResult {
    QString path;
    quint64 generation = 0;
    QVector<FileInfoSection> sections;
};

// Does the work. Deliberately a free function taking nothing but a path: it
// runs on a worker and must not reach for anything owned by the GUI thread.
//
// Everything in here is file I/O -- exiv2 reads 27 times for a JPEG and 64
// times for an AVIF -- which is why this is not allowed on the GUI thread. On a
// slow network share that is seconds, and the window would be frozen for all of
// it.
FileInfoResult extractFileInfo(QString const &path);

class FileInfoRunnable : public QObject, public QRunnable {
    Q_OBJECT

public:
    FileInfoRunnable(QString path, quint64 generation);
    void run() override;

signals:
    void finished(std::shared_ptr<FileInfoResult> result);

private:
    QString mPath;
    quint64 mGeneration;
};

// Owns the worker pool and drops results the user has already navigated past.
class FileInfoExtractor : public QObject {
    Q_OBJECT

public:
    explicit FileInfoExtractor(QObject *parent = nullptr);
    ~FileInfoExtractor() override;

    // Newest request wins; anything still in flight is discarded when it lands.
    void request(QString const &path);
    void cancel();

signals:
    void ready(QString path, QVector<FileInfoSection> sections);

private slots:
    void onFinished(std::shared_ptr<FileInfoResult> result);

private:
    QThreadPool pool;
    quint64 generation = 0;
};
