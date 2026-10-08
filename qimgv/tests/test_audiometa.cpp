// The reader as the app uses it: which parsers a file gets, decided by its content; the order in which its
// tags fill each other's gaps; cover decoding with its size limits; cover files next to the audio; reads on
// several threads at once; and all of it over truncated and corrupted copies of every sample. The parsers' own
// details are tested in test_audiometa_a.cpp and test_audiometa_b.cpp.
#include <QtTest>
#include <QBuffer>
#include <QColor>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>
#include <QLoggingCategory>
#include <QMutex>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThreadPool>

#include "audiometa_p.h"
#include "audiometadata.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/time.h>
#endif

using namespace Qt::StringLiterals;

namespace {

constexpr int kTags = AudioMetadataReader::Tags;
constexpr int kCover = AudioMetadataReader::Cover;

QString dataPath(QString const &name) {
    return QStringLiteral(QIMGV_TEST_DATA_DIR "/audio/") + name;
}

QByteArray readFile(QString const &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray readFixture(QString const &name) {
    return readFile(dataPath(name));
}

QByteArray redPng() {
    return readFixture(u"cover-red.png"_s);
}

// QFile can set a file's dates, but not a folder's: it does not open folders.
bool setFolderDate(QString const &path, QDateTime const &date) {
    const qint64 ms = date.toMSecsSinceEpoch();
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE folder = CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if(folder == INVALID_HANDLE_VALUE)
        return false;
    // 100 ns ticks since 1601.
    const quint64 ticks = quint64(ms + 11644473600000LL) * 10000;
    const FILETIME time{DWORD(ticks & 0xFFFFFFFF), DWORD(ticks >> 32)};
    const bool ok = SetFileTime(folder, nullptr, nullptr, &time);
    CloseHandle(folder);
    return ok;
#else
    timeval times[2];
    times[0].tv_sec = times[1].tv_sec = time_t(ms / 1000);
    times[0].tv_usec = times[1].tv_usec = suseconds_t((ms % 1000) * 1000);
    return utimes(QFile::encodeName(path).constData(), times) == 0;
#endif
}

bool writeFile(QString const &path, QByteArray const &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

bool hasJpegPlugin() {
    return QImageReader::supportedImageFormats().contains("jpeg");
}

int fieldCount(AudioMetadata const &meta, QString const &key) {
    int count = 0;
    for(AudioField const &field : meta.fields)
        count += field.key == key ? 1 : 0;
    return count;
}

bool sameFields(QList<AudioField> const &a, QList<AudioField> const &b) {
    if(a.size() != b.size())
        return false;
    for(qsizetype i = 0; i < a.size(); i++) {
        if(a.at(i).key != b.at(i).key || a.at(i).value != b.at(i).value)
            return false;
    }
    return true;
}

// The covers are solid colours. JPEG and smooth scaling may shift a channel a little, never by this much.
bool hasColour(QImage const &image, QPoint at, QColor const &colour) {
    if(image.isNull() || !image.rect().contains(at))
        return false;
    const QColor pixel = image.pixelColor(at);
    return qAbs(pixel.red() - colour.red()) < 48 && qAbs(pixel.green() - colour.green()) < 48 &&
           qAbs(pixel.blue() - colour.blue()) < 48;
}

bool hasColour(QImage const &image, QColor const &colour) {
    return hasColour(image, QPoint(image.width() / 2, image.height() / 2), colour);
}

// ---------------------------------------------------------------------------------------------------------
// Byte builders
// ---------------------------------------------------------------------------------------------------------

QByteArray be(quint64 value, int bytes) {
    QByteArray out;
    for(int i = bytes - 1; i >= 0; i--)
        out.append(char((value >> (8 * i)) & 0xFF));
    return out;
}

QByteArray le(quint64 value, int bytes) {
    QByteArray out;
    for(int i = 0; i < bytes; i++)
        out.append(char((value >> (8 * i)) & 0xFF));
    return out;
}

QByteArray syncsafe(quint32 value) {
    QByteArray out;
    for(int shift = 21; shift >= 0; shift -= 7)
        out.append(char((value >> shift) & 0x7F));
    return out;
}

QByteArray id3Tag(int version, QByteArray const &frames) {
    return "ID3"_ba + char(version) + '\0' + '\0' + syncsafe(quint32(frames.size())) + frames;
}

QByteArray id3Frame(int version, QByteArray const &id, QByteArray const &data) {
    const QByteArray size = version == 4 ? syncsafe(quint32(data.size())) : be(quint64(data.size()), 4);
    return id + size + "\0\0"_ba + data;
}

// Latin-1, which is all these samples need.
QByteArray id3Text(int version, QByteArray const &id, QByteArray const &text) {
    return id3Frame(version, id, '\0' + text);
}

QByteArray id3Picture(int version, int type, QByteArray const &mime, QByteArray const &image) {
    return id3Frame(version, "APIC", '\0' + mime + '\0' + char(type) + '\0' + image);
}

QByteArray id3v1(QByteArray const &title, QByteArray const &artist, QByteArray const &album, QByteArray const &year,
                 QByteArray const &comment, int track, int genre) {
    auto field = [](QByteArray value, int size) {
        value.truncate(size);
        return value + QByteArray(size - value.size(), '\0');
    };
    return "TAG"_ba + field(title, 30) + field(artist, 30) + field(album, 30) + field(year, 4) + field(comment, 28) +
           '\0' + char(track) + char(genre);
}

QByteArray apeItem(QByteArray const &key, QByteArray const &value) {
    return le(quint64(value.size()), 4) + le(0, 4) + key + '\0' + value;
}

// APEv2 with a header and a footer.
QByteArray apeTag(QList<QByteArray> const &items) {
    const QByteArray body = items.join();
    auto block = [&](quint32 flags) {
        return "APETAGEX"_ba + le(2000, 4) + le(quint64(body.size() + 32), 4) + le(quint64(items.size()), 4) +
               le(flags, 4) + QByteArray(8, '\0');
    };
    return block(0xA0000000) + body + block(0x80000000);
}

QByteArray riffChunk(QByteArray const &id, QByteArray const &data) {
    return id + le(quint64(data.size()), 4) + data + (data.size() % 2 ? "\0"_ba : QByteArray());
}

QByteArray aiffChunk(QByteArray const &id, QByteArray const &data) {
    return id + be(quint64(data.size()), 4) + data + (data.size() % 2 ? "\0"_ba : QByteArray());
}

QByteArray flacBlock(int type, QByteArray const &data, bool last = false) {
    return char((last ? 0x80 : 0) | type) + be(quint64(data.size()), 3) + data;
}

QByteArray vorbisComment(QList<QByteArray> const &entries) {
    const QByteArray vendor = "qimgv tests";
    QByteArray out = le(quint64(vendor.size()), 4) + vendor + le(quint64(entries.size()), 4);
    for(QByteArray const &entry : entries)
        out += le(quint64(entry.size()), 4) + entry;
    return out;
}

QByteArray flacPicture(int type, QByteArray const &mime, QByteArray const &image) {
    return be(quint64(type), 4) + be(quint64(mime.size()), 4) + mime + be(0, 4) + be(16, 4) + be(16, 4) + be(24, 4) +
           be(0, 4) + be(quint64(image.size()), 4) + image;
}

// A FLAC file with a title and, if given, one front cover. No audio frames: no parser looks for them.
QByteArray flacFile(QByteArray const &title, QByteArray const &picture = {}) {
    QByteArray out = "fLaC" + flacBlock(0, QByteArray(34, '\0')) +
                     flacBlock(4, vorbisComment({"TITLE=" + title}), picture.isEmpty());
    if(!picture.isEmpty())
        out += flacBlock(6, flacPicture(3, "image/png", picture), true);
    return out;
}

quint32 crc32(QByteArray const &data) {
    quint32 crc = 0xFFFFFFFF;
    for(char c : data) {
        crc ^= quint8(c);
        for(int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

QByteArray pngChunk(QByteArray const &type, QByteArray const &data) {
    return be(quint64(data.size()), 4) + type + data + be(crc32(type + data), 4);
}

// A valid PNG signature, header and an empty compressed stream: all a decoder needs to report the size, and
// nothing that could be decoded at that size.
QByteArray pngHeaderOnly(quint32 width, quint32 height) {
    const QByteArray header = be(width, 4) + be(height, 4) + "\x08\x02\0\0\0"_ba;
    return "\x89PNG\r\n\x1A\n"_ba + pngChunk("IHDR", header) + pngChunk("IDAT", "\x78\x9C\x03\0\0\0\0\x01"_ba) +
           pngChunk("IEND", {});
}

QByteArray encode(QImage const &image, const char *format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if(!buffer.open(QIODevice::WriteOnly) || !QImageWriter(&buffer, format).write(image))
        return {};
    return bytes;
}

QImage solid(QSize size, QColor const &colour) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

// Red on the left half, blue on the right.
QImage halves(QSize size) {
    QImage image = solid(size, Qt::red);
    for(int y = 0; y < size.height(); y++) {
        for(int x = size.width() / 2; x < size.width(); x++)
            image.setPixelColor(x, y, Qt::blue);
    }
    return image;
}

// An EXIF APP1 segment right after the JPEG's SOI: a little-endian TIFF header and one IFD holding nothing
// but the orientation.
QByteArray withExifOrientation(QByteArray const &jpeg, quint16 orientation) {
    const QByteArray tiff = "II*\0"_ba + le(8, 4) + le(1, 2) + le(0x0112, 2) + le(3, 2) + le(1, 4) +
                            le(orientation, 2) + le(0, 2) + le(0, 4);
    const QByteArray app1 = "Exif\0\0"_ba + tiff;
    return jpeg.left(2) + "\xFF\xE1"_ba + be(quint64(app1.size() + 2), 2) + app1 + jpeg.mid(2);
}

// MPEG audio frames without any tag, to put tags around.
QByteArray mpegFrames() {
    return readFixture(u"mp3-bare.mp3"_s);
}

QByteArray apeSample() {
    return apeTag({apeItem("Title", "Ape Title"), apeItem("Artist", "Ape Artist"), apeItem("Album", "Ape Album"),
                   apeItem("Composer", "Ape Composer")});
}

QByteArray id3v1Sample() {
    return id3v1("V1 Title", "V1 Artist", "V1 Album", "1999", "V1 Comment", 5, 17);
}

// ID3v2, APEv2 and ID3v1 on one MP3, disagreeing about everything.
QByteArray mp3WithThreeTags() {
    const QByteArray v2 = id3Tag(4, id3Text(4, "TIT2", "Id3v2 Title") + id3Text(4, "TPE1", "Id3v2 Artist"));
    return v2 + mpegFrames() + apeSample() + id3v1Sample();
}

QByteArray id3Prefix() {
    return id3Tag(3, id3Text(3, "TIT2", "Id3 Title") + id3Text(3, "TCOM", "Id3 Composer"));
}

// ---------------------------------------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------------------------------------

struct Fixture {
    const char *file;
    QStringList tagTypes;
    // Carries the tags make-fixtures.sh lists.
    bool tagged;
    // Empty where the format has no album artist; ID3v1 has a track number but no total.
    const char *albumArtist;
    const char *track;
    QColor cover;
};

QList<Fixture> fixtureTable() {
    const QColor red(Qt::red), blue(Qt::blue), none;
    const QStringList untagged;
    return {
        {"aac-cover.m4a", {u"MP4"_s}, true, "Various", "3/12", red},
        {"aac-isom.mp4", {u"MP4"_s}, true, "Various", "3/12", none},
        {"ac3.ac3", untagged, false, "", "", none},
        {"aiff-id3.aiff", {u"ID3v2.4"_s, u"AIFF"_s}, true, "Various", "3/12", red},
        {"alac.m4a", {u"MP4"_s}, true, "Various", "3/12", none},
        // CAF keeps its tags in an 'info' chunk, which is not read; ffmpeg put only its encoder name there.
        {"apple.caf", untagged, false, "", "", none},
        {"flac-in-ogg.oga", {u"Vorbis comment"_s}, true, "Various", "3/12", none},
        {"flac-picture.flac", {u"Vorbis comment"_s}, true, "Various", "3/12", red},
        {"matroska-cover.mka", {u"Matroska"_s}, true, "Various", "3/12", red},
        {"mp3-bare.mp3", untagged, false, "", "", none},
        // ID3v1 has no album artist, and only a track number.
        {"mp3-id3v1.mp3", {u"ID3v1"_s}, true, "", "3", none},
        {"mp3-id3v23.mp3", {u"ID3v2.3"_s}, true, "Various", "3/12", blue},
        {"mp3-id3v24.mp3", {u"ID3v2.4"_s}, true, "Various", "3/12", red},
        // A video's tags are read all the same; ffmpeg gave this one only its encoder name.
        {"mpeg4-video.mp4", {u"MP4"_s}, false, "", "", none},
        {"opus-audio-only.webm", {u"Matroska"_s}, true, "Various", "3/12", none},
        {"opus-picture.opus", {u"Vorbis comment"_s}, true, "Various", "3/12", red},
        {"speex.spx", {u"Vorbis comment"_s}, true, "Various", "3/12", none},
        {"sun.au", untagged, false, "", "", none},
        {"theora-video.ogg", untagged, false, "", "", none},
        {"tta.tta", {u"APEv2"_s}, true, "Various", "3/12", none},
        {"video-misnamed.mka", {u"Matroska"_s}, false, "", "", none},
        {"vorbis-picture.ogg", {u"Vorbis comment"_s}, true, "Various", "3/12", red},
        {"vp8-video.webm", {u"Matroska"_s}, false, "", "", none},
        // RIFF INFO has no album artist; the id3 chunk behind it fills the gap.
        {"wav-id3.wav", {u"RIFF INFO"_s, u"ID3v2.4"_s}, true, "Various", "3/12", red},
        {"wav-info.wav", {u"RIFF INFO"_s}, true, "", "3/12", none},
        {"wavpack-ape.wv", {u"APEv2"_s}, true, "Various", "3/12", none},
        {"wma.wma", {u"ASF"_s}, true, "Various", "3/12", none},
    };
}

// make-fixtures.sh asks ffmpeg 8 for two structures it silently leaves out. Until those fixtures are built
// another way, their rows have nothing to check.
QString missingStructure(QString const &file, QByteArray const &bytes) {
    if(file == u"mp3-id3v1.mp3"_s && !bytes.right(128).startsWith("TAG"))
        return u"mp3-id3v1.mp3 has no ID3v1 tag: ffmpeg ignores -write_id3v1 together with -id3v2_version 0"_s;
    if(file == u"wav-id3.wav"_s && !bytes.contains("id3 ") && !bytes.contains("ID3 "))
        return u"wav-id3.wav has no id3 chunk: ffmpeg's wav muxer has no -write_id3v2 option"_s;
    return {};
}

void addFixtureRows(bool withCover) {
    for(Fixture const &fixture : fixtureTable()) {
        if(fixture.cover.isValid() == withCover)
            QTest::newRow(fixture.file) << QString::fromLatin1(fixture.file) << fixture.cover;
    }
}

} // namespace

class Test_Audiometa : public QObject {
    Q_OBJECT

private slots:
    void everyFixtureHasARow();
    void fixtures_data();
    void fixtures();
    void contentNotExtension_data();
    void contentNotExtension();
    void unreadableFiles();

    void flacBehindId3v2();
    void oggBehindId3v2();
    void consecutiveId3v2Tags();
    void mp3TagPriority();
    void endTags_data();
    void endTags();
    void nativeContainersIgnoreEndTags_data();
    void nativeContainersIgnoreEndTags();
    void containerSignatures_data();
    void containerSignatures();
    void partsMask_data();
    void partsMask();

    void loadCover_data();
    void loadCover();
    void loadCoverWithoutCover_data();
    void loadCoverWithoutCover();
    void loadCoverScalesSmoothly();
    void loadCoverExifOrientation();
    void decompressionBomb_data();
    void decompressionBomb();

    void sidecarPreferenceOrder();
    void sidecarIgnores();
    void sidecarHiddenOnWindows();
    void sidecarListingFollowsFolderDate();
    void embeddedCoverBeatsSidecar();
    void sidecarWithoutEmbeddedCover();
    void sidecarWhenEmbeddedDoesNotDecode();
    void sidecarTooLarge();

    void concurrentReads();
    void robustness();
};

// ---------------------------------------------------------------------------------------------------------
// Fixtures, end to end
// ---------------------------------------------------------------------------------------------------------

// A fixture added to make-fixtures.sh gets a row here, or this fails.
void Test_Audiometa::everyFixtureHasARow() {
    QStringList known;
    for(Fixture const &fixture : fixtureTable())
        known.append(QString::fromLatin1(fixture.file));
    const QStringList files = QDir(dataPath({})).entryList(QDir::Files);
    QVERIFY(files.size() > 20);
    for(QString const &file : files) {
        if(file != u"make-fixtures.sh"_s && file != u"cover-red.png"_s)
            QVERIFY2(known.contains(file), qPrintable(file));
    }
}

void Test_Audiometa::fixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QStringList>("tagTypes");
    QTest::addColumn<bool>("tagged");
    QTest::addColumn<QString>("albumArtist");
    QTest::addColumn<QString>("track");
    QTest::addColumn<QColor>("cover");
    for(Fixture const &f : fixtureTable()) {
        QTest::newRow(f.file) << QString::fromLatin1(f.file) << f.tagTypes << f.tagged
                              << QString::fromLatin1(f.albumArtist) << QString::fromLatin1(f.track) << f.cover;
    }
}

void Test_Audiometa::fixtures() {
    QFETCH(QString, file);
    QFETCH(QStringList, tagTypes);
    QFETCH(bool, tagged);
    QFETCH(QString, albumArtist);
    QFETCH(QString, track);
    QFETCH(QColor, cover);
    const QByteArray bytes = readFixture(file);
    QVERIFY2(!bytes.isEmpty(), qPrintable(dataPath(file)));
    const QString missing = missingStructure(file, bytes);
    if(!missing.isEmpty())
        QSKIP(qPrintable(missing));

    const AudioMetadata meta = AudioMetadataReader::read(dataPath(file));
    QCOMPARE(meta.tagTypes, tagTypes);
    if(tagged) {
        QCOMPARE(meta.title, u"Fixture"_s);
        QCOMPARE(meta.artist, u"qimgv"_s);
        QCOMPARE(meta.album, u"Test Album"_s);
        QCOMPARE(meta.albumArtist, albumArtist);
        QCOMPARE(meta.track, track);
        QCOMPARE(meta.date, u"2026"_s);
        QCOMPARE(meta.genre, u"Ambient"_s);
    } else {
        QVERIFY(meta.title.isEmpty());
        QVERIFY(meta.artist.isEmpty());
        QVERIFY(meta.album.isEmpty());
    }
    // A well-known field is listed once, however many tags carry it.
    using AudioMeta::Field;
    for(Field field : {Field::Title, Field::Artist, Field::Album, Field::AlbumArtist, Field::Date, Field::Genre,
                       Field::Track, Field::Disc, Field::Composer, Field::Comment})
        QVERIFY2(fieldCount(meta, AudioMeta::fieldName(field)) <= 1, qPrintable(AudioMeta::fieldName(field)));

    if(cover == QColor(Qt::red)) {
        QCOMPARE(meta.cover.data, redPng());
    } else if(cover == QColor(Qt::blue)) {
        QVERIFY(meta.cover.data.startsWith("\xFF\xD8\xFF"));
        QVERIFY(meta.cover.data.endsWith("\xFF\xD9"));
    } else {
        QVERIFY(meta.cover.isNull());
    }
}

void Test_Audiometa::contentNotExtension_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<QString>("name");
    QTest::newRow("FLAC as .mp3") << "flac-picture.flac" << "song.mp3";
    QTest::newRow("MP3 as .flac") << "mp3-id3v24.mp3" << "song.flac";
    QTest::newRow("M4A as .ogg") << "aac-cover.m4a" << "song.ogg";
    QTest::newRow("Ogg as .m4a") << "vorbis-picture.ogg" << "song.m4a";
    QTest::newRow("WMA as .wav") << "wma.wma" << "song.wav";
    QTest::newRow("WAV as .wma") << "wav-info.wav" << "song.wma";
    QTest::newRow("Matroska as .aiff") << "matroska-cover.mka" << "song.aiff";
    QTest::newRow("AIFF as .mka") << "aiff-id3.aiff" << "song.mka";
    QTest::newRow("WavPack without one") << "wavpack-ape.wv" << "song";
}

void Test_Audiometa::contentNotExtension() {
    QFETCH(QString, fixture);
    QFETCH(QString, name);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(name), readFixture(fixture)));
    const AudioMetadata original = AudioMetadataReader::read(dataPath(fixture));
    const AudioMetadata renamed = AudioMetadataReader::read(dir.filePath(name));
    QCOMPARE(renamed.title, u"Fixture"_s);
    QCOMPARE(renamed.tagTypes, original.tagTypes);
    QVERIFY(sameFields(renamed.fields, original.fields));
    QCOMPARE(renamed.cover.data, original.cover.data);
}

void Test_Audiometa::unreadableFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missing = dir.filePath(u"missing.mp3"_s);
    const QString empty = dir.filePath(u"empty.mp3"_s);
    QVERIFY(writeFile(empty, {}));
    const QString directory = dir.filePath(u"directory.mp3"_s);
    QVERIFY(QDir(dir.path()).mkpath(directory));
    for(QString const &path : {missing, empty, directory, QString()}) {
        const AudioMetadata meta = AudioMetadataReader::read(path);
        QVERIFY2(meta.isEmpty() && meta.tagTypes.isEmpty(), qPrintable(path));
        QSize originalSize(1, 1);
        QVERIFY(AudioMetadataReader::loadCover(path, QSize(8, 8), &originalSize).isNull());
        QVERIFY(!originalSize.isValid());
    }
    // Asking for nothing gets nothing.
    const AudioMetadata nothing = AudioMetadataReader::read(dataPath(u"flac-picture.flac"_s), 0);
    QVERIFY(nothing.isEmpty() && nothing.tagTypes.isEmpty());
    QVERIFY(AudioMetadataReader::findSidecarCover({}).isEmpty());
}

