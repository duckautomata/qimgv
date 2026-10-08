// Exercises DocumentInfo's content sniffing against hand-built fixtures that
// target the exact edge cases the old detectors got wrong:
//   - APNG whose acTL chunk sits past the first 120 bytes
//   - AVIF sequences that declare 'avis' only in the compatible-brands list
// and its split of audio from video, which mime types cannot make: Ogg,
// Matroska, MP4 and ASF each hold either. The real files in data/audio come
// from ffmpeg; the layouts it does not write are built here.
//
// The suite runs against Qt's built-in mime database on Windows and the
// system's on Linux, which name the same files differently, so it asserts
// the verdicts and never a mime type.
#include <QtTest>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QImageReader>
#include <QMovie>
#include <QTemporaryDir>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QBuffer>
#include <QtEndian>

#include "settings.h"
#include "sourcecontainers/documentinfo.h"
#include "sourcecontainers/mediaprobe.h"

namespace {

using Kind = MediaProbe::Kind;

// For a hand-built file whose type depends on which mime database answers.
constexpr int kAnyType = -1;

// Every file in data/audio, with what the probe alone makes of it and what DocumentInfo finally decides.
// MPEG audio and AC-3 have no signature beyond a sync word, so the probe leaves those to the extension.
struct AudioFixture {
    const char *file;
    Kind kind;
    const char *container;
    DocumentType type;
};

const AudioFixture kAudioFixtures[] = {
    {"aac-cover.m4a", Kind::Audio, "mp4", AUDIO},
    {"aac-isom.mp4", Kind::Audio, "mp4", AUDIO},
    {"ac3.ac3", Kind::Unknown, "", AUDIO},
    {"aiff-id3.aiff", Kind::Audio, "aiff", AUDIO},
    {"alac.m4a", Kind::Audio, "mp4", AUDIO},
    {"apple.caf", Kind::Audio, "caf", AUDIO},
    {"cover-red.png", Kind::Unknown, "", STATIC},
    {"flac-in-ogg.oga", Kind::Audio, "ogg", AUDIO},
    {"flac-picture.flac", Kind::Audio, "flac", AUDIO},
    {"matroska-cover.mka", Kind::Audio, "matroska", AUDIO},
    {"mp3-bare.mp3", Kind::Unknown, "", AUDIO},
    {"mp3-id3v1.mp3", Kind::Unknown, "", AUDIO},
    {"mp3-id3v23.mp3", Kind::Audio, "mp3", AUDIO},
    {"mp3-id3v24.mp3", Kind::Audio, "mp3", AUDIO},
    {"mpeg4-video.mp4", Kind::Video, "mp4", VIDEO},
    {"opus-audio-only.webm", Kind::Audio, "webm", AUDIO},
    {"opus-picture.opus", Kind::Audio, "ogg", AUDIO},
    {"speex.spx", Kind::Audio, "ogg", AUDIO},
    {"sun.au", Kind::Audio, "au", AUDIO},
    {"theora-video.ogg", Kind::Video, "ogg", VIDEO},
    {"tta.tta", Kind::Audio, "tta", AUDIO},
    {"video-misnamed.mka", Kind::Video, "matroska", VIDEO},
    {"vorbis-picture.ogg", Kind::Audio, "ogg", AUDIO},
    {"vp8-video.webm", Kind::Video, "webm", VIDEO},
    {"wav-id3.wav", Kind::Audio, "wav", AUDIO},
    {"wav-info.wav", Kind::Audio, "wav", AUDIO},
    {"wavpack-ape.wv", Kind::Audio, "wavpack", AUDIO},
    {"wma.wma", Kind::Audio, "asf", AUDIO},
};

// ---------------------------------------------------------------------------
// Byte builders
// ---------------------------------------------------------------------------

template<typename T>
QByteArray bigEndian(T value) {
    QByteArray bytes(sizeof(T), '\0');
    qToBigEndian(value, bytes.data());
    return bytes;
}

template<typename T>
QByteArray littleEndian(T value) {
    QByteArray bytes(sizeof(T), '\0');
    qToLittleEndian(value, bytes.data());
    return bytes;
}

QByteArray zeros(int n) {
    return QByteArray(n, '\0');
}

// ISOBMFF

QByteArray box(const char *type, const QByteArray &payload) {
    return bigEndian<quint32>(quint32(8 + payload.size())) + QByteArray(type, 4) + payload;
}

// Version 0, with the given 24-bit flags.
QByteArray fullBox(const char *type, quint32 flags, const QByteArray &payload) {
    return box(type, bigEndian<quint32>(flags) + payload);
}

QByteArray ftyp(const char *major, std::initializer_list<const char *> compatible) {
    QByteArray payload = QByteArray(major, 4) + zeros(4);
    for(const char *brand : compatible)
        payload += QByteArray(brand, 4);
    return box("ftyp", payload);
}

// tkhd flags: 1 enabled, 2 in movie, 4 in preview.
QByteArray trak(const char *handler, bool enabled) {
    const QByteArray tkhd = fullBox("tkhd", enabled ? 0x7 : 0x6, zeros(80));
    const QByteArray hdlr = fullBox("hdlr", 0, zeros(4) + QByteArray(handler, 4) + zeros(13));
    const QByteArray mdia =
        box("mdia", fullBox("mdhd", 0, zeros(20)) + hdlr + box("minf", fullBox("smhd", 0, zeros(4))));
    return box("trak", tkhd + box("edts", zeros(16)) + mdia);
}

QByteArray moov(const QByteArray &traks) {
    return box("moov", fullBox("mvhd", 0, zeros(96)) + traks);
}

// The 64-bit form writers use for media past 4 GiB.
QByteArray mdat64(int payload) {
    return bigEndian<quint32>(1) + "mdat" + bigEndian<quint64>(quint64(16 + payload)) + QByteArray(payload, '\x55');
}

// Size 0: the box runs to the end of the file.
QByteArray openEndedBox(const char *type, const QByteArray &payload) {
    return bigEndian<quint32>(0) + QByteArray(type, 4) + payload;
}

// Matroska. Sizes always take the 8-byte form, so an element's length does not depend on its content.

QByteArray ebmlId(quint32 id) {
    QByteArray bytes = bigEndian<quint32>(id);
    while(bytes.startsWith('\0'))
        bytes.remove(0, 1);
    return bytes;
}

QByteArray ebml(quint32 id, const QByteArray &data) {
    return ebmlId(id) + "\x01" + bigEndian<quint64>(quint64(data.size())).mid(1) + data;
}

QByteArray ebmlUnknownSize(quint32 id, const QByteArray &data) {
    return ebmlId(id) + QByteArray::fromHex("01ffffffffffffff") + data;
}

QByteArray ebmlUint(quint32 id, quint64 value) {
    return ebml(id, bigEndian<quint64>(value));
}

QByteArray ebmlHeader(const char *docType) {
    return ebml(0x1A45DFA3, ebmlUint(0x4286, 1) + ebml(0x4282, docType));
}

QByteArray tracks(std::initializer_list<int> trackTypes) {
    QByteArray entries;
    int number = 1;
    for(int type : trackTypes) {
        entries += ebml(0xAE, ebmlUint(0xD7, quint64(number++)) + ebmlUint(0x73C5, 0x1234) +
                                  ebml(0x86, type == 1 ? "V_VP8" : "A_VORBIS") + ebmlUint(0x83, quint64(type)));
    }
    return ebml(0x1654AE6B, entries);
}

QByteArray seekHead(quint32 target, quint64 position) {
    return ebml(0x114D9B74, ebml(0x4DBB, ebml(0x53AB, bigEndian<quint32>(target)) + ebmlUint(0x53AC, position)));
}

QByteArray info() {
    return ebml(0x1549A966, ebmlUint(0x2AD7B1, 1000000) + ebml(0x4D80, "qimgv test"));
}

QByteArray cluster(bool unknownSize) {
    const QByteArray content = ebmlUint(0xE7, 0) + ebml(0xA3, QByteArray(300, '\x42'));
    return unknownSize ? ebmlUnknownSize(0x1F43B675, content) : ebml(0x1F43B675, content);
}

// Tracks after the media, an unknown-size Cluster in the way: only the SeekHead leads there.
QByteArray matroskaTracksViaSeekHead(int trackType) {
    const QByteArray head = seekHead(0x1654AE6B, 0);
    const QByteArray media = info() + cluster(true);
    const QByteArray segment = seekHead(0x1654AE6B, quint64(head.size() + media.size())) + media + tracks({trackType});
    return ebmlHeader("matroska") + ebmlUnknownSize(0x18538067, segment);
}

// A first SeekHead that lists only a second one, at the end, which lists Tracks; what mkvmerge does when the
// index it reserved space for at the front is too small.
QByteArray matroskaTracksViaSecondSeekHead() {
    const QByteArray front = seekHead(0x114D9B74, 0);
    const QByteArray media = cluster(false) + cluster(false);
    const QByteArray tracksElement = tracks({2, 1});
    const quint64 tracksAt = quint64(front.size() + media.size());
    const QByteArray segment = seekHead(0x114D9B74, tracksAt + quint64(tracksElement.size())) + media + tracksElement +
                               seekHead(0x1654AE6B, tracksAt);
    return ebmlHeader("matroska") + ebml(0x18538067, segment);
}

// ASF. GUIDs as stored on disk.

const char *const kAsfHeader = "3026b2758e66cf11a6d900aa0062ce6c";
const char *const kAsfData = "3626b2758e66cf11a6d900aa0062ce6c";
const char *const kAsfFileProperties = "a1dcab8c47a9cf118ee400c00c205365";
const char *const kAsfStreamProperties = "9107dcb7b7a9cf118ee600c00c205365";
const char *const kAsfHeaderExtension = "b503bf5f2ea9cf118ee300c00c205365";
const char *const kAsfExtendedStreamProperties = "cba5e61472c632438399a96952065b5a";
const char *const kAsfAudioMedia = "409e69f84d5bcf11a8fd00805f5c442b";
const char *const kAsfVideoMedia = "c0ef19bc4d5bcf11a8fd00805f5c442b";

QByteArray asfObject(const char *guid, const QByteArray &payload) {
    return QByteArray::fromHex(guid) + littleEndian<quint64>(quint64(24 + payload.size())) + payload;
}

QByteArray asfStream(const char *mediaGuid) {
    // Stream type, error correction type, time offset, two data lengths, flags, reserved.
    return asfObject(kAsfStreamProperties, QByteArray::fromHex(mediaGuid) + zeros(16) + zeros(8) + zeros(8) +
                                               littleEndian<quint16>(1) + zeros(4));
}

// 60 bytes of fixed fields, one stream name, one payload extension system, then the stream itself.
QByteArray asfExtendedStream(const char *mediaGuid) {
    const QByteArray name = littleEndian<quint16>(0) + littleEndian<quint16>(6) + QByteArray("v\0i\0d\0", 6);
    const QByteArray system = zeros(16) + littleEndian<quint16>(0xFFFF) + littleEndian<quint32>(4) + zeros(4);
    return asfObject(kAsfExtendedStreamProperties, zeros(60) + littleEndian<quint16>(1) + littleEndian<quint16>(1) +
                                                       name + system + asfStream(mediaGuid));
}

QByteArray asfHeaderExtension(const QByteArray &objects) {
    return asfObject(kAsfHeaderExtension, QByteArray::fromHex("11d2d3abbaa9cf118ee600c00c205365") +
                                              littleEndian<quint16>(6) +
                                              littleEndian<quint32>(quint32(objects.size())) + objects);
}

QByteArray asfFile(const QList<QByteArray> &objects) {
    QByteArray all;
    for(const QByteArray &object : objects)
        all += object;
    return QByteArray::fromHex(kAsfHeader) + littleEndian<quint64>(quint64(30 + all.size())) +
           littleEndian<quint32>(quint32(objects.size())) + "\x01\x02" + all + asfObject(kAsfData, zeros(64));
}

// Ogg. CRCs are left at zero: nothing here checks them, and neither do the mime databases.

QByteArray oggPage(bool firstOfStream, quint32 serial, const QByteArray &packet) {
    QByteArray lacing;
    qsizetype left = packet.size();
    for(; left >= 255; left -= 255)
        lacing += '\xFF';
    lacing += char(left);
    return QByteArray("OggS") + '\0' + char(firstOfStream ? 0x02 : 0x00) + zeros(8) + littleEndian<quint32>(serial) +
           zeros(8) + char(lacing.size()) + lacing + packet;
}

QByteArray vorbisId() {
    return QByteArray("\x01vorbis") + zeros(23);
}

QByteArray theoraId() {
    return QByteArray("\x80theora") + zeros(35);
}

QByteArray skeletonHead() {
    return QByteArray("fishead\0", 8) + zeros(56);
}

QByteArray oggFile(const QList<QByteArray> &firstPackets) {
    QByteArray file;
    quint32 serial = 1;
    for(const QByteArray &packet : firstPackets)
        file += oggPage(true, serial++, packet);
    return file + oggPage(false, 1, QByteArray("\x03vorbis") + zeros(32));
}

// Other formats

QByteArray id3v2(int padding) {
    const quint32 size = quint32(padding);
    QByteArray header("ID3\x04\x00\x00", 6);
    header += char((size >> 21) & 0x7F);
    header += char((size >> 14) & 0x7F);
    header += char((size >> 7) & 0x7F);
    header += char(size & 0x7F);
    return header + zeros(padding);
}

QByteArray riff(const char *magic, const char *form, const QByteArray &chunks) {
    return QByteArray(magic, 4) + littleEndian<quint32>(quint32(4 + chunks.size())) + QByteArray(form, 4) + chunks;
}

QByteArray dsf() {
    return QByteArray("DSD ") + littleEndian<quint64>(28) + littleEndian<quint64>(92) + littleEndian<quint64>(0) +
           QByteArray("fmt ") + littleEndian<quint64>(52) + zeros(40) + QByteArray("data") + littleEndian<quint64>(12) +
           zeros(4);
}

QByteArray midi() {
    return QByteArray::fromHex("4d546864000000060000000100604d54726b0000000400ff2f00");
}

QByteArray png() {
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "png");
    return bytes;
}

