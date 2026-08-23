#include "fileinfoextractor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QSet>

#include "sourcecontainers/documentinfo.h"

#ifdef USE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

namespace {

QString tr_(char const *s) {
    return QCoreApplication::translate("FileInfo", s);
}

// Long values are the exception, not the rule, but the exceptions are extreme:
// an unrecognised 20 KB MakerNote renders to a 71,000-character string. Cap the
// text and say what was cut, rather than handing the panel something it cannot
// lay out.
constexpr int kMaxValueChars = 220;

QString capped(QString value) {
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if(value.size() > kMaxValueChars)
        value = value.left(kMaxValueChars) + QStringLiteral("...");
    return value;
}

QString humanSize(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat);
}

// The first bytes of the file, read once. Every sniffer below works off this
// buffer instead of opening the file again -- the old code opened it four to
// six times per image, which on a network share is four to six round trips.
QByteArray readHead(QString const &path, int bytes) {
    QFile f(path);
    if(!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(bytes);
}

quint32 be32(QByteArray const &b, int off) {
    return (static_cast<quint8>(b[off]) << 24) | (static_cast<quint8>(b[off + 1]) << 16) |
           (static_cast<quint8>(b[off + 2]) << 8) | static_cast<quint8>(b[off + 3]);
}

// What actually encodes the pixels, as opposed to what the container is called.
// A .webp may hold VP8 or VP8L; an .avif is AV1 but a .heic is HEVC, and both
// are the same ISOBMFF container.
void appendCodecFields(QByteArray const &head, QVector<FileInfoField> &out) {
    if(head.size() < 16)
        return;

    // RIFF....WEBP<fourcc>
    if(head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
        QByteArray const chunk = head.mid(12, 4);
        QString codec;
        if(chunk == "VP8 ")
            codec = QStringLiteral("VP8 (lossy)");
        else if(chunk == "VP8L")
            codec = QStringLiteral("VP8L (lossless)");
        else if(chunk == "VP8X" && head.size() >= 21) {
            // VP8X is an extended container, not a codec: the flag byte says
            // what is inside it.
            quint8 const flags = static_cast<quint8>(head[20]);
            QStringList features;
            if(flags & 0x10)
                features << QStringLiteral("alpha");
            if(flags & 0x02)
                features << QStringLiteral("animation");
            codec = QStringLiteral("VP8X (extended)");
            if(!features.isEmpty())
                codec += QStringLiteral(" - ") + features.join(QStringLiteral(", "));
        }
        if(!codec.isEmpty())
            out.append({tr_("Codec"), codec});
        return;
    }

    // PNG: the IHDR payload starts 16 bytes in.
    if(head.startsWith(QByteArray("\x89PNG\r\n\x1a\n", 8)) && head.size() >= 29) {
        static char const *const colourTypes[] = {"Greyscale", "", "RGB", "Indexed", "Greyscale + alpha", "", "RGBA"};
        quint8 const depth = static_cast<quint8>(head[24]);
        quint8 const colour = static_cast<quint8>(head[25]);
        quint8 const interlace = static_cast<quint8>(head[28]);
        if(colour < sizeof(colourTypes) / sizeof(*colourTypes) && *colourTypes[colour])
            out.append({tr_("Colour type"),
                        QStringLiteral("%1, %2-bit").arg(QString::fromLatin1(colourTypes[colour])).arg(depth)});
        out.append({tr_("Interlace"), interlace ? tr_("Adam7") : tr_("None")});
        return;
    }

    // ISOBMFF: ....ftyp<major><minor><compatible brands...>
    if(head.size() >= 12 && head.mid(4, 4) == "ftyp") {
        quint32 const boxLen = be32(head, 0);
        int const end = qMin<int>(head.size(), boxLen > 0 ? int(boxLen) : head.size());
        QSet<QByteArray> brands;
        for(int i = 8; i + 4 <= end; i += 4)
            brands.insert(head.mid(i, 4));
        QString codec;
        if(brands.contains("av01") || brands.contains("avif") || brands.contains("avis"))
            codec = QStringLiteral("AV1");
        else if(brands.contains("heic") || brands.contains("heix") || brands.contains("hevc") ||
                brands.contains("hevx") || brands.contains("msf1") || brands.contains("heis"))
            codec = QStringLiteral("HEVC");
        else if(brands.contains("avc1") || brands.contains("avcs"))
            codec = QStringLiteral("H.264");
        if(!codec.isEmpty())
            out.append({tr_("Codec"), codec});
        return;
    }

    // JPEG: walk the segment headers for the frame marker. Baseline and
    // progressive are different markers, and nothing else reports the
    // difference -- QImageReader::subType() says "Automatic" for both.
    if(head.size() >= 4 && static_cast<quint8>(head[0]) == 0xFF && static_cast<quint8>(head[1]) == 0xD8) {
        int i = 2;
        while(i + 4 <= head.size()) {
            if(static_cast<quint8>(head[i]) != 0xFF)
                break;
            quint8 const marker = static_cast<quint8>(head[i + 1]);
            if(marker == 0xC0 || marker == 0xC1) {
                out.append({tr_("Encoding"), tr_("Baseline")});
                return;
            }
            if(marker == 0xC2) {
                out.append({tr_("Encoding"), tr_("Progressive")});
                return;
            }
            if(marker == 0xDA || marker == 0xD9)
                break;
            int const len = (static_cast<quint8>(head[i + 2]) << 8) | static_cast<quint8>(head[i + 3]);
            if(len < 2)
                break;
            i += 2 + len;
        }
    }
}

#ifdef USE_EXIV2
// exiv2's own pretty-printer. value().toString() gives "89" for Flash; print()
// gives "Yes, auto, red-eye reduction". Same for ExposureProgram, Orientation,
// MeteringMode and friends -- the raw numbers are useless to a reader.
template<typename Data, typename Datum>
void appendTags(Data const &data, QString const &title, QVector<FileInfoSection> &out) {
    if(data.empty())
        return;
    FileInfoSection section;
    section.title = title;
    for(auto it = data.begin(); it != data.end(); ++it) {
        Datum const &md = *it;
        QString const key = QString::fromStdString(md.key());
        // Structure rather than content: offsets to other IFDs, and exiv2's own
        // bookkeeping. They render as bare byte offsets and mean nothing to a
        // reader.
        static QSet<QString> const structural = {
            QStringLiteral("Exif.Image.ExifTag"),
            QStringLiteral("Exif.Image.GPSTag"),
            QStringLiteral("Exif.Photo.InteroperabilityTag"),
            QStringLiteral("Exif.MakerNote.Offset"),
            QStringLiteral("Exif.MakerNote.ByteOrder"),
        };
        if(structural.contains(key))
            continue;
        QString label = QString::fromStdString(md.tagLabel());
        if(label.isEmpty())
            label = key;
        // size() and count() are O(1); print() on a large blob is not. Check
        // first so a 20 KB MakerNote never becomes a 71,000-character string.
        if(md.size() > 512) {
            section.fields.append({label, tr_("<binary, %1 bytes>").arg(md.size())});
            continue;
        }
        QString value;
        try {
            std::ostringstream os;
            md.write(os);
            value = QString::fromStdString(os.str());
        } catch(...) {
            continue;
        }
        if(value.isEmpty())
            continue;
        section.fields.append({label, capped(value)});
    }
    if(!section.fields.isEmpty())
        out.append(section);
}
#endif

} // namespace

