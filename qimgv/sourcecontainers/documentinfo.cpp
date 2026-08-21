#include "documentinfo.h"

// Plugin availability cannot change while the process is running, so build
// the set once. QImageReader::supportedImageFormats() walks the plugin loader
// and allocates a fresh QList on every call, which is measurable when
// scanning a directory of thousands of images.
static const QSet<QByteArray>& readableFormats() {
    static const QSet<QByteArray> formats = [] {
        const QList<QByteArray> list = QImageReader::supportedImageFormats();
        return QSet<QByteArray>(list.begin(), list.end());
    }();
    return formats;
}


DocumentInfo::DocumentInfo(QString path)
    : mDocumentType(DocumentType::NONE),
      mOrientation(0),
      mFormat(""),
      exifLoaded(false)
{
    fileInfo.setFile(path);
    if(!fileInfo.isFile()) {
        qDebug() << "FileInfo: cannot open: " << path;
        return;
    }
    detectFormat();
}

DocumentInfo::~DocumentInfo() {
}

// ##############################################################
// ####################### PUBLIC METHODS #######################
// ##############################################################

QString DocumentInfo::directoryPath() const {
    return fileInfo.absolutePath();
}

QString DocumentInfo::filePath() const {
    return fileInfo.absoluteFilePath();
}

QString DocumentInfo::fileName() const {
    return fileInfo.fileName();
}

QString DocumentInfo::baseName() const {
    return fileInfo.baseName();
}

// bytes
qint64 DocumentInfo::fileSize() const {
    return fileInfo.size();
}

DocumentType DocumentInfo::type() const {
    return mDocumentType;
}

QMimeType DocumentInfo::mimeType() const {
    return mMimeType;
}

QString DocumentInfo::format() const {
    return mFormat;
}

QDateTime DocumentInfo::lastModified() const {
    return fileInfo.lastModified();
}

// For cases like orientation / even mimetype change we just reload
// Image from scratch, so don`t bother handling it here
void DocumentInfo::refresh() {
    fileInfo.refresh();
}

int DocumentInfo::exifOrientation() const {
    return mOrientation;
}