struct Built {
    const char *name;
    const char *fileName; // the extension matters to DocumentInfo, never to the probe
    QByteArray bytes;
    Kind kind;
    const char *container;
    int type; // a DocumentType, or kAnyType
};

QList<Built> builtFiles(const QByteArray &flacFixture) {
    return {
        // ISOBMFF
        {"mp4, audio, moov after a 64-bit mdat, open-ended", "late-moov.mp4",
         ftyp("isom", {"isom", "iso2", "mp41"}) + box("free", zeros(8)) + mdat64(70000) +
             openEndedBox("moov", fullBox("mvhd", 0, zeros(96)) + trak("soun", true)),
         Kind::Audio, "mp4", AUDIO},
        {"mp4, video, moov after mdat", "late-moov-video.mp4",
         ftyp("isom", {"isom", "mp41"}) + mdat64(70000) + moov(trak("soun", true) + trak("vide", true)), Kind::Video,
         "mp4", VIDEO},
        // Apple's own audiobooks say M4B, which settles it at the ftyp; a generic brand leaves it to the tracks.
        {"mp4, disabled chapter-image track", "chapters.m4b",
         ftyp("isom", {"isom", "mp42"}) + moov(trak("soun", true) + trak("text", false) + trak("vide", false)) +
             box("mdat", zeros(64)),
         Kind::Audio, "mp4", AUDIO},
        {"mp4, enabled video track after audio", "two-tracks.mp4",
         ftyp("mp42", {"isom", "mp42"}) + moov(trak("soun", true) + trak("vide", true)) + box("mdat", zeros(64)),
         Kind::Video, "mp4", VIDEO},
        {"mp4, audio brand overrides tracks", "brand.mp4",
         ftyp("M4A ", {"M4A ", "isom"}) + moov(trak("vide", true)) + box("mdat", zeros(64)), Kind::Audio, "mp4", AUDIO},
        {"mp4, only a text track", "text.mp4", ftyp("isom", {"isom"}) + moov(trak("text", true)), Kind::Unknown, "mp4",
         kAnyType},
        // A download cut short keeps the sizes it was written with.
        {"mp4, cut short inside moov", "partial.mp4",
         (ftyp("isom", {"isom"}) + moov(trak("vide", true) + trak("soun", true))).chopped(40), Kind::Video, "mp4",
         VIDEO},
        {"quicktime without ftyp, audio", "old.mov", moov(trak("soun", true)) + box("mdat", zeros(64)), Kind::Audio,
         "mov", AUDIO},
        {"canon cr3 is not ours", "photo.cr3", ftyp("crx ", {"crx ", "isom"}) + moov(trak("vide", true)), Kind::Unknown,
         "", kAnyType},
        {"avif sequence is not ours", "seq.avif", ftyp("avis", {"avif", "mif1", "miaf"}) + moov(trak("vide", true)),
         Kind::Unknown, "", ANIMATED},

        // Matroska
        {"matroska, tracks only via SeekHead, video", "seek.mka", matroskaTracksViaSeekHead(1), Kind::Video, "matroska",
         VIDEO},
        {"matroska, tracks only via SeekHead, audio", "seek.mkv", matroskaTracksViaSeekHead(2), Kind::Audio, "matroska",
         AUDIO},
        {"matroska, tracks via a second SeekHead", "second-seek.mka", matroskaTracksViaSecondSeekHead(), Kind::Video,
         "matroska", VIDEO},
        {"webm, audio only", "audio.webm",
         ebmlHeader("webm") + ebmlUnknownSize(0x18538067, info() + tracks({2}) + cluster(true)), Kind::Audio, "webm",
         AUDIO},

        // ASF
        {"asf, video stream, named .wma", "video.wma",
         asfFile({asfObject(kAsfFileProperties, zeros(80)), asfStream(kAsfAudioMedia), asfStream(kAsfVideoMedia)}),
         Kind::Video, "asf", VIDEO},
        {"asf, cut short inside the header", "partial.wma",
         asfFile({asfStream(kAsfVideoMedia), asfObject(kAsfFileProperties, zeros(80))}).left(30 + 78 + 40), Kind::Video,
         "asf", VIDEO},
        {"asf, audio only, named .wmv", "audio.wmv", asfFile({asfStream(kAsfAudioMedia)}), Kind::Audio, "asf", AUDIO},
        {"asf, video declared only in the header extension", "mbr.wmv",
         asfFile({asfStream(kAsfAudioMedia), asfHeaderExtension(asfExtendedStream(kAsfVideoMedia))}), Kind::Video,
         "asf", VIDEO},

        // Ogg
        {"ogg, skeleton then vorbis", "skeleton.ogg", oggFile({skeletonHead(), vorbisId()}), Kind::Audio, "ogg", AUDIO},
        {"ogg, vorbis then theora", "both.ogg", oggFile({vorbisId(), theoraId()}), Kind::Video, "ogg", VIDEO},
        {"ogg, OGM video", "ogm.ogg", oggFile({QByteArray("\x01video\0\0\0", 9) + zeros(48)}), Kind::Video, "ogg",
         VIDEO},
        {"ogg, skeleton only", "skeleton-only.ogg", oggFile({skeletonHead()}), Kind::Unknown, "ogg", kAnyType},

        // Signatures
        {"flac content named .bin", "mystery.bin", flacFixture, Kind::Audio, "flac", AUDIO},
        {"id3v2 in front of flac", "tagged.flac", id3v2(100) + flacFixture, Kind::Audio, "flac", AUDIO},
        {"id3v2 in front of adts", "tagged.aac", id3v2(20) + QByteArray::fromHex("fff15080") + zeros(64), Kind::Audio,
         "aac", AUDIO},
        {"id3v2 with nothing known after it", "tagged.mp3", id3v2(20) + zeros(64), Kind::Audio, "mp3", AUDIO},
        // Some mime databases take the three letters for audio/mpeg regardless.
        {"text that starts with ID3", "notes.txt", "ID3 tags are metadata for mp3 files, nothing more.\n",
         Kind::Unknown, "", kAnyType},
        {"monkey's audio", "a.ape", QByteArray("MAC ") + littleEndian<quint16>(3990) + zeros(64), Kind::Audio, "ape",
         AUDIO},
        {"musepack sv7", "a.mpc", QByteArray("MP+\x07") + zeros(64), Kind::Audio, "musepack", AUDIO},
        {"musepack sv8", "b.mpc", QByteArray("MPCKSH") + zeros(64), Kind::Audio, "musepack", AUDIO},
        {"tak", "a.tak", QByteArray("tBaK") + zeros(64), Kind::Audio, "tak", AUDIO},
        {"amr", "a.amr", QByteArray("#!AMR\n") + zeros(64), Kind::Audio, "amr", AUDIO},
        {"dsf", "a.dsf", dsf(), Kind::Audio, "dsf", AUDIO},
        {"dff", "a.dff", QByteArray("FRM8") + bigEndian<quint64>(64) + "DSD " + zeros(60), Kind::Audio, "dff", AUDIO},
        {"8svx", "a.8svx", QByteArray("FORM") + bigEndian<quint32>(68) + "8SVX" + zeros(64), Kind::Audio, "8svx",
         AUDIO},
        {"rf64", "big.wav", riff("RF64", "WAVE", QByteArray("ds64") + littleEndian<quint32>(28) + zeros(28)),
         Kind::Audio, "wav", AUDIO},
        {"wave64", "a.w64",
         QByteArray::fromHex("726966662e91cf11a5d628db04c10000") + littleEndian<quint64>(64) +
             QByteArray::fromHex("77617665f3acd3118cd100c04f8edb8a") + zeros(24),
         Kind::Audio, "w64", AUDIO},
        {"avi", "a.avi", riff("RIFF", "AVI ", QByteArray("LIST") + littleEndian<quint32>(4) + "hdrl"), Kind::Video,
         "avi", VIDEO},

        // Nothing to play, but an audio extension: the player is asked, says it cannot, and Play folder and
        // Shuffle move on. Read as an image, it would be an empty view that stopped them.
        {"midi named .mp3", "midi.mp3", midi(), Kind::Unknown, "", AUDIO},
        {"m3u playlist named .mp3", "list.mp3", "#EXTM3U\n#EXTINF:123,Artist - Title\nsong.mp3\n", Kind::Unknown, "",
         AUDIO},
        {"pls playlist named .mp3", "list2.mp3", "[playlist]\nFile1=song.mp3\nNumberOfEntries=1\nVersion=2\n",
         Kind::Unknown, "", AUDIO},
        {"text named .wav", "notes.wav", "Not a recording, just some notes.\n", Kind::Unknown, "", AUDIO},
        // Without one, a mime type that rules out playing still does.
        {"midi named .mid", "song.mid", midi(), Kind::Unknown, "", STATIC},
        // An image is an image, whatever it is called.
        {"png named .mp3", "picture.mp3", png(), Kind::Unknown, "", STATIC},
    };
}

} // namespace

