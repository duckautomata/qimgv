#include "thumbnailerrunnable.h"
#include "components/audiometa/audiometadata.h"

ThumbnailerRunnable::ThumbnailerRunnable(ThumbnailCache *_cache, QString _path, int _size, bool _crop, bool _force)
    : path(_path), size(_size), crop(_crop), force(_force), cache(_cache) {}

void ThumbnailerRunnable::run() {
    emit taskStart(path, size);
    std::shared_ptr<Thumbnail> thumbnail = generate(cache, path, size, crop, force);
    emit taskEnd(thumbnail, path);
}

QString ThumbnailerRunnable::generateIdString(QString path, int size, bool crop) {
    QString queryStr = path + QString::number(size);
    if(crop)
        queryStr.append("s");
    queryStr = QString("%1").arg(QString(QCryptographicHash::hash(queryStr.toUtf8(), QCryptographicHash::Md5).toHex()));
    return queryStr;
}

// From the type detected now, never read back from the cache: the cache key
// does not include the type, so a stored label outlives a change in detection.
// An .ogg cached while Ogg audio was still taken for video would say " [v]"
// until the file itself changed.
static QString typeLabel(DocumentType type) {
    if(type == ANIMATED)
        return QStringLiteral(" [a]");
    if(type == VIDEO)
        return QStringLiteral(" [v]");
    return {};
}

// What a cover thumbnail was made from, kept with it in the cache: "embedded",
// or the name of the cover file next to the track with that file's date and
// size.
static void recordCoverSource(QImage &thumbnail, AudioCoverSource const &source) {
    if(source.sidecar.isEmpty()) {
        thumbnail.setText("coverSource", QStringLiteral("embedded"));
        return;
    }
    thumbnail.setText("coverSource", QFileInfo(source.sidecar).fileName());
    thumbnail.setText("coverModified", QString::number(source.lastModified.toMSecsSinceEpoch()));
    thumbnail.setText("coverSize", QString::number(source.size));
}

// The track's own date covers its embedded picture, but not a cover file next
// to it: that can be replaced or deleted, or another one that wins over it can
// appear (see findSidecarCover()), all without the track changing.
static bool coverSourceUnchanged(QImage const &thumbnail, QString const &audioPath) {
    QString const source = thumbnail.text("coverSource");
    if(source == QLatin1String("embedded"))
        return true;
    // Nothing recorded: made by a build that did not record it, or while the
    // file was taken for a video.
    if(source.isEmpty())
        return false;
    QFileInfo const sidecar(AudioMetadataReader::findSidecarCover(audioPath));
    return sidecar.fileName() == source &&
           thumbnail.text("coverModified") == QString::number(sidecar.lastModified().toMSecsSinceEpoch()) &&
           thumbnail.text("coverSize") == QString::number(sidecar.size());
}