// ---------------------------------------------------------------------------------------------------------
// Dispatch and priority
// ---------------------------------------------------------------------------------------------------------

// The Vorbis comment is the tag FLAC tools write and show, so it wins over an ID3v2 tag in front of the file,
// which only fills its gaps. The title differs between the two.
void Test_Audiometa::flacBehindId3v2() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"behind.mp3"_s);
    QVERIFY(writeFile(path, id3Prefix() + readFixture(u"flac-picture.flac"_s)));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QCOMPARE(meta.tagTypes, (QStringList{u"Vorbis comment"_s, u"ID3v2.3"_s}));
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.composer, u"Id3 Composer"_s);
    QCOMPARE(fieldCount(meta, AudioMeta::fieldName(AudioMeta::Field::Title)), 1);
    QCOMPARE(meta.cover.data, redPng());
}

void Test_Audiometa::oggBehindId3v2() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"behind.ogg"_s);
    QVERIFY(writeFile(path, id3Prefix() + readFixture(u"vorbis-picture.ogg"_s)));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QCOMPARE(meta.tagTypes, (QStringList{u"Vorbis comment"_s, u"ID3v2.3"_s}));
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.composer, u"Id3 Composer"_s);
    QCOMPARE(meta.cover.data, redPng());
}

// A tagger that missed the tag already there writes a second in front of it. The first one is the newer.
void Test_Audiometa::consecutiveId3v2Tags() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray first = id3Tag(4, id3Text(4, "TIT2", "First") + id3Text(4, "TCOM", "First Composer"));
    const QByteArray second = id3Tag(3, id3Text(3, "TIT2", "Second") + id3Text(3, "TALB", "Second Album"));

    const QString mp3 = dir.filePath(u"two.mp3"_s);
    QVERIFY(writeFile(mp3, first + second + mpegFrames()));
    const AudioMetadata meta = AudioMetadataReader::read(mp3);
    QCOMPARE(meta.tagTypes, (QStringList{u"ID3v2.4"_s, u"ID3v2.3"_s}));
    QCOMPARE(meta.title, u"First"_s);
    QCOMPARE(meta.album, u"Second Album"_s);

    const QString flac = dir.filePath(u"two.flac"_s);
    QVERIFY(writeFile(flac, first + second + readFixture(u"flac-picture.flac"_s)));
    const AudioMetadata native = AudioMetadataReader::read(flac);
    QCOMPARE(native.tagTypes, (QStringList{u"Vorbis comment"_s, u"ID3v2.4"_s, u"ID3v2.3"_s}));
    QCOMPARE(native.title, u"Fixture"_s);
    QCOMPARE(native.composer, u"First Composer"_s);
    QCOMPARE(native.cover.data, redPng());
}