class Test_FormatDetect : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void detectsType_data();
    void detectsType();
    void decodesAnimatedAvif();

    void everyAudioFixtureIsListed();
    void probesFixtures_data();
    void probesFixtures();
    void detectsBuiltFiles_data();
    void detectsBuiltFiles();
    void namesFormat_data();
    void namesFormat();
    void audioPlaybackOff();
    void videoPlaybackOff();
    void allPlaybackOff();
    void probeSurvivesDamagedFiles();
    void probeSurvivesHostileFiles();

private:
    QTemporaryDir mTemp;

    QString dataPath(const QString &name) const {
        return QStringLiteral(QIMGV_TEST_DATA_DIR) + QLatin1Char('/') + name;
    }

    QByteArray fixture(const QString &name) const {
        QFile file(dataPath(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    QString writeTemp(const QString &name, const QByteArray &bytes) const {
        const QString path = mTemp.filePath(name);
        QFile file(path);
        if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
            return QString();
        return path;
    }

    // Probes the bytes and fails the test when that takes long enough to suggest an unbounded walk.
    void probeQuickly(const QByteArray &bytes, const char *what);
};

void Test_FormatDetect::initTestCase() {
    // Keep the test out of the developer's real config directory.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("qimgv-test");
    QCoreApplication::setApplicationName("qimgv-test");
    settings = Settings::getInstance();
    QVERIFY(settings != nullptr);
    // Persisted: a run that died inside a test that turns these off must not leave them off for the next.
    settings->setVideoPlayback(true);
    settings->setAudioPlayback(true);
    QVERIFY(mTemp.isValid());
}

void Test_FormatDetect::detectsType_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int>("expectedType");
    QTest::addColumn<QByteArray>("requiresReader");

    // The regression that motivated rewriting detectAPNG(): 200 bytes of text
    // metadata ahead of acTL. The old 120-byte window missed it.
    // qimgv only reports APNG as animated when an "apng" reader is present,
    // so these rows are skipped on a system without that plugin.
    QTest::newRow("apng, acTL past 120 bytes") << "apng_late_actl.png" << int(ANIMATED) << QByteArray("apng");
    QTest::newRow("apng, acTL early") << "apng_early_actl.png" << int(ANIMATED) << QByteArray("apng");
    QTest::newRow("png, no acTL") << "png_still.png" << int(STATIC) << QByteArray();

    // The regression that motivated rewriting detectAnimatedAvif(): major
    // brand is 'avif', 'avis' appears only among the compatible brands.
    QTest::newRow("avif seq via compat brand") << "avif_seq_compat.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("avif seq via major brand") << "avif_seq_major.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("avif still") << "avif_still.avif" << int(STATIC) << QByteArray();
    QTest::newRow("heif sequence") << "heif_seq.heic" << int(ANIMATED) << QByteArray();
    QTest::newRow("heif still") << "heif_still.heic" << int(STATIC) << QByteArray();

    QTest::newRow("webp animated") << "webp_animated.webp" << int(ANIMATED) << QByteArray();
    QTest::newRow("webp still") << "webp_still.webp" << int(STATIC) << QByteArray();

    // Real files produced by ffmpeg/libaom, not hand-built headers.
    QTest::newRow("real avif sequence") << "real_anim.avif" << int(ANIMATED) << QByteArray();
    QTest::newRow("real avif still") << "real_still.avif" << int(STATIC) << QByteArray();

    for(const AudioFixture &f : kAudioFixtures) {
        QTest::newRow(f.file) << QStringLiteral("audio/") + QLatin1String(f.file) << int(f.type) << QByteArray();
    }
}

void Test_FormatDetect::detectsType() {
    QFETCH(QString, file);
    QFETCH(int, expectedType);
    QFETCH(QByteArray, requiresReader);

    if(!requiresReader.isEmpty() && !QImageReader::supportedImageFormats().contains(requiresReader)) {
        QSKIP(
            qPrintable(QStringLiteral("no '%1' image plugin on this system").arg(QString::fromLatin1(requiresReader))));
    }

    const QString path = dataPath(file);
    QVERIFY2(QFile::exists(path), qPrintable(path));

    DocumentInfo info(path);
    QCOMPARE(int(info.type()), expectedType);
}

// Detection says "this file is animated"; this checks that the installed
// plugin can actually decode it, which is what decides whether the user sees
// an animation or one frozen frame.
void Test_FormatDetect::decodesAnimatedAvif() {
    if(!QImageReader::supportedImageFormats().contains(QByteArrayLiteral("avif")))
        QSKIP("no 'avif' image plugin on this system (install kimageformats)");

    const QString path = dataPath(QStringLiteral("real_anim.avif"));
    QVERIFY2(QFile::exists(path), qPrintable(path));

    QImageReader reader(path);
    QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
    QCOMPARE(reader.format(), QByteArray("avif"));
    QVERIFY(reader.supportsAnimation());
    QCOMPARE(reader.imageCount(), 10);

    // qimgv drives ANIMATED documents through QMovie, so cover that path too.
    QMovie movie(path);
    movie.setFormat("avif");
    QVERIFY(movie.isValid());
    QCOMPARE(movie.frameCount(), 10);

    for(int i = 0; i < 10; i++) {
        QVERIFY(movie.jumpToFrame(i));
        QVERIFY2(!movie.currentImage().isNull(), qPrintable(QStringLiteral("frame %1 decoded to a null image").arg(i)));
    }
}

// make-fixtures.sh grows a new fixture now and then; this makes sure it also gets an expectation above.
void Test_FormatDetect::everyAudioFixtureIsListed() {
    QStringList listed;
    for(const AudioFixture &f : kAudioFixtures)
        listed << QLatin1String(f.file);
    QStringList present = QDir(dataPath("audio")).entryList(QDir::Files, QDir::Name);
    present.removeAll(QStringLiteral("make-fixtures.sh"));
    present.sort();
    listed.sort();
    QCOMPARE(present, listed);
}

void Test_FormatDetect::probesFixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int>("kind");
    QTest::addColumn<QByteArray>("container");
    for(const AudioFixture &f : kAudioFixtures)
        QTest::newRow(f.file) << QStringLiteral("audio/") + QLatin1String(f.file) << int(f.kind)
                              << QByteArray(f.container);
}

