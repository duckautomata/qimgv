#include "audiometadata.h"
#include "audiometa_p.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <iterator>
#include <new>

// Which parsers a file gets and in what order, the cover decoded from what they find, and the cover files
// people keep next to their music. The order is the priority rule: addField() keeps the first value of each
// well-known field, so the tag that should win is parsed first and the others only fill its gaps.

namespace AudioMeta {

namespace {

enum class Container { Unknown, Flac, Ogg, Mp4, Asf, RiffWave, Aiff, Dsf, Matroska };

// ASF's header GUID is the longest signature.
constexpr qint64 kSniffBytes = 16;
constexpr char kAsfHeaderGuid[] = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
// QuickTime files written before 'ftyp' existed open with whichever of these boxes the muxer put first.
constexpr const char *kMp4FirstBoxes[] = {"ftyp", "moov", "mdat", "free", "skip", "wide"};

// A tagger that misses the ID3v2 tag already there writes another in front of it, so a file can start with
// several. More than a handful is not a tagger's doing.
constexpr int kMaxLeadingId3v2Tags = 8;

// Album art is a few thousand pixels on a side at most. A header claiming more is a corrupt file or one built
// to make a worker thread allocate gigabytes, and is refused before anything is decoded.
constexpr int kMaxCoverSide = 16384;
constexpr qint64 kMaxCoverPixels = 100'000'000;

Container sniff(QByteArray const &head) {
    if(head.startsWith("fLaC"))
        return Container::Flac;
    if(head.startsWith("OggS"))
        return Container::Ogg;
    if(head.startsWith(QByteArrayView(kAsfHeaderGuid, kSniffBytes)))
        return Container::Asf;
    if(head.startsWith("\x1A\x45\xDF\xA3"))
        return Container::Matroska;
    if(head.startsWith("DSD "))
        return Container::Dsf;
    const QByteArray form = head.mid(8, 4);
    if((head.startsWith("RIFF") || head.startsWith("RF64") || head.startsWith("BW64")) && form == "WAVE")
        return Container::RiffWave;
    if(head.startsWith("FORM") && (form == "AIFF" || form == "AIFC"))
        return Container::Aiff;
    const QByteArray box = head.mid(4, 4);
    for(const char *type : kMp4FirstBoxes) {
        if(box == type)
            return Container::Mp4;
    }
    return Container::Unknown;
}

// The container's own tag, for a file that starts with its container.
bool parseContainer(Container container, ByteSource const &src, AudioMetadata &meta, int parts) {
    switch(container) {
    case Container::Flac:
        return parseFlac(src, 0, meta, parts);
    case Container::Ogg:
        return parseOgg(src, 0, meta, parts);
    case Container::Mp4:
        return parseMp4(src, meta, parts);
    case Container::Asf:
        return parseAsf(src, meta, parts);
    case Container::RiffWave:
        return parseRiffWave(src, meta, parts);
    case Container::Aiff:
        return parseAiff(src, meta, parts);
    case Container::Dsf:
        return parseDsf(src, meta, parts);
    case Container::Matroska:
        return parseMatroska(src, meta, parts);
    case Container::Unknown:
        break;
    }
    return false;
}

// MP3, ADTS AAC, Monkey's Audio, WavPack, Musepack, TTA, TAK, OptimFROG: none has a tag of its own, and
// their taggers append APEv2 and ID3v1 at the end. APEv2 holds more and in Unicode, so it goes first.
void parseEndTags(ByteSource const &src, AudioMetadata &meta, int parts) {
    parseApeTag(src, meta, parts);
    parseId3v1(src, meta, parts);
}

// By content: the mime type a tag declares is often missing or wrong, and a sidecar's extension can be too.
QImage decode(QIODevice &device, QSize maxSize, QSize *originalSize) {
    QImageReader reader(&device);
    reader.setAutoTransform(true);
    // A decoder that cannot say how large the image is before decoding it cannot be held to the limit.
    const QSize stored = reader.size();
    if(stored.isEmpty() || stored.width() > kMaxCoverSide || stored.height() > kMaxCoverSide ||
       qint64(stored.width()) * stored.height() > kMaxCoverPixels)
        return {};
    // The EXIF orientation is applied after decoding, so the scaled size the decoder gets is in the stored
    // orientation while the caller's limit is in the displayed one.
    const bool turned = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    const QSize full = turned ? stored.transposed() : stored;
    const bool shrink = !maxSize.isEmpty() && (full.width() > maxSize.width() || full.height() > maxSize.height());
    if(shrink && reader.supportsOption(QImageIOHandler::ScaledSize)) {
        const QSize scaled = full.scaled(maxSize, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        reader.setScaledSize(turned ? scaled.transposed() : scaled);
    }
    QImage image = reader.read();
    if(image.isNull())
        return {};
    if(shrink && (image.width() > maxSize.width() || image.height() > maxSize.height())) {
        const QSize scaled = image.size().scaled(maxSize, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        image = image.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if(originalSize)
        *originalSize = full;
    return image;
}

} // namespace

static AudioMetadata readIndexed(ByteSource const &src, int parts) {
    AudioMetadata meta;
    if(!src.isValid() || !(parts & AudioMetadataReader::All))
        return meta;
    // Callers are worker threads, where an escaping exception ends the program. A file that needs more memory
    // than there is just has no tags.
    try {
        QList<qint64> id3v2Tags;
        qint64 start = 0;
        while(id3v2Tags.size() < kMaxLeadingId3v2Tags) {
            const qint64 size = id3v2TagSize(src, start);
            if(size <= 0)
                break;
            id3v2Tags.append(start);
            if(size >= src.size() - start) {
                start = src.size();
                break;
            }
            start += size;
        }
        const Container container = sniff(src.read(start, qMin(kSniffBytes, src.size() - start)));

        if(id3v2Tags.isEmpty()) {
            if(!parseContainer(container, src, meta, parts))
                parseEndTags(src, meta, parts);
            return meta;
        }
        // Taggers that only know ID3v2 put it in front of FLAC and Ogg as well, but the native comment is the
        // one FLAC and Ogg tools write and show, so it wins. Only these two can be parsed from an offset;
        // anything else behind ID3v2 is MPEG audio or AAC as far as tags go.
        bool native = false;
        if(container == Container::Flac)
            native = parseFlac(src, start, meta, parts);
        else if(container == Container::Ogg)
            native = parseOgg(src, start, meta, parts);
        for(const qint64 offset : std::as_const(id3v2Tags))
            parseId3v2(src, offset, meta, parts);
        if(!native)
            parseEndTags(src, meta, parts);
        return meta;
    } catch(std::bad_alloc const &) {
        return {};
    }
}

AudioMetadata read(ByteSource const &src, int parts) {
    AudioMetadata meta = readIndexed(src, parts);
    // Only the parsers need it, and it would travel with every copy.
    meta.fieldIndex.clear();
    return meta;
}

QImage loadCover(QByteArray const &embedded, QString const &audioPath, QSize maxSize, QSize *originalSize,
                 AudioCoverSource *source) {
    if(originalSize)
        *originalSize = QSize();
    if(source)
        *source = {};
    if(!embedded.isEmpty()) {
        QBuffer buffer;
        buffer.setData(embedded);
        if(buffer.open(QIODevice::ReadOnly)) {
            QImage image = decode(buffer, maxSize, originalSize);
            if(!image.isNull())
                return image;
        }
    }
    const QString sidecar = AudioMetadataReader::findSidecarCover(audioPath);
    if(sidecar.isEmpty())
        return {};
    const QFileInfo info(sidecar);
    const QDateTime lastModified = info.lastModified();
    QFile file(sidecar);
    // Some decoders read the whole file before looking at it, so a sidecar is held to the size an embedded
    // picture is.
    if(!file.open(QIODevice::ReadOnly) || file.size() > kMaxPictureBytes)
        return {};
    QImage image = decode(file, maxSize, originalSize);
    if(source && !image.isNull())
        *source = {sidecar, lastModified, info.size()};
    return image;
}

} // namespace AudioMeta

namespace AudioMetadataReader {

AudioMetadata read(QString const &path, int parts) {
    if(path.isEmpty() || !(parts & All))
        return {};
    return AudioMeta::read(AudioMeta::ByteSource(path), parts);
}

QImage loadCover(QString const &path, QSize maxSize, QSize *originalSize, AudioCoverSource *source) {
    // The audio file is closed again before decoding starts: on Windows an open file cannot be renamed or
    // deleted, and the user may be doing either.
    return AudioMeta::loadCover(read(path, Cover).cover.data, path, maxSize, originalSize, source);
}

namespace {

// What a cover file next to the tracks may be, in order of preference.
constexpr const char *kCoverExtensions[] = {"jpg", "jpeg", "png", "webp", "bmp", "gif"};

// A folder of N tracks without art of their own is asked for its cover N times, nearly all at once while its
// thumbnails are made, and listing it every time made that quadratic: slow for a large folder, and slower over
// a network share. So the last few listings are kept for as long as the folder's modification date, which
// adding, removing or renaming a file in it changes, says they still hold.
struct Listing {
    QString folder;
    qint64 modified = 0; // the folder's, in ms
    QElapsedTimer age;
    QStringList names;
};

constexpr int kKeptListings = 4;
// FAT does not date a folder when the files in it change, and network file systems may report a date late,
// so even an unchanged date is believed only this long: about as long as one folder's thumbnails take.
constexpr qint64 kListingLifetimeMs = 5000;
// A file system's clock moves in ticks, of up to two seconds on FAT, so a change still to come could leave a
// folder changed this recently with the date it has now. Such a folder is listed every time.
constexpr qint64 kSettledMs = 2000;
// A folder of photos with a few tracks among them would otherwise keep tens of thousands of names.
constexpr qsizetype kMaxKeptNames = 10000;

// The files in `folder` with a cover's extension.
QStringList listCandidates(QString const &folder) {
    static const QStringList filters = [] {
        QStringList patterns;
        for(const char *extension : kCoverExtensions)
            patterns.append(QStringLiteral("*.") + QLatin1StringView(extension));
        return patterns;
    }();
    static QMutex mutex;
    static QList<Listing> kept; // most recently used first
    // Dated before listing: a change made while the listing runs then shows as a new date next time.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QDateTime date = QFileInfo(folder).lastModified();
    const qint64 modified = date.isValid() ? date.toMSecsSinceEpoch() : -1;
    {
        QMutexLocker lock(&mutex);
        for(qsizetype i = 0; i < kept.size(); i++) {
            if(kept.at(i).folder != folder)
                continue;
            if(kept.at(i).modified == modified && !kept.at(i).age.hasExpired(kListingLifetimeMs)) {
                kept.move(i, 0);
                return kept.first().names;
            }
            kept.removeAt(i);
            break;
        }
    }
    // One listing, matched by the caller: over a network share, a stat for each candidate name costs a round
    // trip. Hidden and system files too, which is how Windows Media Player leaves Folder.jpg and its AlbumArt
    // files.
    const QStringList names = QDir(folder).entryList(filters, QDir::Files | QDir::Hidden | QDir::System, QDir::NoSort);
    if(date.isValid() && now - modified >= kSettledMs && names.size() <= kMaxKeptNames) {
        QMutexLocker lock(&mutex);
        // Another thread may have listed it meanwhile.
        kept.removeIf([&folder](Listing const &listing) { return listing.folder == folder; });
        Listing listing{folder, modified, {}, names};
        listing.age.start();
        kept.prepend(std::move(listing));
        if(kept.size() > kKeptListings)
            kept.removeLast();
    }
    return names;
}

} // namespace

QString findSidecarCover(QString const &audioPath) {
    if(audioPath.isEmpty())
        return {};
    // In order of preference. Windows Media Player's AlbumArt_{GUID}_Large/_Small come last: they are named
    // after its own album IDs, so in a folder holding several albums they may well belong to another one.
    static constexpr const char *kNames[] = {"cover", "folder", "front", "album", "albumart", "albumartsmall"};
    constexpr int kExtensionCount = int(std::size(kCoverExtensions));
    constexpr int kTrackRank = int(std::size(kNames));
    constexpr int kWmpLargeRank = kTrackRank + 1;
    constexpr int kWmpSmallRank = kTrackRank + 2;

    const QFileInfo audio(audioPath);
    const QString track = audio.completeBaseName();
    const QDir dir = audio.absoluteDir();
    const QStringList names = listCandidates(dir.absolutePath());

    QString best;
    int bestRank = 0;
    for(QString const &name : names) {
        const qsizetype dot = name.lastIndexOf(u'.');
        if(dot < 0)
            continue;
        const QStringView base = QStringView(name).first(dot);
        const QStringView suffix = QStringView(name).sliced(dot + 1);
        int extension = 0;
        while(extension < kExtensionCount &&
              suffix.compare(QLatin1StringView(kCoverExtensions[extension]), Qt::CaseInsensitive) != 0)
            extension++;
        if(extension == kExtensionCount)
            continue;
        int baseRank = 0;
        while(baseRank < kTrackRank && base.compare(QLatin1StringView(kNames[baseRank]), Qt::CaseInsensitive) != 0)
            baseRank++;
        if(baseRank == kTrackRank && base.compare(track, Qt::CaseInsensitive) != 0) {
            if(!base.startsWith(u"AlbumArt_{", Qt::CaseInsensitive))
                continue;
            if(base.endsWith(u"}_Large", Qt::CaseInsensitive))
                baseRank = kWmpLargeRank;
            else if(base.endsWith(u"}_Small", Qt::CaseInsensitive))
                baseRank = kWmpSmallRank;
            else
                continue;
        }
        const int rank = baseRank * kExtensionCount + extension;
        // Equal ranks differ only in case, which a case-sensitive file system allows; the name decides, so the
        // listing's order does not.
        if(best.isEmpty() || rank < bestRank || (rank == bestRank && name < best)) {
            bestRank = rank;
            best = name;
        }
    }
    return best.isEmpty() ? QString() : dir.absoluteFilePath(best);
}

} // namespace AudioMetadataReader