// ID3v2 wins; APEv2 fills what it lacks, then ID3v1 what both lack.
void Test_Audiometa::mp3TagPriority() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"three.mp3"_s);
    QVERIFY(writeFile(path, mp3WithThreeTags()));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QCOMPARE(meta.tagTypes, (QStringList{u"ID3v2.4"_s, u"APEv2"_s, u"ID3v1"_s}));
    QCOMPARE(meta.title, u"Id3v2 Title"_s);
    QCOMPARE(meta.artist, u"Id3v2 Artist"_s);
    QCOMPARE(meta.album, u"Ape Album"_s);
    QCOMPARE(meta.composer, u"Ape Composer"_s);
    QCOMPARE(meta.date, u"1999"_s);
    QCOMPARE(meta.track, u"5"_s);
    QCOMPARE(meta.genre, u"Rock"_s);
    QCOMPARE(meta.comment, u"V1 Comment"_s);
    for(AudioField const &field : meta.fields)
        QVERIFY2(fieldCount(meta, field.key) == 1, qPrintable(field.key));
}

// Formats without a tag of their own keep APEv2 and ID3v1 at the end, and so does whatever is not recognised.
void Test_Audiometa::endTags_data() {
    QTest::addColumn<QByteArray>("head");
    QTest::newRow("Monkey's Audio") << "MAC \x96\x0F"_ba;
    QTest::newRow("WavPack") << "wvpk"_ba;
    QTest::newRow("Musepack SV8") << "MPCK"_ba;
    QTest::newRow("Musepack SV7") << "MP+\x07"_ba;
    QTest::newRow("TTA") << "TTA1"_ba;
    QTest::newRow("TAK") << "tBaK"_ba;
    QTest::newRow("OptimFROG") << "OFR "_ba;
    QTest::newRow("ADTS AAC") << "\xFF\xF1\x50\x80"_ba;
    QTest::newRow("MPEG audio") << mpegFrames();
    QTest::newRow("unknown") << "\x12\x34\x56\x78"_ba;
    QTest::newRow("nothing before the tags") << QByteArray();
}