// The probe on its own: DocumentInfo would get most of these right through the mime type or the extension
// even with the probe broken.
void Test_FormatDetect::probesFixtures() {
    QFETCH(QString, file);
    QFETCH(int, kind);
    QFETCH(QByteArray, container);
    const MediaProbe::Result result = MediaProbe::probe(dataPath(file));
    QCOMPARE(int(result.kind), kind);
    QCOMPARE(result.container, container);
}

void Test_FormatDetect::detectsBuiltFiles_data() {
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("kind");
    QTest::addColumn<QByteArray>("container");
    QTest::addColumn<int>("expectedType");
    const QByteArray flac = fixture("audio/flac-picture.flac");
    QVERIFY(!flac.isEmpty());
    for(const Built &b : builtFiles(flac)) {
        QTest::newRow(b.name) << QString::fromLatin1(b.fileName) << b.bytes << int(b.kind) << QByteArray(b.container)
                              << b.type;
    }
}

void Test_FormatDetect::detectsBuiltFiles() {
    QFETCH(QString, fileName);
    QFETCH(QByteArray, bytes);
    QFETCH(int, kind);
    QFETCH(QByteArray, container);
    QFETCH(int, expectedType);

    const QString path = writeTemp(fileName, bytes);
    QVERIFY(!path.isEmpty());
    const MediaProbe::Result result = MediaProbe::probe(path);
    QCOMPARE(int(result.kind), kind);
    QCOMPARE(result.container, container);
    if(expectedType != kAnyType) {
        DocumentInfo info(path);
        QCOMPARE(int(info.type()), expectedType);
    }
}

