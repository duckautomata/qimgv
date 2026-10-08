// Audio in the thumbnailer and the file info panel, run through the real
// detection and the real tag reader against the fixtures in data/audio:
//   - cover art is scaled, and cropped, the way an image file of the same size
//     would be, including a non-square cover and a sidecar cover.png
//   - a file without cover art is an audio placeholder, not a failure
//   - the label names the format, never the cover's dimensions, and comes
//     from detection even when the thumbnail comes from the cache
//   - a cached thumbnail made from a cover file next to the track goes when
//     that file changes, goes, or loses out to a better one
//   - the I-key panel shows File / Audio / Tags / Picture and nothing from
//     exiv2, and no more tag rows than it can build quickly
#include <QtTest>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QPainter>
#include <QtEndian>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "components/audiometa/audiometadata.h"
#include "components/cache/thumbnailcache.h"
#include "components/fileinfo/fileinfoextractor.h"
#include "components/thumbnailer/thumbnailerrunnable.h"
#include "settings.h"
#include "sourcecontainers/documentinfo.h"

class Test_AudioThumbs : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void coverThumbnail_data();
    void coverThumbnail();
    void nonSquareSidecarCover();
    void noCover();
    void imageLabelUnchanged();
    void cacheHitUsesCurrentType();
    void cachedOnlyWhenLarger();
    void cachedSidecarCoverFollowsTheFile();
    void cachedSidecarCoverUnicodeName();
    void fileInfoAudio_data();
    void fileInfoAudio();
    void fileInfoSectionTitles();
    void fileInfoMimeTypeAgreesWithSection();
    void fileInfoManyTags();

private:
    static QString dataPath(QString const &name) {
        return QStringLiteral(QIMGV_TEST_DATA_DIR) + QLatin1Char('/') + name;
    }
    static QString audioPath(QString const &name) { return dataPath(QStringLiteral("audio/") + name); }
};