void Test_Audiometa::endTags() {
    QFETCH(QByteArray, head);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"track"_s);
    QVERIFY(writeFile(path, head + QByteArray(300, '\x55') + apeSample() + id3v1Sample()));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QCOMPARE(meta.tagTypes, (QStringList{u"APEv2"_s, u"ID3v1"_s}));
    QCOMPARE(meta.title, u"Ape Title"_s);
    QCOMPARE(meta.date, u"1999"_s);
}

// Containers with a tag of their own are not looked at for appended ones: what looks like a tag at their end is
// whatever their own structure put there.
void Test_Audiometa::nativeContainersIgnoreEndTags_data() {
    QTest::addColumn<QString>("fixture");
    for(const char *file : {"flac-picture.flac", "vorbis-picture.ogg", "aac-cover.m4a", "wma.wma", "wav-info.wav",
                            "aiff-id3.aiff", "matroska-cover.mka"})
        QTest::newRow(file) << QString::fromLatin1(file);
}

void Test_Audiometa::nativeContainersIgnoreEndTags() {
    QFETCH(QString, fixture);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"track"_s);
    QVERIFY(writeFile(path, readFixture(fixture) + apeSample() + id3v1Sample()));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QVERIFY(!meta.tagTypes.contains(u"APEv2"_s));
    QVERIFY(!meta.tagTypes.contains(u"ID3v1"_s));
    QCOMPARE(meta.title, u"Fixture"_s);
    QVERIFY(meta.composer.isEmpty());
}

// Every signature the reader dispatches on, including the ones no fixture starts with.
void Test_Audiometa::containerSignatures_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("tagType");
    QTest::addColumn<QString>("title");
    const QByteArray info = riffChunk("LIST", "INFO" + riffChunk("INAM", "Signature"));
    for(const char *magic : {"RIFF", "RF64", "BW64"}) {
        QTest::newRow(magic) << QByteArray(magic) + le(quint64(info.size() + 4), 4) + "WAVE" + info << u"RIFF INFO"_s
                             << u"Signature"_s;
    }
    const QByteArray name = aiffChunk("NAME", "Signature");
    for(const char *form : {"AIFF", "AIFC"})
        QTest::newRow(form) << "FORM" + be(quint64(name.size() + 4), 4) + form + name << u"AIFF"_s << u"Signature"_s;

    // DSF points at an ID3v2 tag after the audio.
    const QByteArray tag = id3Tag(3, id3Text(3, "TIT2", "Signature"));
    constexpr quint64 kTagAt = 92;
    QTest::newRow("DSF") << "DSD "_ba + le(28, 8) + le(kTagAt + quint64(tag.size()), 8) + le(kTagAt, 8) +
                                QByteArray(kTagAt - 28, '\0') + tag
                         << u"ID3v2.3"_s << u"Signature"_s;

    // The fixture's boxes are ftyp, free, mdat, moov. QuickTime files from before ftyp start with any of the
    // others; renaming ftyp keeps every offset where it was.
    const QByteArray m4a = readFixture(u"aac-cover.m4a"_s);
    QCOMPARE(m4a.mid(4, 4), "ftyp"_ba);
    for(const char *first : {"free", "skip", "wide", "mdat"}) {
        QByteArray renamed = m4a;
        renamed.replace(4, 4, first);
        QTest::newRow(first) << renamed << u"MP4"_s << u"Fixture"_s;
    }
    const qsizetype ftypEnd = AudioMeta::be32(m4a, 0);
    const qsizetype moov = m4a.indexOf("moov") - 4;
    QVERIFY(moov > ftypEnd && AudioMeta::be32(m4a, moov) == m4a.size() - moov);
    QTest::newRow("moov") << m4a.mid(moov) + m4a.mid(ftypEnd, moov - ftypEnd) << u"MP4"_s << u"Fixture"_s;
}