void Test_FormatDetect::namesFormat_data() {
    QTest::addColumn<QString>("fixtureFile");
    QTest::addColumn<QString>("copyAs");
    QTest::addColumn<int>("expectedType");
    QTest::addColumn<QString>("format");

    // An audio extension names the format...
    QTest::newRow("flac") << "audio/flac-picture.flac" << QString() << int(AUDIO) << "flac";
    QTest::newRow("vorbis") << "audio/vorbis-picture.ogg" << QString() << int(AUDIO) << "ogg";
    QTest::newRow("matroska audio") << "audio/matroska-cover.mka" << QString() << int(AUDIO) << "mka";
    QTest::newRow("wma") << "audio/wma.wma" << QString() << int(AUDIO) << "wma";
    QTest::newRow("upper-case extension") << "audio/flac-picture.flac" << "LOUD.FLAC" << int(AUDIO) << "flac";
    // ...and so does a video one: a .3gp holding only sound is still a .3gp.
    QTest::newRow("audio-only mp4") << "audio/aac-isom.mp4" << QString() << int(AUDIO) << "mp4";
    QTest::newRow("audio-only webm") << "audio/opus-audio-only.webm" << QString() << int(AUDIO) << "webm";
    QTest::newRow("audio-only 3gp") << "audio/aac-isom.mp4" << "voice-memo.3gp" << int(AUDIO) << "3gp";
    QTest::newRow("audio-only mkv") << "audio/matroska-cover.mka" << "sound.mkv" << int(AUDIO) << "mkv";
    QTest::newRow("audio-only wmv") << "audio/wma.wma" << "sound.wmv" << int(AUDIO) << "wmv";
    // Without either, the container does, by the extension its files usually have.
    QTest::newRow("no extension at all") << "audio/flac-picture.flac" << "flac-without-extension" << int(AUDIO)
                                         << "flac";
    QTest::newRow("matroska, no extension") << "audio/matroska-cover.mka" << "matroska-audio" << int(AUDIO) << "mka";
    QTest::newRow("asf, no extension") << "audio/wma.wma" << "asf-audio" << int(AUDIO) << "wma";
    QTest::newRow("wavpack, no extension") << "audio/wavpack-ape.wv" << "wavpack-audio" << int(AUDIO) << "wv";
    QTest::newRow("matroska, foreign extension") << "audio/matroska-cover.mka" << "sound.bin" << int(AUDIO) << "mka";
    // Video is named by its extension when that is a video one, whatever the mime database calls the type.
    QTest::newRow("theora") << "audio/theora-video.ogg" << QString() << int(VIDEO) << "ogg";
    QTest::newRow("theora as .ogv") << "audio/theora-video.ogg" << "clip.ogv" << int(VIDEO) << "ogv";
    QTest::newRow("vp8") << "audio/vp8-video.webm" << QString() << int(VIDEO) << "webm";
    QTest::newRow("mpeg-4") << "audio/mpeg4-video.mp4" << QString() << int(VIDEO) << "mp4";
    QTest::newRow("mpeg-4 as .mov") << "audio/mpeg4-video.mp4" << "clip.mov" << int(VIDEO) << "mov";
    QTest::newRow("video in .mka") << "audio/video-misnamed.mka" << QString() << int(VIDEO) << "mka";
}