// ##############################################################
// ####################### PRIVATE METHODS ######################
// ##############################################################
void DocumentInfo::detectFormat() {
    if(mDocumentType != DocumentType::NONE)
        return;
    QMimeDatabase mimeDb;
    mMimeType = mimeDb.mimeTypeForFile(fileInfo.filePath(), QMimeDatabase::MatchContent);
    auto mimeName = mMimeType.name().toUtf8();
    auto suffix = fileInfo.suffix().toLower().toUtf8();

    // Mime databases sniff an ISOBMFF container by its major brand, and an
    // image *sequence* declares a brand they read as video: an animated AVIF
    // out of ffmpeg has major brand 'avis' and comes back as video/quicktime,
    // which would hand an animated image to the video player. Qt's built-in
    // copy of freedesktop.org.xml does exactly this, so it happens on any
    // system without an external shared-mime-info. The ftyp brands are
    // authoritative and we already parse them -- let them correct the verdict
    // before dispatch. Only these two mime names pay for the extra read.
    if(mimeName == "video/quicktime" || mimeName == "video/mp4") {
        const QSet<QByteArray> brands = isoBmffBrands();
        const char *corrected = nullptr;
        if(brands.contains(QByteArrayLiteral("avif")) || brands.contains(QByteArrayLiteral("avis")))
            corrected = "image/avif";
        else if(brands.contains(QByteArrayLiteral("mif1")) || brands.contains(QByteArrayLiteral("msf1")))
            corrected = "image/heif";
        if(corrected) {
            mMimeType = mimeDb.mimeTypeForName(QString::fromLatin1(corrected));
            mimeName = corrected;
        }
    }
    if(mimeName == "image/jpeg") {
        mFormat = "jpg";
        mDocumentType = DocumentType::STATIC;
    } else if(mimeName == "image/png") {
        if(readableFormats().contains(QByteArrayLiteral("apng")) && detectAPNG()) {
            mFormat = "apng";
            mDocumentType = DocumentType::ANIMATED;
        } else {
            mFormat = "png";
            mDocumentType = DocumentType::STATIC;
        }
    } else if(mimeName == "image/gif") {
        mFormat = "gif";
        mDocumentType = DocumentType::ANIMATED;
    } else if(mimeName == "image/webp" || (mimeName == "audio/x-riff" && suffix == "webp")) {
        mFormat = "webp";
        mDocumentType = detectAnimatedWebP() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/jxl") {
        mFormat = "jxl";
        mDocumentType = detectAnimatedJxl() ? DocumentType::ANIMATED : DocumentType::STATIC;
        if(mDocumentType == DocumentType::ANIMATED && !settings->jxlAnimation()) {
            mDocumentType = DocumentType::NONE;
            qDebug() << "animated jxl is off; skipping file";
        }
    } else if(mimeName == "image/avif") {
        mFormat = "avif";
        mDocumentType = detectAnimatedAvif() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/heif" || mimeName == "image/heic"
              || mimeName == "image/heif-sequence" || mimeName == "image/heic-sequence") {
        // Qt reports both under the "heic" reader name.
        mFormat = "heic";
        mDocumentType = detectAnimatedHeif() ? DocumentType::ANIMATED : DocumentType::STATIC;
    } else if(mimeName == "image/bmp") {
        mFormat = "bmp";
        mDocumentType = DocumentType::STATIC;
    } else if(settings->videoPlayback() && settings->videoFormats().contains(mimeName)) {
        mDocumentType = DocumentType::VIDEO;
        mFormat = settings->videoFormats().value(mimeName);
    } else {
        // just try to open via suffix if all of the above fails
        mFormat = suffix;
        if(mFormat.compare("jfif", Qt::CaseInsensitive) == 0)
            mFormat = "jpg";
        if(settings->videoPlayback() && settings->videoFormats().values().contains(suffix))
            mDocumentType = DocumentType::VIDEO;
        else
            mDocumentType = DocumentType::STATIC;
    }
    loadExifOrientation();
}

// ---------------------------------------------------------------------------
// Format sniffers
//
// These run for every file in a directory listing, so they must stay cheap:
// bounded reads, no full-file scans, no QImageReader unless there is no
// cheaper option.
// ---------------------------------------------------------------------------

// Reads a big-endian uint32 from an open stream. Returns false at EOF.
static bool readU32(QDataStream &in, quint32 &out) {
    if(in.readRawData(reinterpret_cast<char*>(&out), 4) != 4)
        return false;
    out = qFromBigEndian(out);
    return true;
}

// Walks PNG chunks looking for acTL (the APNG animation control chunk), which
// the spec requires to appear before the first IDAT.
//
// The previous implementation only searched the first 120 bytes, so an APNG
// carrying a colour profile or text chunks ahead of acTL was misdetected as a
// still PNG.
bool DocumentInfo::detectAPNG() {
    QFile f(fileInfo.filePath());
    if(!f.open(QFile::ReadOnly))
        return false;

    static const char kPngSignature[8] = {'\x89','P','N','G','\r','\n','\x1a','\n'};
    char signature[8];
    QDataStream in(&f);
    if(in.readRawData(signature, 8) != 8 || memcmp(signature, kPngSignature, 8) != 0)
        return false;

    // Bounded: a conformant encoder puts acTL within the first handful of
    // chunks. This is a guard against a malformed file, not a real limit.
    for(int chunk = 0; chunk < 64; chunk++) {
        quint32 length;
        char type[4];
        if(!readU32(in, length))
            return false;
        if(in.readRawData(type, 4) != 4)
            return false;
        if(memcmp(type, "acTL", 4) == 0)
            return true;
        if(memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0)
            return false; // acTL must precede IDAT
        // Skip payload + CRC. Guard against a length that would overflow.
        if(length > quint32(std::numeric_limits<int>::max()) - 4)
            return false;
        if(in.skipRawData(static_cast<int>(length) + 4) != static_cast<int>(length) + 4)
            return false;
    }
    return false;
}