void Test_Audiometa::containerSignatures() {
    QFETCH(QByteArray, bytes);
    QFETCH(QString, tagType);
    QFETCH(QString, title);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"track"_s);
    QVERIFY(writeFile(path, bytes));
    const AudioMetadata meta = AudioMetadataReader::read(path);
    QCOMPARE(meta.tagTypes, QStringList{tagType});
    QCOMPARE(meta.title, title);
}

// The thumbnailer asks for the cover alone and the audio view for the tags alone.
void Test_Audiometa::partsMask_data() {
    QTest::addColumn<QString>("file");
    for(Fixture const &fixture : fixtureTable()) {
        if(fixture.tagged || fixture.cover.isValid())
            QTest::newRow(fixture.file) << QString::fromLatin1(fixture.file);
    }
}

void Test_Audiometa::partsMask() {
    QFETCH(QString, file);
    const QString path = dataPath(file);
    const AudioMetadata all = AudioMetadataReader::read(path);
    const AudioMetadata cover = AudioMetadataReader::read(path, kCover);
    QVERIFY(cover.fields.isEmpty());
    QVERIFY(cover.title.isEmpty() && cover.artist.isEmpty() && cover.album.isEmpty() && cover.track.isEmpty());
    QCOMPARE(cover.cover.data, all.cover.data);
    const AudioMetadata tags = AudioMetadataReader::read(path, kTags);
    QVERIFY(tags.cover.isNull());
    QVERIFY(sameFields(tags.fields, all.fields));
    QCOMPARE(tags.title, all.title);
}

// ---------------------------------------------------------------------------------------------------------
// Cover decoding
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa::loadCover_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QColor>("colour");
    addFixtureRows(true);
}

void Test_Audiometa::loadCover() {
    QFETCH(QString, file);
    QFETCH(QColor, colour);
    const QString missing = missingStructure(file, readFixture(file));
    if(!missing.isEmpty())
        QSKIP(qPrintable(missing));
    if(colour == QColor(Qt::blue) && !hasJpegPlugin())
        QSKIP("no 'jpeg' image plugin to decode the cover with");
    const QString path = dataPath(file);

    QSize originalSize;
    QImage cover = AudioMetadataReader::loadCover(path, QSize(), &originalSize);
    QCOMPARE(cover.size(), QSize(16, 16));
    QCOMPARE(originalSize, QSize(16, 16));
    QVERIFY(hasColour(cover, colour));
    QVERIFY(hasColour(cover, QPoint(0, 0), colour));

    originalSize = {};
    cover = AudioMetadataReader::loadCover(path, QSize(8, 8), &originalSize);
    QCOMPARE(cover.size(), QSize(8, 8));
    QCOMPARE(originalSize, QSize(16, 16));
    QVERIFY(hasColour(cover, colour));

    // Aspect kept inside a box that is not square.
    QCOMPARE(AudioMetadataReader::loadCover(path, QSize(10, 4)).size(), QSize(4, 4));
    // Never enlarged.
    cover = AudioMetadataReader::loadCover(path, QSize(64, 64), &originalSize);
    QCOMPARE(cover.size(), QSize(16, 16));
    QCOMPARE(originalSize, QSize(16, 16));
    QCOMPARE(AudioMetadataReader::loadCover(path, QSize(16, 16)).size(), QSize(16, 16));
    QCOMPARE(AudioMetadataReader::loadCover(path, QSize(64, 8)).size(), QSize(8, 8));
}

void Test_Audiometa::loadCoverWithoutCover_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QColor>("colour");
    addFixtureRows(false);
}

void Test_Audiometa::loadCoverWithoutCover() {
    QFETCH(QString, file);
    QSize originalSize(1, 1);
    QVERIFY(AudioMetadataReader::loadCover(dataPath(file), QSize(8, 8), &originalSize).isNull());
    QVERIFY(!originalSize.isValid());
}

// PNG has no way to scale while decoding; the scaling after must still average, not pick pixels. One-pixel
// stripes of black and white come out grey, where nearest-neighbour would keep them black or white.
void Test_Audiometa::loadCoverScalesSmoothly() {
    QImage stripes(64, 64, QImage::Format_RGB32);
    for(int x = 0; x < 64; x++) {
        for(int y = 0; y < 64; y++)
            stripes.setPixelColor(x, y, x % 2 ? Qt::white : Qt::black);
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"stripes.flac"_s);
    QVERIFY(writeFile(path, flacFile("Stripes", encode(stripes, "png"))));
    QSize originalSize;
    const QImage cover = AudioMetadataReader::loadCover(path, QSize(16, 16), &originalSize);
    QCOMPARE(cover.size(), QSize(16, 16));
    QCOMPARE(originalSize, QSize(64, 64));
    for(QPoint at : {QPoint(5, 5), QPoint(8, 8), QPoint(10, 3)}) {
        const int grey = qGray(cover.pixel(at));
        QVERIFY2(grey > 64 && grey < 192, qPrintable(QString::number(grey)));
    }
}

// Covers are photos too, and a camera's JPEG is stored sideways with an EXIF note saying so. The size limit
// the caller gives is for the picture as shown.
void Test_Audiometa::loadCoverExifOrientation() {
    if(!hasJpegPlugin())
        QSKIP("no 'jpeg' image plugin");
    // Stored 64x32, red left, blue right. Orientation 6: turn 90 degrees clockwise to show it, which puts the
    // red on top of a 32x64 picture.
    const QByteArray jpeg = withExifOrientation(encode(halves(QSize(64, 32)), "jpeg"), 6);
    QVERIFY(jpeg.size() > 100);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"sideways.mp3"_s);
    // Declared as PNG: the content decides.
    QVERIFY(writeFile(path, id3Tag(3, id3Picture(3, 3, "image/png", jpeg)) + mpegFrames()));

    QSize originalSize;
    QImage cover = AudioMetadataReader::loadCover(path, QSize(), &originalSize);
    QCOMPARE(cover.size(), QSize(32, 64));
    QCOMPARE(originalSize, QSize(32, 64));
    QVERIFY(hasColour(cover, QPoint(16, 12), Qt::red));
    QVERIFY(hasColour(cover, QPoint(16, 52), Qt::blue));

    // JPEG scales while decoding, in the stored orientation: the box must be turned for it.
    cover = AudioMetadataReader::loadCover(path, QSize(16, 16), &originalSize);
    QCOMPARE(cover.size(), QSize(8, 16));
    QCOMPARE(originalSize, QSize(32, 64));
    QVERIFY(hasColour(cover, QPoint(4, 3), Qt::red));
    QVERIFY(hasColour(cover, QPoint(4, 13), Qt::blue));
}