void Test_FormatDetect::namesFormat() {
    QFETCH(QString, fixtureFile);
    QFETCH(QString, copyAs);
    QFETCH(int, expectedType);
    QFETCH(QString, format);

    QString path = dataPath(fixtureFile);
    if(!copyAs.isEmpty())
        path = writeTemp(copyAs, fixture(fixtureFile));
    QVERIFY(!path.isEmpty());
    DocumentInfo info(path);
    QCOMPARE(int(info.type()), expectedType);
    QCOMPARE(info.format(), format);
}

// Off means the files behave exactly as before audio support: a Vorbis .ogg is a video by its extension, and
// an .mp3 is nothing playable.
void Test_FormatDetect::audioPlaybackOff() {
    const bool before = settings->audioPlayback();
    const auto restore = qScopeGuard([before] { settings->setAudioPlayback(before); });
    settings->setAudioPlayback(false);

    QCOMPARE(int(DocumentInfo(dataPath("audio/vorbis-picture.ogg")).type()), int(VIDEO));
    QCOMPARE(int(DocumentInfo(dataPath("audio/opus-audio-only.webm")).type()), int(VIDEO));
    QCOMPARE(int(DocumentInfo(dataPath("audio/theora-video.ogg")).type()), int(VIDEO));
    QCOMPARE(int(DocumentInfo(dataPath("audio/mp3-id3v24.mp3")).type()), int(STATIC));
    QCOMPARE(int(DocumentInfo(dataPath("audio/flac-picture.flac")).type()), int(STATIC));
    QCOMPARE(int(DocumentInfo(dataPath("audio/matroska-cover.mka")).type()), int(STATIC));
}