namespace {

QColor const kRed(255, 0, 0);
QColor const kGreen(0, 255, 0);
QColor const kBlue(0, 0, 255);

// Covers are tiny solid-colour PNGs and JPEGs; the JPEG drifts a little.
bool sameColor(QColor const &a, QColor const &b) {
    int const tolerance = 24;
    return qAbs(a.red() - b.red()) <= tolerance && qAbs(a.green() - b.green()) <= tolerance &&
           qAbs(a.blue() - b.blue()) <= tolerance;
}

QString describe(QColor const &c) {
    return QStringLiteral("rgb(%1, %2, %3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

bool hasDimensions(QString const &label) {
    static QRegularExpression const dimensions(QStringLiteral("\\d+\\s*x\\s*\\d+"));
    return label.contains(dimensions);
}

QImage thumbnailImage(std::shared_ptr<Thumbnail> const &thumb) {
    return thumb->pixmap()->toImage();
}

// A copy of `source` in a fresh directory, so no sidecar cover is found next
// to it unless the test puts one there, and every run gets its own cache ids.
QString copyInto(QTemporaryDir const &dir, QString const &source, QString const &name) {
    QString const copy = dir.filePath(name);
    return QFile::copy(source, copy) ? copy : QString();
}

// Larger than the thumbnails below, so that they are cached.
bool saveSolidCover(QString const &path, QColor const &color, int side) {
    QImage cover(side, side, QImage::Format_RGB32);
    cover.fill(color);
    return cover.save(path);
}

QColor centre(std::shared_ptr<Thumbnail> const &thumb) {
    QImage const image = thumbnailImage(thumb);
    return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
}

// Thirds: red, green, blue. Wider than tall, and larger than any thumbnail
// here, so it is scaled down and a square crop is the middle third alone.
bool saveTricolourCover(QString const &path) {
    QImage cover(600, 200, QImage::Format_RGB32);
    {
        QPainter p(&cover);
        p.fillRect(0, 0, 200, 200, kRed);
        p.fillRect(200, 0, 200, 200, kGreen);
        p.fillRect(400, 0, 200, 200, kBlue);
    }
    return cover.save(path);
}

// Mirrors ThumbnailerRunnable::generateIdString(), which is private. Were the
// two to drift, the planted thumbnails below would simply not be found and the
// colour checks would say so.
QString cacheId(QString const &path, int size, bool crop) {
    QString key = path + QString::number(size);
    if(crop)
        key.append(QLatin1Char('s'));
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// A thumbnail as the cache would hold it for `path`, in a colour no fixture
// has, labelled the way an older build might have labelled it. `coverSource`
// is what an audio thumbnail records about its cover; an empty one is left
// out, as a build before it was recorded would have.
QImage plantedThumbnail(QString const &path, int size, QSize originalSize, QString const &staleLabel,
                        QString const &coverSource = {}) {
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(Qt::yellow);
    image.setText("originalWidth", QString::number(originalSize.width()));
    image.setText("originalHeight", QString::number(originalSize.height()));
    image.setText("lastModified", QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch()));
    image.setText("label", staleLabel);
    if(!coverSource.isEmpty())
        image.setText("coverSource", coverSource);
    return image;
}

// A FLAC file that is all tags: STREAMINFO, then one Vorbis comment block.
QByteArray flacWithComments(QStringList const &comments) {
    auto le32 = [](quint32 v) {
        QByteArray b(4, '\0');
        qToLittleEndian(v, b.data());
        return b;
    };
    QByteArray vendor = "qimgv test";
    QByteArray block = le32(quint32(vendor.size())) + vendor + le32(quint32(comments.size()));
    for(QString const &comment : comments) {
        QByteArray const utf8 = comment.toUtf8();
        block += le32(quint32(utf8.size())) + utf8;
    }
    QByteArray streamInfo(34, '\0');
    streamInfo[10] = char(0x0A); // 44100 Hz, the rest zero
    streamInfo[11] = char(0xC4);
    streamInfo[12] = char(0x42);
    streamInfo[13] = char(0xF0);
    auto header = [](bool last, int type, qsizetype length) {
        QByteArray h(4, '\0');
        h[0] = char((last ? 0x80 : 0) | type);
        h[1] = char((length >> 16) & 0xFF);
        h[2] = char((length >> 8) & 0xFF);
        h[3] = char(length & 0xFF);
        return h;
    };
    return QByteArray("fLaC") + header(false, 0, streamInfo.size()) + streamInfo + header(true, 4, block.size()) +
           block;
}

bool writeFile(QString const &path, QByteArray const &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

FileInfoSection const *section(FileInfoResult const &info, QString const &title) {
    for(auto const &s : info.sections)
        if(s.title == title)
            return &s;
    return nullptr;
}

QString field(FileInfoSection const *s, QString const &name) {
    if(!s)
        return {};
    for(auto const &f : s->fields)
        if(f.name == name)
            return f.value;
    return {};
}

QStringList titles(FileInfoResult const &info) {
    QStringList out;
    for(auto const &s : info.sections)
        out << s.title;
    return out;
}

} // namespace

void Test_AudioThumbs::initTestCase() {
    // Keep the test out of the developer's real config directory.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-test");
    settings = Settings::getInstance();
    QVERIFY(settings != nullptr);
    QVERIFY2(settings->audioPlayback(), "built without USE_MPV: nothing would be detected as audio");
}

void Test_AudioThumbs::coverThumbnail_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("format");
    QTest::addColumn<QColor>("color");

    QTest::newRow("flac PICTURE block") << "flac-picture.flac" << "FLAC" << kRed;
    QTest::newRow("mp4 covr") << "aac-cover.m4a" << "M4A" << kRed;
    QTest::newRow("id3v2.4 APIC") << "mp3-id3v24.mp3" << "MP3" << kRed;
    QTest::newRow("id3v2.3 APIC, jpeg") << "mp3-id3v23.mp3" << "MP3" << kBlue;
    QTest::newRow("vorbis METADATA_BLOCK_PICTURE") << "vorbis-picture.ogg" << "OGG" << kRed;
    QTest::newRow("opus METADATA_BLOCK_PICTURE") << "opus-picture.opus" << "OPUS" << kRed;
    QTest::newRow("matroska attachment") << "matroska-cover.mka" << "MKA" << kRed;
    QTest::newRow("aiff ID3 chunk") << "aiff-id3.aiff" << "AIFF" << kRed;
}

// The covers are 16x16, so both modes enlarge them to fill the whole square,
// exactly as createThumbnail() does for a 16x16 image file.
void Test_AudioThumbs::coverThumbnail() {
    QFETCH(QString, file);
    QFETCH(QString, format);
    QFETCH(QColor, color);
    QString const path = audioPath(file);
    QCOMPARE(int(DocumentInfo(path).type()), int(AUDIO));

    for(bool const crop : {false, true}) {
        for(int const size : {64, 101}) {
            auto const thumb = ThumbnailerRunnable::generate(nullptr, path, size, crop, false);
            QVERIFY(thumb);
            QVERIFY(thumb->kind() == Thumbnail::Kind::Audio);
            QCOMPARE(thumb->name(), file);
            QCOMPARE(thumb->size(), size);
            QCOMPARE(thumb->info(), format);
            QVERIFY2(!hasDimensions(thumb->info()), qPrintable(thumb->info()));

            QImage const image = thumbnailImage(thumb);
            QCOMPARE(image.size(), QSize(size, size));
            for(QPoint const p : {QPoint(size / 2, size / 2), QPoint(1, 1), QPoint(size - 2, size - 2)}) {
                QColor const actual = image.pixelColor(p);
                QVERIFY2(sameColor(actual, color), qPrintable(QStringLiteral("crop %1, size %2, at (%3, %4): %5")
                                                                  .arg(crop)
                                                                  .arg(size)
                                                                  .arg(p.x())
                                                                  .arg(p.y())
                                                                  .arg(describe(actual))));
            }
        }
    }
}

// A 600x200 cover in thirds: red, green, blue. Fitted it keeps all three;
// cropped to a square it is the middle third only. Large enough to be scaled
// down rather than up, and wide enough that the crop needs more than fits in
// size x size -- the path that reads the cover a second time.
void Test_AudioThumbs::nonSquareSidecarCover() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const track = copyInto(dir, audioPath("wav-info.wav"), "track.wav");
    QVERIFY(!track.isEmpty());
    QVERIFY(saveTricolourCover(dir.filePath("cover.png")));

    int const size = 50;
    auto const fitted = ThumbnailerRunnable::generate(nullptr, track, size, false, false);
    QVERIFY(fitted);
    QVERIFY(fitted->kind() == Thumbnail::Kind::Audio);
    QCOMPARE(fitted->info(), QStringLiteral("WAV"));
    QImage const fittedImage = thumbnailImage(fitted);
    QCOMPARE(fittedImage.size(), QSize(600, 200).scaled(size, size, Qt::KeepAspectRatio));
    int const midY = fittedImage.height() / 2;
    QVERIFY2(sameColor(fittedImage.pixelColor(4, midY), kRed), qPrintable(describe(fittedImage.pixelColor(4, midY))));
    QVERIFY2(sameColor(fittedImage.pixelColor(25, midY), kGreen),
             qPrintable(describe(fittedImage.pixelColor(25, midY))));
    QVERIFY2(sameColor(fittedImage.pixelColor(45, midY), kBlue),
             qPrintable(describe(fittedImage.pixelColor(45, midY))));

    auto const cropped = ThumbnailerRunnable::generate(nullptr, track, size, true, false);
    QVERIFY(cropped);
    QCOMPARE(cropped->info(), QStringLiteral("WAV"));
    QImage const croppedImage = thumbnailImage(cropped);
    QCOMPARE(croppedImage.size(), QSize(size, size));
    for(QPoint const p : {QPoint(3, 3), QPoint(25, 25), QPoint(46, 46), QPoint(3, 46), QPoint(46, 3)})
        QVERIFY2(sameColor(croppedImage.pixelColor(p), kGreen), qPrintable(describe(croppedImage.pixelColor(p))));
}

void Test_AudioThumbs::noCover() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString const track = copyInto(dir, audioPath("wav-info.wav"), "track.wav");
    QVERIFY(!track.isEmpty());
    QCOMPARE(int(DocumentInfo(track).type()), int(AUDIO));