std::shared_ptr<Thumbnail> ThumbnailerRunnable::generate(ThumbnailCache *cache, QString path, int size, bool crop,
                                                         bool force) {
    DocumentInfo imgInfo(path);
    QString thumbnailId = generateIdString(path, size, crop);
    std::unique_ptr<QImage> image;
    bool const isAudio = imgInfo.type() == AUDIO;
    Thumbnail::Kind const kind = isAudio ? Thumbnail::Kind::Audio : Thumbnail::Kind::Generic;

    QString time = QString::number(imgInfo.lastModified().toMSecsSinceEpoch());

    if(!force && cache) {
        image.reset(cache->readThumbnail(thumbnailId));
        if(image && (image->text("lastModified") != time || (isAudio && !coverSourceUnchanged(*image, path))))
            image.reset(nullptr);
    }

    if(!image) {
        if(imgInfo.type() == DocumentType::NONE) {
            std::shared_ptr<Thumbnail> thumbnail(new Thumbnail(imgInfo.fileName(), "", size, nullptr));
            return thumbnail;
        }
        std::pair<QImage *, QSize> pair;
        AudioCoverSource coverSource;
        if(imgInfo.type() == VIDEO)
            pair = createVideoThumbnail(path, size, crop);
        else if(isAudio)
            pair = createAudioThumbnail(path, size, crop, &coverSource);
        else
            pair = createThumbnail(imgInfo.filePath(), imgInfo.format().toStdString().c_str(), size, crop);
        if(!pair.first) {
            // Audio without cover art; the other two always return an image.
            // Not a failure, and nothing to cache: finding out took a few
            // small reads, not a decode.
            std::shared_ptr<Thumbnail> thumbnail(
                new Thumbnail(imgInfo.fileName(), imgInfo.format().toUpper(), size, nullptr, kind));
            return thumbnail;
        }
        image.reset(pair.first);
        QSize originalSize = pair.second;

        image = ImageLib::exifRotated(std::move(image), imgInfo.exifOrientation());

        // put in image info
        image->setText("originalWidth", QString::number(originalSize.width()));
        image->setText("originalHeight", QString::number(originalSize.height()));
        image->setText("lastModified", time);
        if(isAudio)
            recordCoverSource(*image, coverSource);

        if(cache) {
            // save thumbnail if it makes sense
            // FIXME: avoid too much i/o
            if(originalSize.width() > size || originalSize.height() > size)
                cache->saveThumbnail(image.get(), thumbnailId);
        }
    }
    // This runs on a thread pool, so it must not build a QPixmap: that is a GUI
    // thread type, and constructing one here is undefined behaviour that
    // happens to work often enough to survive. Thumbnail turns the image into a
    // pixmap when something first paints it. The old code also allocated a
    // full-size pixmap and overwrote it on the next line, which is now gone.
    QString label;
    if(image->width() == 0) {
        label = "error";
    } else if(isAudio) {
        // The cover's dimensions say nothing about the file; what it is does.
        label = imgInfo.format().toUpper();
    } else {
        // put info into Thumbnail object
        label = image->text("originalWidth") + "x" + image->text("originalHeight") + typeLabel(imgInfo.type());
    }
    std::shared_ptr<Thumbnail> thumbnail(new Thumbnail(imgInfo.fileName(), label, size, std::move(image), kind));
    return thumbnail;
}

ThumbnailerRunnable::~ThumbnailerRunnable() {}