// Audio still plays with video off, and a video never turns into audio for want of the video player. Nor
// into an image: the probe has seen that it is a video, and it fails to load as one that cannot be played.
void Test_FormatDetect::videoPlaybackOff() {
    const bool before = settings->videoPlayback();
    const auto restore = qScopeGuard([before] { settings->setVideoPlayback(before); });
    settings->setVideoPlayback(false);

    QCOMPARE(int(DocumentInfo(dataPath("audio/vorbis-picture.ogg")).type()), int(AUDIO));
    QCOMPARE(int(DocumentInfo(dataPath("audio/aac-isom.mp4")).type()), int(AUDIO));
    QCOMPARE(int(DocumentInfo(dataPath("audio/theora-video.ogg")).type()), int(NONE));
    QCOMPARE(int(DocumentInfo(dataPath("audio/vp8-video.webm")).type()), int(NONE));
    QCOMPARE(int(DocumentInfo(dataPath("audio/video-misnamed.mka")).type()), int(NONE));
    QCOMPARE(int(DocumentInfo(dataPath("audio/mpeg4-video.mp4")).type()), int(NONE));
    // Images are not probed, so they stay what they are.
    QCOMPARE(int(DocumentInfo(dataPath("audio/cover-red.png")).type()), int(STATIC));
}

// With neither kind of playback on, nothing is probed, and a video opens the way it did before either existed.
void Test_FormatDetect::allPlaybackOff() {
    const bool audioBefore = settings->audioPlayback();
    const bool videoBefore = settings->videoPlayback();
    const auto restore = qScopeGuard([=] {
        settings->setAudioPlayback(audioBefore);
        settings->setVideoPlayback(videoBefore);
    });
    settings->setAudioPlayback(false);
    settings->setVideoPlayback(false);

    QCOMPARE(int(DocumentInfo(dataPath("audio/theora-video.ogg")).type()), int(STATIC));
    QCOMPARE(int(DocumentInfo(dataPath("audio/vorbis-picture.ogg")).type()), int(STATIC));
    QCOMPARE(int(DocumentInfo(dataPath("audio/mp3-id3v24.mp3")).type()), int(STATIC));
}