    for(bool const crop : {false, true}) {
        auto const thumb = ThumbnailerRunnable::generate(nullptr, track, 64, crop, false);
        QVERIFY(thumb);
        // Kind::Audio with no image is what makes the widget draw the music
        // note rather than the error icon.
        QVERIFY(thumb->kind() == Thumbnail::Kind::Audio);
        QCOMPARE(thumb->pixmap()->width(), 0);
        QCOMPARE(thumb->info(), QStringLiteral("WAV"));
        QCOMPARE(thumb->name(), QStringLiteral("track.wav"));
        QCOMPARE(thumb->size(), 64);
    }
}

// The type label now comes from detection rather than the cached image; an
// image must still say what it always said.
void Test_AudioThumbs::imageLabelUnchanged() {
    QString const path = audioPath("cover-red.png");
    QCOMPARE(int(DocumentInfo(path).type()), int(STATIC));
    auto const thumb = ThumbnailerRunnable::generate(nullptr, path, 64, false, false);
    QVERIFY(thumb);
    QVERIFY(thumb->kind() == Thumbnail::Kind::Generic);
    QCOMPARE(thumb->info(), QStringLiteral("16x16"));
    QCOMPARE(thumbnailImage(thumb).size(), QSize(64, 64));
}

// The cache key ignores the type, so what a cached thumbnail says has to come
// from detection now, not from the label stored with it.
void Test_AudioThumbs::cacheHitUsesCurrentType() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ThumbnailCache cache;
    int const size = 64;
    QStringList planted;
    auto const cleanup = qScopeGuard([&] {
        for(QString const &id : std::as_const(planted))
            QFile::remove(cache.thumbnailPath(id));
    });
    auto const plant = [&](QString const &path, QSize originalSize, QString const &staleLabel,
                           QString const &coverSource = {}) {
        QImage image = plantedThumbnail(path, size, originalSize, staleLabel, coverSource);
        planted << cacheId(path, size, false);
        cache.saveThumbnail(&image, planted.last());
        return cache.exists(planted.last());
    };

    // Ogg audio whose stored label is wrong for what it is now.
    QString const audio = copyInto(dir, audioPath("vorbis-picture.ogg"), "track.ogg");
    QVERIFY(!audio.isEmpty());
    QCOMPARE(int(DocumentInfo(audio).type()), int(AUDIO));
    QVERIFY(plant(audio, QSize(600, 600), " [v]", "embedded"));
    auto const audioThumb = ThumbnailerRunnable::generate(&cache, audio, size, false, false);
    QVERIFY(audioThumb);
    QVERIFY2(sameColor(thumbnailImage(audioThumb).pixelColor(size / 2, size / 2), QColor(Qt::yellow)),
             "the planted thumbnail was not used");
    QVERIFY(audioThumb->kind() == Thumbnail::Kind::Audio);
    QCOMPARE(audioThumb->info(), QStringLiteral("OGG"));

    // One that does not say where its cover came from was made back when the
    // file was played as video, or by a build that did not keep track: it may
    // be of a cover file that has changed since, so it is made again.
    QVERIFY(plant(audio, QSize(600, 600), " [v]"));
    auto const remade = ThumbnailerRunnable::generate(&cache, audio, size, false, false);
    QVERIFY(remade);
    QVERIFY2(sameColor(centre(remade), kRed), qPrintable(describe(centre(remade))));
    QCOMPARE(remade->info(), QStringLiteral("OGG"));

    // An image whose stored label no longer matches what it is.
    QString const image = copyInto(dir, audioPath("cover-red.png"), "still.png");
    QVERIFY(!image.isEmpty());
    QVERIFY(plant(image, QSize(16, 16), " [a]"));
    auto const imageThumb = ThumbnailerRunnable::generate(&cache, image, size, false, false);
    QVERIFY(imageThumb);
    QVERIFY2(sameColor(thumbnailImage(imageThumb).pixelColor(size / 2, size / 2), QColor(Qt::yellow)),
             "the planted thumbnail was not used");
    QVERIFY(imageThumb->kind() == Thumbnail::Kind::Generic);
    QCOMPARE(imageThumb->info(), QStringLiteral("16x16"));
}