FileInfoResult extractFileInfo(QString const &path) {
    FileInfoResult result;
    result.path = path;

    QFileInfo const fi(path);
    if(!fi.isFile())
        return result;

    FileInfoSection file;
    file.title = tr_("File");
    file.fields.append({tr_("Name"), fi.fileName()});
    file.fields.append({tr_("Folder"), fi.absolutePath()});
    file.fields.append({tr_("Size"), humanSize(fi.size())});
    file.fields.append({tr_("Modified"), QLocale().toString(fi.lastModified(), QLocale::ShortFormat)});
    result.sections.append(file);

    FileInfoSection image;
    image.title = tr_("Image");
    {
        // detectFormat() sniffs content rather than trusting the extension, so
        // a .png that is really a JPEG reports what it actually is.
        DocumentInfo doc(path);
        QString const format = doc.format();
        if(!format.isEmpty())
            image.fields.append({tr_("Format"), format.toUpper()});
        QMimeType const mime = doc.mimeType();
        if(mime.isValid()) {
            image.fields.append({tr_("MIME type"), mime.name()});
            if(!mime.comment().isEmpty())
                image.fields.append({tr_("Kind"), mime.comment()});
        }
    }

    QImageReader reader(path);
    QSize const size = reader.size();
    if(size.isValid())
        image.fields.append({tr_("Resolution"), QStringLiteral("%1 x %2").arg(size.width()).arg(size.height())});
    if(reader.supportsAnimation() && reader.imageCount() > 1)
        image.fields.append({tr_("Frames"), QString::number(reader.imageCount())});

    QByteArray const head = readHead(path, 4096);
    appendCodecFields(head, image.fields);

    if(!image.fields.isEmpty())
        result.sections.append(image);

#ifdef USE_EXIV2
    try {
#if EXIV2_TEST_VERSION(0, 28, 0)
        // 0.28 has no wide-path API; main() puts LC_CTYPE in UTF-8 mode so the
        // CRT decodes this correctly for names outside the ANSI codepage.
        auto img = Exiv2::ImageFactory::open(path.toUtf8().toStdString());
#else
        auto img = Exiv2::ImageFactory::open(path.toStdString());
#endif
        if(img) {
            img->readMetadata();
            // Deliberately not gated on exifData being non-empty: the old code
            // returned early there and so never looked at IPTC or XMP at all,
            // which is why XMP-only files showed nothing.
            appendTags<Exiv2::ExifData, Exiv2::Exifdatum>(img->exifData(), tr_("Exif"), result.sections);
            appendTags<Exiv2::IptcData, Exiv2::Iptcdatum>(img->iptcData(), tr_("IPTC"), result.sections);
            appendTags<Exiv2::XmpData, Exiv2::Xmpdatum>(img->xmpData(), tr_("XMP"), result.sections);
        }
    } catch(Exiv2::Error const &e) {
        qDebug() << "[FileInfo] exiv2:" << e.what();
    } catch(...) {
        // A corrupt file must not take down a pool thread.
        qDebug() << "[FileInfo] unknown exiv2 failure for" << path;
    }
#endif

    return result;
}