std::pair<QImage *, QSize> ThumbnailerRunnable::createThumbnail(QString path, const char *format, int size,
                                                                bool squared) {
    QImageReader *reader = new QImageReader(path, format);
    Qt::AspectRatioMode ARMode = squared ? (Qt::KeepAspectRatioByExpanding) : (Qt::KeepAspectRatio);
    QImage *result = nullptr;
    QSize originalSize;
    bool indexed = (reader->imageFormat() == QImage::Format_Indexed8);
    bool manualResize = indexed || !reader->supportsOption(QImageIOHandler::Size);
    if(!manualResize) { // resize during read via QImageReader (faster)
        QSize scaledSize = reader->size().scaled(size, size, ARMode);
        reader->setScaledSize(scaledSize);
        if(squared) {
            QRect clip(0, 0, size, size);
            QRect scaledRect(QPoint(0, 0), scaledSize);
            clip.moveCenter(scaledRect.center());
            reader->setScaledClipRect(clip);
        }
        originalSize = reader->size();
        result = new QImage();
        if(!reader->read(result)) {
            // If read() returns false there's no guarantee that size conversion worked properly.
            // So we fallback to manual.
            // Se far I've seen this happen only on some weird (corrupted?) jpeg saved from camera
            manualResize = true;
            delete result;
            result = nullptr;
            // Force reset reader because it is really finicky
            // and can fail on the second read attempt (yeah wtf)
            reader->setFileName("");
            delete reader;
            reader = new QImageReader(path, format);
        }
    }
    if(manualResize) { // manual resize & crop. slower but should just work
        QImage *fullSize = new QImage();
        reader->read(fullSize);
        if(indexed) {
            auto newFmt = QImage::Format_RGB32;
            if(fullSize->hasAlphaChannel())
                newFmt = QImage::Format_ARGB32;
            auto tmp = new QImage(fullSize->convertToFormat(newFmt));
            delete fullSize;
            fullSize = tmp;
        }
        originalSize = fullSize->size();
        QSize scaledSize = fullSize->size().scaled(size, size, ARMode);
        if(squared) {
            QRect clip(0, 0, size, size);
            QRect scaledRect(QPoint(0, 0), scaledSize);
            clip.moveCenter(scaledRect.center());
            QImage scaled = QImage(fullSize->scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            result = ImageLib::croppedRaw(&scaled, clip);
        } else {
            result = new QImage(fullSize->scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
        delete fullSize;
    }
    // force reader to close file so it can be deleted later
    reader->setFileName("");
    delete reader;
    return std::make_pair(result, originalSize);
}

std::pair<QImage *, QSize> ThumbnailerRunnable::createVideoThumbnail(QString path, int size, bool squared) {
    QFileInfo fi(path);
    QImageReader reader;
    QString tmpFilePath = settings->tmpDir() + fi.fileName() + ".png";
    QString tmpFilePathEsc = tmpFilePath;
    tmpFilePathEsc.replace("%", "%%");
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(settings->mpvBinary(), QStringList() << "--start=30%"
                                                       << "--frames=1"
                                                       << "--aid=no"
                                                       << "--sid=no"
                                                       << "--no-config"
                                                       << "--load-scripts=no"
                                                       << "--no-terminal"
                                                       << "--o=" + tmpFilePathEsc << path);
    process.waitForFinished(8000);
    process.close();

    reader.setFileName(tmpFilePath);
    reader.setFormat("png");
    Qt::AspectRatioMode ARMode = squared ? (Qt::KeepAspectRatioByExpanding) : (Qt::KeepAspectRatio);
    QImage *result = nullptr;

    // scale & crop
    QSize scaledSize = reader.size().scaled(size, size, ARMode);
    reader.setScaledSize(scaledSize);
    if(squared) {
        QRect clip(0, 0, size, size);
        QRect scaledRect(QPoint(0, 0), scaledSize);
        clip.moveCenter(scaledRect.center());
        reader.setScaledClipRect(clip);
    }
    QSize originalSize = reader.size();
    result = new QImage(reader.read());

    // force reader to close file so it can be deleted later
    reader.setFileName("");

    // remove temporary file
    QFile tmpFile(tmpFilePath);
    tmpFile.remove();

    return std::make_pair(result, originalSize);
}

// The cover art, scaled and cropped the way createThumbnail() treats an image
// file of the same size. Read in-process: the mpv binary the video path runs
// would cost a process per file, and finds no cover art at the 30% it seeks to.
std::pair<QImage *, QSize> ThumbnailerRunnable::createAudioThumbnail(QString path, int size, bool squared,
                                                                     AudioCoverSource *source) {
    Qt::AspectRatioMode ARMode = squared ? (Qt::KeepAspectRatioByExpanding) : (Qt::KeepAspectRatio);
    QSize originalSize;
    // Asking for no more than the thumbnail lets the decoder scale while it
    // reads; for a 3000 px JPEG cover that is most of the work.
    QImage cover = AudioMetadataReader::loadCover(path, QSize(size, size), &originalSize, source);
    if(cover.isNull())
        return {nullptr, QSize()};
    if(!originalSize.isValid())
        originalSize = cover.size();
    QSize scaledSize = originalSize.scaled(size, size, ARMode);
    // Cropping wants the short side at the full thumbnail size, which a cover
    // that is not square does not have once it fits in size x size. Covers
    // nearly always are square, so reading again beats always reading large.
    bool const downscaled = cover.width() < originalSize.width() || cover.height() < originalSize.height();
    if(downscaled && (cover.width() < scaledSize.width() || cover.height() < scaledSize.height())) {
        AudioCoverSource largerSource;
        QImage larger = AudioMetadataReader::loadCover(path, scaledSize, nullptr, &largerSource);
        if(!larger.isNull()) {
            cover = std::move(larger);
            if(source)
                *source = largerSource;
        }
    }
    if(cover.size() != scaledSize)
        cover = cover.scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if(squared) {
        QRect clip(0, 0, size, size);
        clip.moveCenter(cover.rect().center());
        cover = cover.copy(clip);
    }
    return {new QImage(std::move(cover)), originalSize};
}