// The image rule: worth storing only when the source is larger than the
// thumbnail. A 16x16 cover is cheaper to make again.
void Test_AudioThumbs::cachedOnlyWhenLarger() {
    ThumbnailCache cache;
    int const size = 50;

    QTemporaryDir smallDir;
    QVERIFY(smallDir.isValid());
    QString const small = copyInto(smallDir, audioPath("flac-picture.flac"), "small.flac");
    QVERIFY(!small.isEmpty());
    QVERIFY(ThumbnailerRunnable::generate(&cache, small, size, false, false));
    QVERIFY(!cache.exists(cacheId(small, size, false)));

    QTemporaryDir largeDir;
    QVERIFY(largeDir.isValid());
    QString const large = copyInto(largeDir, audioPath("wav-info.wav"), "large.wav");
    QVERIFY(!large.isEmpty());
    QVERIFY(saveTricolourCover(largeDir.filePath("cover.png")));
    QString const id = cacheId(large, size, true);
    auto const cleanup = qScopeGuard([&] { QFile::remove(cache.thumbnailPath(id)); });
    QVERIFY(ThumbnailerRunnable::generate(&cache, large, size, true, false));
    QVERIFY(cache.exists(id));
    // Read back, it is still an audio thumbnail of the cropped cover.
    auto const hit = ThumbnailerRunnable::generate(&cache, large, size, true, false);
    QVERIFY(hit);
    QVERIFY(hit->kind() == Thumbnail::Kind::Audio);
    QCOMPARE(hit->info(), QStringLiteral("WAV"));
    QImage const hitImage = thumbnailImage(hit);
    QCOMPARE(hitImage.size(), QSize(size, size));
    QVERIFY2(sameColor(hitImage.pixelColor(size / 2, size / 2), kGreen),
             qPrintable(describe(hitImage.pixelColor(size / 2, size / 2))));
}

