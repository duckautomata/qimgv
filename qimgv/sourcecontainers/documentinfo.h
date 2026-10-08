#pragma once

#include <QString>
#include <QSize>
#include <QUrl>
#include <QMimeDatabase>
#include <QSet>
#include <QtEndian>
#include <limits>
#include <QDebug>
#include <QFileInfo>
#include <QDateTime>
#include <cmath>
#include <cstring>
#include "utils/stuff.h"
#include "settings.h"

#ifdef USE_EXIV2

#include <exiv2/exiv2.hpp>
#include <iostream>
#include <iomanip>
#include <cassert>

#endif

#include <QImageReader>

enum DocumentType { NONE, STATIC, ANIMATED, VIDEO, AUDIO };

class DocumentInfo {
public:
    DocumentInfo(QString path);
    ~DocumentInfo();

    QString directoryPath() const;
    QString filePath() const;
    QString fileName() const;
    QString baseName() const;
    qint64 fileSize() const;
    DocumentType type() const;
    QMimeType mimeType() const;

    // file extension, guessed from the mime type. Audio and video keep their own extension when it is one of
    // theirs (an .mp4 holding only sound is still "mp4"); audio without one is named by its container
    QString format() const;
    int exifOrientation() const;

    QDateTime lastModified() const;
    void refresh();
    void loadExifTags();
    QMap<QString, QString> getExifTags();

private:
    QFileInfo fileInfo;
    DocumentType mDocumentType;
    int mOrientation;
    QString mFormat;
    bool exifLoaded;

    // guesses file type from its contents
    // and sets extension
    void detectFormat();
    void detectMedia(const QByteArray &mimeName, const QByteArray &suffix);
    void loadExifOrientation();
    bool detectAPNG();
    bool detectAnimatedWebP();
    bool detectAnimatedJxl();
    bool detectAnimatedAvif();
    bool detectAnimatedHeif();
    QSet<QByteArray> isoBmffBrands() const;
    QMap<QString, QString> exifTags;
    QMimeType mMimeType;
};