void Test_Audiometa::decompressionBomb_data() {
    QTest::addColumn<QByteArray>("png");
    QTest::addColumn<QSize>("claimed");
    QTest::addColumn<QSize>("maxSize");
    QTest::addColumn<QSize>("expected");
    const QSize refused;
    // 30 GB as 8-bit RGB, in 65 bytes.
    QTest::newRow("header claims 100000 x 100000")
        << pngHeaderOnly(100000, 100000) << QSize(100000, 100000) << QSize(64, 64) << refused;
    // These two would decode, into 80 KB and 400 MB: only the limits stop them.
    QTest::newRow("wider than any cover")
        << encode(solid(QSize(20000, 1), Qt::red), "png") << QSize(20000, 1) << QSize() << refused;
    QImage mono(16384, 6104, QImage::Format_Mono);
    mono.fill(0);
    QTest::newRow("over 100 megapixels") << encode(mono, "png") << QSize(16384, 6104) << QSize() << refused;
    QTest::newRow("as wide as allowed") << encode(solid(QSize(16384, 1), Qt::red), "png") << QSize(16384, 1)
                                        << QSize(64, 64) << QSize(64, 1);
}

// Pictures come from files downloaded from anywhere and are decoded on worker threads; a header claiming an
// absurd size is refused before anything is allocated for it, embedded or next to the file.
void Test_Audiometa::decompressionBomb() {
    QFETCH(QByteArray, png);
    QFETCH(QSize, claimed);
    QFETCH(QSize, maxSize);
    QFETCH(QSize, expected);
    QVERIFY(!png.isEmpty());
    // The header is valid: a reader asked for the size gets the claimed one, so the refusal below is ours.
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QCOMPARE(QImageReader(&buffer).size(), claimed);

    // Qt's own allocation limit would refuse some of these too; the app raises it to the user's setting, 1 GB
    // by default, so it is no safeguard there.
    const int allocationLimit = QImageReader::allocationLimit();
    QImageReader::setAllocationLimit(1024);
    const auto restore = qScopeGuard([&] { QImageReader::setAllocationLimit(allocationLimit); });

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString embedded = dir.filePath(u"embedded.flac"_s);
    QVERIFY(writeFile(embedded, flacFile("Bomb", png)));
    // The tag reader hands the bytes over as they are; deciding what to decode is loadCover's job.
    QCOMPARE(AudioMetadataReader::read(embedded).cover.data, png);
    QSize originalSize;
    QElapsedTimer timer;
    timer.start();
    QImage cover = AudioMetadataReader::loadCover(embedded, maxSize, &originalSize);
    if(expected.isValid()) {
        QCOMPARE(cover.size(), expected);
        QCOMPARE(originalSize, claimed);
        QVERIFY(hasColour(cover, Qt::red));
    } else {
        QVERIFY(cover.isNull());
        QVERIFY(!originalSize.isValid());
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    }

    const QString sidecarTrack = dir.filePath(u"track.wav"_s);
    QVERIFY(QFile::copy(dataPath(u"wav-info.wav"_s), sidecarTrack));
    QVERIFY(writeFile(dir.filePath(u"cover.png"_s), png));
    cover = AudioMetadataReader::loadCover(sidecarTrack, maxSize, &originalSize);
    QCOMPARE(cover.size(), expected.isValid() ? expected : QSize(0, 0));
    QCOMPARE(originalSize, expected.isValid() ? claimed : QSize());
}

// ---------------------------------------------------------------------------------------------------------
// Sidecar covers
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa::sidecarPreferenceOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"Song.mp3"_s);
    QVERIFY(writeFile(track, mpegFrames()));
    // Best first. Case never matters, and among one name the extensions go jpg, jpeg, png, webp, bmp, gif.
    const QStringList ordered = {
        u"COVER.jpg"_s,
        u"cover.JPEG"_s,
        u"Cover.png"_s,
        u"cover.webp"_s,
        u"cover.bmp"_s,
        u"cover.gif"_s,
        u"Folder.jpg"_s,
        u"front.png"_s,
        u"ALBUM.jpg"_s,
        u"albumart.jpeg"_s,
        u"AlbumArtSmall.jpg"_s,
        u"song.PNG"_s,
        u"AlbumArt_{0D7F2B6C-5A3E-4C1B-9E8D-2F6A1B3C4D5E}_Large.jpg"_s,
        u"AlbumArt_{0D7F2B6C-5A3E-4C1B-9E8D-2F6A1B3C4D5E}_Small.jpg"_s,
    };
    for(QString const &name : ordered)
        QVERIFY(writeFile(dir.filePath(name), "not decoded here"));
    const QDir directory(dir.path());
    for(QString const &name : ordered) {
        QCOMPARE(AudioMetadataReader::findSidecarCover(track), directory.absoluteFilePath(name));
        QVERIFY(QFile::remove(dir.filePath(name)));
    }
    QCOMPARE(AudioMetadataReader::findSidecarCover(track), QString());
}

void Test_Audiometa::sidecarIgnores() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"Song.flac"_s);
    QVERIFY(writeFile(track, readFixture(u"flac-picture.flac"_s)));
    for(const char *name : {"cover.txt", "cover.tiff", "covers.jpg", "my cover.jpg", "cover-red.png", "cover.jpg.bak",
                            "AlbumArt_Large.jpg", "AlbumArt_{GUID}_Medium.jpg", "Song.flac.jpg", "Song2.jpg", ".jpg"})
        QVERIFY(writeFile(dir.filePath(QString::fromLatin1(name)), "x"));
    QVERIFY(QDir(dir.path()).mkpath(u"folder.jpg"_s));
    QVERIFY(QDir(dir.path()).mkpath(u"sub"_s));
    QVERIFY(writeFile(dir.filePath(u"sub/cover.jpg"_s), "x"));
    QCOMPARE(AudioMetadataReader::findSidecarCover(track), QString());
    // The track itself is no candidate: its extension is not an image's.
    QVERIFY(writeFile(dir.filePath(u"Song.jpg"_s), "x"));
    QCOMPARE(QFileInfo(AudioMetadataReader::findSidecarCover(track)).fileName(), u"Song.jpg"_s);
}