// The track's own date says nothing about a cover file next to it. Replacing
// that file, deleting it, or adding one that wins over it must each show in
// the next thumbnail, though the cached one is still there and the track has
// not changed.
void Test_AudioThumbs::cachedSidecarCoverFollowsTheFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ThumbnailCache cache;
    int const size = 50;
    QString const first = copyInto(dir, audioPath("wav-info.wav"), "t1.wav");
    QString const second = copyInto(dir, audioPath("wav-info.wav"), "t2.wav");
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QStringList const ids = {cacheId(first, size, true), cacheId(second, size, true)};
    auto const cleanup = qScopeGuard([&] {
        for(QString const &id : ids)
            QFile::remove(cache.thumbnailPath(id));
    });
    auto const thumbnailOf = [&](QString const &track) {
        return ThumbnailerRunnable::generate(&cache, track, size, true, false);
    };

    QString const folderCover = dir.filePath("folder.png");
    QVERIFY(saveSolidCover(folderCover, kRed, 300));
    for(QString const &track : {first, second})
        QVERIFY2(sameColor(centre(thumbnailOf(track)), kRed), qPrintable(track));
    for(QString const &id : ids)
        QVERIFY(cache.exists(id));
    // While nothing changes, the cached one is used: painted over, it shows.
    {
        std::unique_ptr<QImage> cached(cache.readThumbnail(ids.first()));
        QVERIFY(cached);
        cached->fill(Qt::yellow);
        cache.saveThumbnail(cached.get(), ids.first());
    }
    QVERIFY2(sameColor(centre(thumbnailOf(first)), QColor(Qt::yellow)), "the cached thumbnail was not used");

    // Replaced by another picture, a size that also tells it apart where
    // the file system dates it no finer than the first one.
    QVERIFY(saveSolidCover(folderCover, kGreen, 400));
    for(QString const &track : {first, second})
        QVERIFY2(sameColor(centre(thumbnailOf(track)), kGreen), qPrintable(describe(centre(thumbnailOf(track)))));

    // A better name appears next to it.
    QVERIFY(saveSolidCover(dir.filePath("cover.png"), kBlue, 300));
    QVERIFY2(sameColor(centre(thumbnailOf(first)), kBlue), qPrintable(describe(centre(thumbnailOf(first)))));

    // Both go: the placeholder, not a cover that is no longer there.
    QVERIFY(QFile::remove(dir.filePath("cover.png")));
    QVERIFY(QFile::remove(folderCover));
    for(QString const &track : {first, second}) {
        auto const thumb = thumbnailOf(track);
        QVERIFY(thumb);
        QVERIFY(thumb->kind() == Thumbnail::Kind::Audio);
        QCOMPARE(thumb->pixmap()->width(), 0);
        QCOMPARE(thumb->info(), QStringLiteral("WAV"));
    }
}