void Test_FormatDetect::probeQuickly(const QByteArray &bytes, const char *what) {
    QBuffer buffer;
    buffer.setData(bytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QElapsedTimer timer;
    timer.start();
    const MediaProbe::Result result = MediaProbe::probe(buffer);
    const qint64 ms = timer.elapsed();
    // Generous for a loaded CI machine; a walk that is not bounded takes minutes, or forever.
    QVERIFY2(ms < 500, qPrintable(QStringLiteral("%1: %2 ms").arg(QLatin1String(what)).arg(ms)));
    QVERIFY(result.kind == Kind::Unknown || result.kind == Kind::Audio || result.kind == Kind::Video);
}

// Every fixture and built file cut short at every point in its first kilobyte and at intervals after, and
// with bytes overwritten at random (fixed seed, so a failure reproduces). In memory: the same walk as on
// disk, without writing thousands of files.
void Test_FormatDetect::probeSurvivesDamagedFiles() {
    QList<QByteArray> originals;
    for(const AudioFixture &f : kAudioFixtures)
        originals << fixture(QStringLiteral("audio/") + QLatin1String(f.file));
    for(const Built &b : builtFiles(fixture("audio/flac-picture.flac")))
        originals << b.bytes;

    QRandomGenerator rng(20261007);
    for(const QByteArray &original : std::as_const(originals)) {
        QVERIFY(!original.isEmpty());
        for(qsizetype cut = 0; cut < original.size(); cut += cut < 1024 ? 1 : 61) {
            probeQuickly(original.left(cut), "truncated");
            if(QTest::currentTestFailed())
                return;
        }
        for(int variant = 0; variant < 64; variant++) {
            QByteArray damaged = original;
            const int changes = 1 + rng.bounded(16);
            for(int i = 0; i < changes; i++) {
                // Mostly the headers, where the sizes and counts are.
                const qsizetype limit = rng.bounded(4) == 0 ? damaged.size() : qMin<qsizetype>(damaged.size(), 512);
                damaged[rng.bounded(int(limit))] = char(rng.bounded(256));
            }
            probeQuickly(damaged, "corrupt");
            if(QTest::currentTestFailed())
                return;
        }
    }
}

// Structures built to make a careless walker loop, overflow or read the whole file.
void Test_FormatDetect::probeSurvivesHostileFiles() {
    QList<QPair<const char *, QByteArray>> files;

    // 200,000 empty boxes ahead of where moov would be.
    QByteArray boxes = ftyp("isom", {"isom"});
    boxes.reserve(boxes.size() + 8 * 200000);
    for(int i = 0; i < 200000; i++)
        boxes += box("free", {});
    boxes += moov(trak("soun", true));
    files << qMakePair("mp4, endless tiny boxes", boxes);

    files << qMakePair("mp4, 64-bit size at the maximum",
                       ftyp("isom", {}) + bigEndian<quint32>(1) + "moov" + QByteArray::fromHex("ffffffffffffffff"));
    files << qMakePair("mp4, size smaller than its header", ftyp("isom", {}) + bigEndian<quint32>(4) + "moov");
    files << qMakePair("mp4, ftyp claiming 4 GiB", bigEndian<quint32>(0xFFFFFFFF) + "ftypisom");

    // A Segment of nothing but two-byte Void elements.
    QByteArray voids;
    voids.reserve(2 * 300000);
    for(int i = 0; i < 300000; i++)
        voids += QByteArray::fromHex("ec80");
    files << qMakePair("matroska, endless voids", ebmlHeader("matroska") + ebmlUnknownSize(0x18538067, voids));
    files << qMakePair("matroska, SeekHead pointing at itself",
                       ebmlHeader("matroska") + ebmlUnknownSize(0x18538067, seekHead(0x114D9B74, 0) + cluster(true)));
    files << qMakePair("matroska, SeekHead pointing past the end",
                       ebmlHeader("matroska") +
                           ebmlUnknownSize(0x18538067, seekHead(0x1654AE6B, 0x00FFFFFFFFFFFFFF) + cluster(true)));
    files << qMakePair("matroska, zero ID bytes", ebmlHeader("matroska") + ebmlUnknownSize(0x18538067, zeros(4096)));
    files << qMakePair("matroska, size longer than eight bytes", QByteArray::fromHex("1a45dfa300") + zeros(64));

    // An object count of four billion, and objects of the minimum size.
    QByteArray objects;
    for(int i = 0; i < 100000; i++)
        objects += asfObject(kAsfFileProperties, {});
    files << qMakePair("asf, endless objects", QByteArray::fromHex(kAsfHeader) +
                                                   littleEndian<quint64>(quint64(30 + objects.size())) +
                                                   littleEndian<quint32>(0xFFFFFFFF) + "\x01\x02" + objects);
    // 65,535 empty stream names, and as many payload extension systems claimed after them.
    QByteArray names;
    for(int i = 0; i < 65535; i++)
        names += zeros(4);
    files << qMakePair("asf, endless stream names",
                       asfFile({asfHeaderExtension(
                           asfObject(kAsfExtendedStreamProperties, zeros(60) + littleEndian<quint16>(0xFFFF) +
                                                                       littleEndian<quint16>(0xFFFF) + names))}));
    files << qMakePair("asf, object of size zero", QByteArray::fromHex(kAsfHeader) + littleEndian<quint64>(100) +
                                                       littleEndian<quint32>(1) + "\x01\x02" +
                                                       QByteArray::fromHex(kAsfStreamProperties) + zeros(70));

    QByteArray pages;
    for(quint32 i = 0; i < 1000; i++)
        pages += oggPage(true, i, skeletonHead());
    files << qMakePair("ogg, a thousand streams", pages + oggPage(true, 1000, vorbisId()));
    files << qMakePair("ogg, page with no segments", QByteArray("OggS\0\x02", 6) + zeros(20) + zeros(64));
    files << qMakePair("ogg, lacing past the end", QByteArray("OggS\0\x02", 6) + zeros(20) + '\xFF' + zeros(8));

    files << qMakePair("id3, largest size", QByteArray::fromHex("4944330400007f7f7f7f") + zeros(64));
    files << qMakePair("empty", QByteArray());

    for(const auto &[what, bytes] : std::as_const(files)) {
        probeQuickly(bytes, what);
        if(QTest::currentTestFailed())
            return;
    }

    // The probe gives up on the endless boxes before the moov behind them, rather than read 1.6 MB of headers
    // to get there. On disk too, where the reads are real.
    const QString path = writeTemp(QStringLiteral("boxes.mp4"), files.first().second);
    QVERIFY(!path.isEmpty());
    QCOMPARE(int(MediaProbe::probe(path).kind), int(Kind::Unknown));
}

QTEST_MAIN(Test_FormatDetect)
#include "test_formatdetect.moc"