// Windows Media Player marks the Folder.jpg and AlbumArt files it writes hidden and system.
void Test_Audiometa::sidecarHiddenOnWindows() {
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"track.mp3"_s);
    QVERIFY(writeFile(track, mpegFrames()));
    const QString folder = dir.filePath(u"Folder.jpg"_s);
    QVERIFY(writeFile(folder, "x"));
    const QString native = QDir::toNativeSeparators(folder);
    const auto name = reinterpret_cast<const wchar_t *>(native.utf16());
    QVERIFY(SetFileAttributesW(name, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    const auto restore = qScopeGuard([&] { SetFileAttributesW(name, FILE_ATTRIBUTE_NORMAL); });
    QVERIFY(QFileInfo(folder).isHidden());
    QCOMPARE(QFileInfo(AudioMetadataReader::findSidecarCover(track)).fileName(), u"Folder.jpg"_s);
#else
    QSKIP("hidden and system attributes are a Windows thing");
#endif
}

// A folder is listed once for all of its tracks while its date stays the same, and again once the date moves.
// The tracks of a folder ask in a burst, and a listing for each made that quadratic.
void Test_Audiometa::sidecarListingFollowsFolderDate() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"track.mp3"_s);
    QVERIFY(writeFile(track, mpegFrames()));
    QVERIFY(writeFile(dir.filePath(u"folder.jpg"_s), "x"));
    const auto found = [&] { return QFileInfo(AudioMetadataReader::findSidecarCover(track)).fileName(); };

    // Changed a moment ago, a folder could change again within the same tick of its file system's clock and
    // keep the date it has; so its listing is not kept.
    const QDateTime recent = QDateTime::currentDateTime().addMSecs(-500);
    QVERIFY(setFolderDate(dir.path(), recent));
    QCOMPARE(found(), u"folder.jpg"_s);
    QVERIFY(writeFile(dir.filePath(u"cover.jpg"_s), "x"));
    QVERIFY(setFolderDate(dir.path(), recent));
    QCOMPARE(found(), u"cover.jpg"_s);

    // Changed long ago, it is kept: a change that put the date back is not seen...
    const QDateTime settled = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() - 3600);
    QVERIFY(setFolderDate(dir.path(), settled));
    QCOMPARE(found(), u"cover.jpg"_s);
    QVERIFY(QFile::remove(dir.filePath(u"cover.jpg"_s)));
    QVERIFY(setFolderDate(dir.path(), settled));
    QCOMPARE(found(), u"cover.jpg"_s);
    // ...and any that leaves a new date is.
    QVERIFY(setFolderDate(dir.path(), settled.addSecs(60)));
    QCOMPARE(found(), u"folder.jpg"_s);
}

void Test_Audiometa::embeddedCoverBeatsSidecar() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"track.flac"_s);
    QVERIFY(writeFile(track, readFixture(u"flac-picture.flac"_s)));
    QVERIFY(writeFile(dir.filePath(u"cover.png"_s), encode(solid(QSize(20, 20), Qt::blue), "png")));
    QSize originalSize;
    AudioCoverSource source{u"stale"_s, QDateTime::currentDateTime(), 1};
    const QImage cover = AudioMetadataReader::loadCover(track, QSize(), &originalSize, &source);
    QCOMPARE(cover.size(), QSize(16, 16));
    QCOMPARE(originalSize, QSize(16, 16));
    QVERIFY(hasColour(cover, Qt::red));
    QVERIFY(source.sidecar.isEmpty());
}

void Test_Audiometa::sidecarWithoutEmbeddedCover() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"track.wav"_s);
    QVERIFY(QFile::copy(dataPath(u"wav-info.wav"_s), track));
    // A PNG under a JPEG name: decoded by content here too.
    const QString folder = dir.filePath(u"folder.jpg"_s);
    QVERIFY(writeFile(folder, encode(solid(QSize(20, 10), Qt::blue), "png")));
    QSize originalSize;
    AudioCoverSource source;
    QImage cover = AudioMetadataReader::loadCover(track, QSize(), &originalSize, &source);
    QCOMPARE(cover.size(), QSize(20, 10));
    QCOMPARE(originalSize, QSize(20, 10));
    QVERIFY(hasColour(cover, Qt::blue));
    // Where it came from, for a cache to check later.
    QCOMPARE(source.sidecar, QDir(dir.path()).absoluteFilePath(u"folder.jpg"_s));
    QCOMPARE(source.lastModified, QFileInfo(folder).lastModified());
    QCOMPARE(source.size, QFileInfo(folder).size());
    cover = AudioMetadataReader::loadCover(track, QSize(10, 10), &originalSize);
    QCOMPARE(cover.size(), QSize(10, 5));
    QCOMPARE(originalSize, QSize(20, 10));

    // Named like the track, when nothing better is there.
    QVERIFY(QFile::remove(dir.filePath(u"folder.jpg"_s)));
    QVERIFY(writeFile(dir.filePath(u"TRACK.png"_s), encode(solid(QSize(12, 12), Qt::green), "png")));
    cover = AudioMetadataReader::loadCover(track);
    QCOMPARE(cover.size(), QSize(12, 12));
    QVERIFY(hasColour(cover, Qt::green));
}

// A cover that does not decode is as good as none.
void Test_Audiometa::sidecarWhenEmbeddedDoesNotDecode() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFile(dir.filePath(u"cover.png"_s), encode(solid(QSize(20, 20), Qt::blue), "png")));
    for(QByteArray const &picture :
        {"definitely not an image"_ba, "\x89PNG\r\n\x1A\n"_ba + QByteArray(64, '\0'), pngHeaderOnly(100000, 100000)}) {
        const QString track = dir.filePath(u"track.flac"_s);
        QVERIFY(writeFile(track, flacFile("Broken", picture)));
        QCOMPARE(AudioMetadataReader::read(track).cover.data, picture);
        QSize originalSize;
        const QImage cover = AudioMetadataReader::loadCover(track, QSize(), &originalSize);
        QCOMPARE(cover.size(), QSize(20, 20));
        QCOMPARE(originalSize, QSize(20, 20));
        QVERIFY(hasColour(cover, Qt::blue));
    }
}

// Some decoders read a whole file before they look at it, so a sidecar gets the limit an embedded picture has.
void Test_Audiometa::sidecarTooLarge() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString track = dir.filePath(u"track.wav"_s);
    QVERIFY(QFile::copy(dataPath(u"wav-info.wav"_s), track));
    QByteArray png = encode(solid(QSize(20, 20), Qt::blue), "png");
    QVERIFY(writeFile(dir.filePath(u"cover.png"_s), png));
    QCOMPARE(AudioMetadataReader::loadCover(track).size(), QSize(20, 20));
    // Trailing bytes after IEND do not stop a PNG from decoding.
    png.append(QByteArray(AudioMeta::kMaxPictureBytes, '\0'));
    QVERIFY(writeFile(dir.filePath(u"cover.png"_s), png));
    AudioCoverSource source{u"stale"_s, QDateTime::currentDateTime(), 1};
    QVERIFY(AudioMetadataReader::loadCover(track, QSize(), nullptr, &source).isNull());
    // No picture, so nothing came from anywhere.
    QVERIFY(source.sidecar.isEmpty());
}

// ---------------------------------------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------------------------------------

// The fixtures read on several threads at once, as the thumbnailer does, against the same files read one at a
// time.
void Test_Audiometa::concurrentReads() {
    struct Expected {
        QString path, title;
        QStringList tagTypes;
        QByteArray picture;
        QSize coverSize;
        QRgb pixel = 0;
    };
    QList<Expected> expected;
    for(Fixture const &fixture : fixtureTable()) {
        const QString path = dataPath(QString::fromLatin1(fixture.file));
        const AudioMetadata meta = AudioMetadataReader::read(path);
        const QImage cover = AudioMetadataReader::loadCover(path, QSize(8, 8));
        expected.append(
            {path, meta.title, meta.tagTypes, meta.cover.data, cover.size(), cover.isNull() ? 0 : cover.pixel(4, 4)});
    }
    QMutex mutex;
    QStringList mismatches;
    QThreadPool pool;
    pool.setMaxThreadCount(8);
    for(int task = 0; task < 8; task++) {
        pool.start([&, task] {
            for(int round = 0; round < 4; round++) {
                for(qsizetype i = 0; i < expected.size(); i++) {
                    // Each thread starts somewhere else, so the same file is rarely read by all at once.
                    Expected const &e = expected.at((i + task * 3) % expected.size());
                    const AudioMetadata meta = AudioMetadataReader::read(e.path);
                    const QImage cover = AudioMetadataReader::loadCover(e.path, QSize(8, 8));
                    if(meta.title != e.title || meta.tagTypes != e.tagTypes || meta.cover.data != e.picture ||
                       cover.size() != e.coverSize || (!cover.isNull() && cover.pixel(4, 4) != e.pixel)) {
                        const QMutexLocker lock(&mutex);
                        mismatches.append(e.path);
                    }
                }
            }
        });
    }
    pool.waitForDone();
    QVERIFY2(mismatches.isEmpty(), qPrintable(mismatches.join(u", "_s)));
}