// The cover's file name goes into the PNG's text, which must bring back a
// name outside Latin-1 as it went in, or the thumbnail would be made again
// every time.
void Test_AudioThumbs::cachedSidecarCoverUnicodeName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ThumbnailCache cache;
    int const size = 50;
    QString const track = copyInto(dir, audioPath("wav-info.wav"), QStringLiteral("\u66F2.wav"));
    QVERIFY(!track.isEmpty());
    QString const id = cacheId(track, size, false);
    auto const cleanup = qScopeGuard([&] { QFile::remove(cache.thumbnailPath(id)); });
    QVERIFY(saveSolidCover(dir.filePath(QStringLiteral("\u66F2.png")), kGreen, 300));
    QVERIFY(sameColor(centre(ThumbnailerRunnable::generate(&cache, track, size, false, false)), kGreen));
    QVERIFY(cache.exists(id));

    // Paint the cached copy, so that only a cache hit is yellow.
    std::unique_ptr<QImage> cached(cache.readThumbnail(id));
    QVERIFY(cached);
    QCOMPARE(cached->text("coverSource"), QStringLiteral("\u66F2.png"));
    cached->fill(Qt::yellow);
    cache.saveThumbnail(cached.get(), id);
    QVERIFY(sameColor(centre(ThumbnailerRunnable::generate(&cache, track, size, false, false)), QColor(Qt::yellow)));
}

void Test_AudioThumbs::fileInfoAudio_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("tagType");
    QTest::addColumn<QStringList>("expectedTags"); // "name=value"
    QTest::addColumn<QString>("pictureMime");      // empty: no picture

    QStringList const all = {"Title=Fixture", "Artist=qimgv", "Album=Test Album", "Album artist=Various",
                             "Track=3/12",    "Date=2026",    "Genre=Ambient"};
    QTest::newRow("flac") << "flac-picture.flac" << "FLAC" << "Vorbis comment" << all << "image/png";
    QTest::newRow("mp3 id3v2.4") << "mp3-id3v24.mp3" << "MP3" << "ID3v2.4" << all << "image/png";
    QTest::newRow("mp3 id3v2.3") << "mp3-id3v23.mp3" << "MP3" << "ID3v2.3" << all << "image/jpeg";
    QTest::newRow("m4a") << "aac-cover.m4a" << "M4A" << "MP4" << all << "image/png";
    QTest::newRow("wav LIST/INFO") << "wav-info.wav" << "WAV" << "RIFF INFO"
                                   << QStringList{"Title=Fixture", "Artist=qimgv"} << QString();
}

void Test_AudioThumbs::fileInfoAudio() {
    QFETCH(QString, file);
    QFETCH(QString, format);
    QFETCH(QString, tagType);
    QFETCH(QStringList, expectedTags);
    QFETCH(QString, pictureMime);

    FileInfoResult const info = extractFileInfo(audioPath(file));
    // Exactly these: no Exif/IPTC/XMP from exiv2, no image section.
    if(pictureMime.isEmpty())
        QCOMPARE(titles(info), (QStringList{"File", "Audio", "Tags"}));
    else
        QCOMPARE(titles(info), (QStringList{"File", "Audio", "Tags", "Picture"}));

    FileInfoSection const *audio = section(info, "Audio");
    QCOMPARE(field(audio, "Format"), format);
    QVERIFY(!field(audio, "MIME type").isEmpty());
    // Nothing the image probes would add: they are skipped for audio.
    for(auto const &f : audio->fields)
        QVERIFY2(f.name == "Format" || f.name == "MIME type" || f.name == "Kind", qPrintable(f.name));

    FileInfoSection const *tags = section(info, "Tags");
    for(QString const &expected : expectedTags) {
        QString const name = expected.section('=', 0, 0);
        QCOMPARE(field(tags, name), expected.section('=', 1));
    }
    // First, as what the rows below it come from.
    QCOMPARE(tags->fields.first().name, QStringLiteral("Tag format"));
    QVERIFY2(field(tags, "Tag format").contains(tagType), qPrintable(field(tags, "Tag format")));
    QVERIFY(field(tags, "More fields").isEmpty());

    if(!pictureMime.isEmpty()) {
        FileInfoSection const *picture = section(info, "Picture");
        QCOMPARE(field(picture, "MIME type"), pictureMime);
        QCOMPARE(field(picture, "Resolution"), QStringLiteral("16 x 16"));
        QVERIFY(!field(picture, "Size").isEmpty());
    }
}