FileInfoRunnable::FileInfoRunnable(QString path, quint64 generation)
    : mPath(std::move(path)), mGeneration(generation) {}

void FileInfoRunnable::run() {
    auto result = std::make_shared<FileInfoResult>(extractFileInfo(mPath));
    result->generation = mGeneration;
    emit finished(result);
}

FileInfoExtractor::FileInfoExtractor(QObject *parent) : QObject(parent) {
    // One at a time. Nobody needs two of these at once, and a single thread
    // keeps the file I/O off the shared pools the loader and thumbnailer use.
    pool.setMaxThreadCount(1);
    qRegisterMetaType<std::shared_ptr<FileInfoResult>>("std::shared_ptr<FileInfoResult>");
    qRegisterMetaType<QVector<FileInfoSection>>("QVector<FileInfoSection>");
}

FileInfoExtractor::~FileInfoExtractor() {
    // Bump first so anything already running is stale on arrival, then wait:
    // the runnables emit into this object and must not outlive it.
    ++generation;
    pool.clear();
    pool.waitForDone();
}

void FileInfoExtractor::request(QString const &path) {
    if(path.isEmpty())
        return;
    auto *runnable = new FileInfoRunnable(path, ++generation);
    runnable->setAutoDelete(true);
    connect(runnable, &FileInfoRunnable::finished, this, &FileInfoExtractor::onFinished, Qt::QueuedConnection);
    pool.start(runnable);
}

void FileInfoExtractor::cancel() {
    ++generation;
    pool.clear();
}

void FileInfoExtractor::onFinished(std::shared_ptr<FileInfoResult> result) {
    // The user can page through images faster than this finishes; a result for
    // one they have already left is not wrong, just no longer wanted.
    if(!result || result->generation != generation)
        return;
    emit ready(result->path, result->sections);
}