// Every fixture and every sample above, cut short at every offset or a spread of them, and corrupted at random:
// each must come back, and soon. All of it in memory, through the functions behind read() and loadCover(); a
// spread of the same variants from disk, through read() and loadCover() themselves.
void Test_Audiometa::robustness() {
    // Corrupt pictures make Qt's decoders complain at length.
    QLoggingCategory::setFilterRules(u"qt.gui.imageio*=false"_s);
    const auto restoreLogging = qScopeGuard([] { QLoggingCategory::setFilterRules({}); });

    QList<QPair<QString, QByteArray>> samples;
    const QDir data(dataPath({}));
    for(QString const &file : data.entryList(QDir::Files)) {
        if(!file.endsWith(u".sh"_s))
            samples.append({file, readFile(data.filePath(file))});
    }
    QVERIFY(samples.size() > 20);
    QByteArray manyTags;
    for(int i = 0; i < 2000; i++)
        manyTags += id3Tag(3, {});
    samples += QList<QPair<QString, QByteArray>>{
        {u"flac behind id3v2"_s, id3Prefix() + readFixture(u"flac-picture.flac"_s)},
        {u"ogg behind id3v2"_s, id3Prefix() + readFixture(u"vorbis-picture.ogg"_s)},
        {u"mp3 with three tags"_s, mp3WithThreeTags()},
        {u"monkey's audio"_s, "MAC \x96\x0F"_ba + QByteArray(300, '\x55') + apeSample() + id3v1Sample()},
        {u"flac with a bomb"_s, flacFile("Bomb", pngHeaderOnly(100000, 100000))},
        {u"thousands of id3v2 tags"_s, manyTags + flacFile("Behind them all", redPng())},
    };
    if(hasJpegPlugin()) {
        const QByteArray jpeg = withExifOrientation(encode(halves(QSize(64, 32)), "jpeg"), 6);
        samples.append({u"sideways jpeg"_s, id3Tag(3, id3Picture(3, 3, "image/jpeg", jpeg)) + mpegFrames()});
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(u"sample"_s);
    // One handle, rewritten in place: on Windows every new file costs a virus scan.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite | QIODevice::Truncate));

    // Debug builds and busy CI machines are slow, but a read of a few kilobytes that takes this long is a loop
    // gone wrong, not a slow machine.
    constexpr qint64 kMaxNs = 250'000'000;
    struct Worst {
        qint64 ns = 0;
        QString what;
        int runs = 0;
    } reads, covers;
    QString oversized;
    auto time = [](Worst &worst, QString const &what, auto &&run) {
        QElapsedTimer timer;
        timer.start();
        run();
        const qint64 ns = timer.nsecsElapsed();
        worst.runs++;
        if(ns > worst.ns) {
            worst.ns = ns;
            worst.what = what;
        }
    };
    // Whatever a cover turns out to be, it keeps to the size asked for.
    auto checkCover = [&](QImage const &cover, QString const &what) {
        if(cover.width() > 8 || cover.height() > 8)
            oversized = what;
    };
    int variant = 0;
    auto check = [&](QByteArray const &bytes, QString const &what) {
        const AudioMeta::ByteSource src(bytes);
        time(reads, what, [&] { AudioMeta::read(src, AudioMetadataReader::All); });
        time(covers, what, [&] {
            const QByteArray picture = AudioMeta::read(src, kCover).cover.data;
            checkCover(AudioMeta::loadCover(picture, QString(), QSize(8, 8), nullptr), what);
        });
        if(variant++ % 16)
            return true;
        if(!file.resize(0) || !file.seek(0) || file.write(bytes) != bytes.size() || !file.flush())
            return false;
        const QString onDisk = what + u" from disk"_s;
        time(reads, onDisk, [&] { AudioMetadataReader::read(path); });
        time(covers, onDisk, [&] { checkCover(AudioMetadataReader::loadCover(path, QSize(8, 8)), onDisk); });
        return true;
    };

    QRandomGenerator random(20261007);
    static constexpr quint8 kInteresting[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};
    auto corrupt = [&](QByteArray bytes, qsizetype from, qsizetype to) {
        const int changes = 1 + random.bounded(4);
        for(int i = 0; i < changes; i++) {
            const qsizetype at = from + random.bounded(int(to - from));
            bytes[at] = random.bounded(2) ? char(random.bounded(256))
                                          : char(kInteresting[random.bounded(int(sizeof(kInteresting)))]);
        }
        return bytes;
    };

    for(auto const &[name, bytes] : samples) {
        const qsizetype size = bytes.size();
        // All of a small sample; for a larger one a spread plus its first and last bytes, where tags live.
        QList<qsizetype> cuts;
        if(size < 4096) {
            for(qsizetype i = 0; i < size; i++)
                cuts.append(i);
        } else {
            for(qsizetype i = 0; i < 300; i++)
                cuts.append(size * i / 300);
            for(qsizetype i = 0; i < 64; i++) {
                cuts.append(i);
                cuts.append(size - 1 - i);
            }
        }
        for(qsizetype cut : cuts) {
            QVERIFY(check(bytes.left(cut), name + u" cut at "_s + QString::number(cut)));
            if(cut % 8 == 0)
                QVERIFY(check(bytes.mid(cut), name + u" from "_s + QString::number(cut)));
        }
        if(size == 0)
            continue;
        for(int round = 0; round < 300; round++) {
            // Half anywhere, half in the first or last 512 bytes.
            qsizetype from = 0, to = size;
            if(round % 2) {
                const qsizetype window = qMin<qsizetype>(512, size);
                from = round % 4 == 1 ? 0 : size - window;
                to = from + window;
            }
            QVERIFY(check(corrupt(bytes, from, to), name + u" corruption "_s + QString::number(round)));
        }
        // The picture itself, for the decoder rather than the parsers.
        qsizetype picture = bytes.indexOf(redPng());
        qsizetype pictureSize = redPng().size();
        if(picture < 0 && name == u"sideways jpeg"_s) {
            picture = bytes.indexOf("\xFF\xD8\xFF"_ba);
            pictureSize = bytes.indexOf("\xFF\xD9"_ba, picture) + 2 - picture;
        }
        if(picture >= 0) {
            for(int round = 0; round < 100; round++) {
                const QByteArray bad = corrupt(bytes, picture, picture + pictureSize);
                QVERIFY(check(bad, name + u" picture corruption "_s + QString::number(round)));
            }
        }
    }
    qInfo("%d reads, slowest %.2f ms (%s); %d covers, slowest %.2f ms (%s)", reads.runs, double(reads.ns) / 1e6,
          qPrintable(reads.what), covers.runs, double(covers.ns) / 1e6, qPrintable(covers.what));
    QVERIFY2(reads.ns < kMaxNs, qPrintable(reads.what));
    QVERIFY2(covers.ns < kMaxNs, qPrintable(covers.what));
    QVERIFY2(oversized.isEmpty(), qPrintable(oversized));
}

QTEST_GUILESS_MAIN(Test_Audiometa)
#include "test_audiometa.moc"