// The format section is named for what the file is.
void Test_AudioThumbs::fileInfoSectionTitles() {
    FileInfoResult const image = extractFileInfo(audioPath("cover-red.png"));
    QVERIFY(section(image, "Image"));
    QCOMPARE(field(section(image, "Image"), "Resolution"), QStringLiteral("16 x 16"));

    QString const video = audioPath("theora-video.ogg");
    QCOMPARE(int(DocumentInfo(video).type()), int(VIDEO));
    FileInfoResult const videoInfo = extractFileInfo(video);
    QVERIFY2(section(videoInfo, "Video"), qPrintable(titles(videoInfo).join(", ")));
    QVERIFY(!section(videoInfo, "Image"));
}

// The mime database names a container, and an audio-only .mp4 or .wma is
// "video" to it. Under the Audio heading that is left out, and the same the
// other way round.
void Test_AudioThumbs::fileInfoMimeTypeAgreesWithSection() {
    for(char const *file : {"aac-isom.mp4", "aac-cover.m4a", "alac.m4a", "wma.wma", "opus-audio-only.webm",
                            "matroska-cover.mka", "flac-picture.flac", "vorbis-picture.ogg"}) {
        QString const path = audioPath(QString::fromLatin1(file));
        QCOMPARE(int(DocumentInfo(path).type()), int(AUDIO));
        FileInfoResult const info = extractFileInfo(path);
        FileInfoSection const *audio = section(info, "Audio");
        QVERIFY2(audio, file);
        QVERIFY2(!field(audio, "MIME type").startsWith("video/"), qPrintable(field(audio, "MIME type")));
        QVERIFY2(!field(audio, "Kind").contains("video", Qt::CaseInsensitive), qPrintable(field(audio, "Kind")));
    }
    QString const video = audioPath("video-misnamed.mka");
    QCOMPARE(int(DocumentInfo(video).type()), int(VIDEO));
    FileInfoResult const videoInfo = extractFileInfo(video);
    FileInfoSection const *videoSection = section(videoInfo, "Video");
    QVERIFY(videoSection);
    QVERIFY2(!field(videoSection, "MIME type").startsWith("audio/"), qPrintable(field(videoSection, "MIME type")));
    QCOMPARE(field(videoSection, "Format"), QStringLiteral("MKA"));
}

// Each row is widgets built on the GUI thread, so the panel gets the start of
// a huge tag and a count of the rest. The common fields are never cut,
// wherever in the tag they are.
void Test_AudioThumbs::fileInfoManyTags() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList comments;
    for(int i = 0; i < 3000; i++)
        comments << QStringLiteral("CUSTOM%1=value %1").arg(i);
    comments << QStringLiteral("TITLE=Last of all") << QStringLiteral("ARTIST=qimgv");
    QString const path = dir.filePath("huge.flac");
    QVERIFY(writeFile(path, flacWithComments(comments)));
    QCOMPARE(int(DocumentInfo(path).type()), int(AUDIO));

    FileInfoResult const info = extractFileInfo(path);
    QCOMPARE(titles(info), (QStringList{"File", "Audio", "Tags"}));
    FileInfoSection const *tags = section(info, "Tags");
    QCOMPARE(tags->fields.first().name, QStringLiteral("Tag format"));
    QCOMPARE(field(tags, "Title"), QStringLiteral("Last of all"));
    QCOMPARE(field(tags, "Artist"), QStringLiteral("qimgv"));
    QCOMPARE(field(tags, "CUSTOM0"), QStringLiteral("value 0"));
    QCOMPARE(field(tags, "CUSTOM99"), QStringLiteral("value 99"));
    QVERIFY(field(tags, "CUSTOM100").isEmpty());
    QCOMPARE(tags->fields.last().name, QStringLiteral("More fields"));
    // The reader stops at a thousand or so; of those, the panel shows 100.
    int kept = 0;
    for(AudioField const &f : AudioMetadataReader::read(path).fields)
        kept += f.common ? 0 : 1;
    QVERIFY(kept > 900);
    QCOMPARE(tags->fields.last().value, QStringLiteral("%1 not shown").arg(kept - 100));
    QCOMPARE(tags->fields.size(), 1 + 100 + 2 + 1);
}

QTEST_MAIN(Test_AudioThumbs)
#include "test_audiothumbs.moc"