bool DocumentInfo::detectAnimatedWebP() {
    QFile f(fileInfo.filePath());
    if(!f.open(QFile::ReadOnly))
        return false;

    // RIFF____WEBPVP8X, then flags; bit 1 of the first flag byte is ANIMATION.
    char header[16];
    QDataStream in(&f);
    if(in.readRawData(header, 16) != 16)
        return false;
    if(memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WEBP", 4) != 0)
        return false;
    if(memcmp(header + 12, "VP8X", 4) != 0)
        return false; // simple lossy/lossless WebP: never animated

    in.skipRawData(4); // VP8X chunk size
    char flags;
    if(in.readRawData(&flags, 1) != 1)
        return false;
    return (flags & (1 << 1)) != 0;
}

// JPEG XL animation cannot be sniffed cheaply -- the signature does not carry
// it -- so we defer to the plugin.
bool DocumentInfo::detectAnimatedJxl() {
    QImageReader r(fileInfo.filePath(), "jxl");
    return r.supportsAnimation();
}

// Collects the major brand plus every compatible brand from an ISOBMFF 'ftyp'
// box. AVIF, HEIF and friends are all ISOBMFF, so one reader serves them all.
QSet<QByteArray> DocumentInfo::isoBmffBrands() const {
    QSet<QByteArray> brands;
    QFile f(fileInfo.filePath());
    if(!f.open(QFile::ReadOnly))
        return brands;

    QDataStream in(&f);
    quint32 boxSize;
    char boxType[4];
    if(!readU32(in, boxSize))
        return brands;
    if(in.readRawData(boxType, 4) != 4 || memcmp(boxType, "ftyp", 4) != 0)
        return brands;

    // size==1 means a 64-bit size follows; size==0 means "to end of file".
    // Neither is plausible for an ftyp box, so treat them as malformed.
    if(boxSize < 16 || boxSize > 1024)
        return brands;

    char brand[4];
    if(in.readRawData(brand, 4) != 4)   // major_brand
        return brands;
    brands.insert(QByteArray(brand, 4));
    in.skipRawData(4);                  // minor_version

    // Remaining bytes are a list of 4-byte compatible brands.
    const int remaining = static_cast<int>(boxSize) - 16;
    for(int i = 0; i + 4 <= remaining; i += 4) {
        if(in.readRawData(brand, 4) != 4)
            break;
        brands.insert(QByteArray(brand, 4));
    }
    return brands;
}

// An AVIF image sequence declares the 'avis' brand. Most encoders set it as
// the major brand, but some set major='avif' and list 'avis' only among the
// compatible brands -- checking just the major brand misses those.
bool DocumentInfo::detectAnimatedAvif() {
    return isoBmffBrands().contains(QByteArrayLiteral("avis"));
}

// HEIF image sequences use the 'msf1' brand (ISO/IEC 23008-12); 'hevc'/'avcs'
// appear alongside it depending on the codec.
bool DocumentInfo::detectAnimatedHeif() {
    const QSet<QByteArray> brands = isoBmffBrands();
    return brands.contains(QByteArrayLiteral("msf1"))
        || brands.contains(QByteArrayLiteral("hevc"))
        || brands.contains(QByteArrayLiteral("avcs"));
}


void DocumentInfo::loadExifTags() {
    if(exifLoaded)
        return;
    exifLoaded = true;
    exifTags.clear();
#ifdef USE_EXIV2
    try {
        std::unique_ptr<Exiv2::Image> image;

#if EXIV2_TEST_VERSION(0, 28, 0)
        // 0.28 dropped the wchar_t overloads of open(), so the only path API
        // left is a narrow std::string that the CRT decodes with the process
        // locale. main() puts that locale in UTF-8 mode on Windows, without
        // which anything outside the ANSI codepage fails to open.
        image = Exiv2::ImageFactory::open(fileInfo.filePath().toUtf8().toStdString());
#else
        image = Exiv2::ImageFactory::open(toStdString(fileInfo.filePath()));
#endif

        assert(image.get() != 0);
        image->readMetadata();
        Exiv2::ExifData &exifData = image->exifData();
        if(exifData.empty())
            return;

        Exiv2::ExifKey make("Exif.Image.Make");
        Exiv2::ExifKey model("Exif.Image.Model");
        Exiv2::ExifKey dateTime("Exif.Image.DateTime");
        Exiv2::ExifKey exposureTime("Exif.Photo.ExposureTime");
        Exiv2::ExifKey fnumber("Exif.Photo.FNumber");
        Exiv2::ExifKey isoSpeedRatings("Exif.Photo.ISOSpeedRatings");
        Exiv2::ExifKey flash("Exif.Photo.Flash");
        Exiv2::ExifKey focalLength("Exif.Photo.FocalLength");
        Exiv2::ExifKey userComment("Exif.Photo.UserComment");

        Exiv2::ExifData::const_iterator it;

        it = exifData.findKey(make);
        if(it != exifData.end() /* && it->count() */)
            exifTags.insert(QObject::tr("Make"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(model);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Model"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(dateTime);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Date/Time"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(exposureTime);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            if(r.first < r.second) {
                qreal exp = round(static_cast<qreal>(r.second) / r.first);
                exifTags.insert(QObject::tr("ExposureTime"), "1/" + QString::number(exp) + QObject::tr(" sec"));
            } else {
                qreal exp = round(static_cast<qreal>(r.first) / r.second);
                exifTags.insert(QObject::tr("ExposureTime"), QString::number(exp) + QObject::tr(" sec"));
            }
        }

        it = exifData.findKey(fnumber);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            exifTags.insert(QObject::tr("F Number"), "f/" + QString::number(fn, 'g', 3));
        }

        it = exifData.findKey(isoSpeedRatings);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("ISO Speed ratings"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(flash);
        if(it != exifData.end())
            exifTags.insert(QObject::tr("Flash"), QString::fromStdString(it->value().toString()));

        it = exifData.findKey(focalLength);
        if(it != exifData.end()) {
            Exiv2::Rational r = it->toRational();
            qreal fn = static_cast<qreal>(r.first) / r.second;
            exifTags.insert(QObject::tr("Focal Length"), QString::number(fn, 'g', 3) + QObject::tr(" mm"));
        }

        it = exifData.findKey(userComment);
        if(it != exifData.end()) {
            // crop out 'charset=ascii' etc"
            auto comment = QString::fromStdString(it->value().toString());
            if(comment.startsWith("charset="))
                comment.remove(0, comment.indexOf(" ") + 1);
            exifTags.insert(QObject::tr("UserComment"), comment);
        }
    }

// this should work with both 0.28 and <0.28
#if not EXIV2_TEST_VERSION(0, 28, 0)
#ifdef __WIN32
    catch (Exiv2::BasicError<wchar_t>& e) {
        qDebug() << "Caught Exiv2::BasicError exception:\n" << e.what() << "\n";
        return;
    }
#else
    catch (Exiv2::BasicError<char>& e) {
        qDebug() << "Caught Exiv2::BasicError exception:\n" << e.what() << "\n";
        return;
    }
#endif
#endif

    catch (Exiv2::Error& e) {
        qDebug() << "Caught Exiv2 exception:\n" << e.what() << "\n";
        return;
    }
#endif
}

QMap<QString, QString> DocumentInfo::getExifTags() {
    if(!exifLoaded)
        loadExifTags();
    return exifTags;
}

void DocumentInfo::loadExifOrientation() {
    if(mDocumentType == DocumentType::VIDEO || mDocumentType == DocumentType::NONE)
        return;

    QString path = filePath();
    QImageReader *reader = nullptr;
    if(!mFormat.isEmpty())
        reader = new QImageReader(path, mFormat.toStdString().c_str());
    else
        reader = new QImageReader(path);

    if(reader->canRead())
        mOrientation = static_cast<int>(reader->transformation());
    delete reader;
}
