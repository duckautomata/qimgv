// Tag parsers, group B: FLAC, Vorbis comments, Ogg (Vorbis, Opus, FLAC, Speex), MP4 and ASF.
//
// Files made by ffmpeg (data/audio; make-fixtures.sh lists the tags they carry) cover the layouts found in
// the wild. Byte sequences built here cover what ffmpeg does not write -- a back cover before the front,
// Ogg packets split at awkward places, multiplexed streams, QuickTime-style atoms, WM/Picture -- and the
// edge cases. Every sample then goes through the robustness pass: cut short and corrupted, each parse
// must return, and quickly.
#include <QtTest>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QtEndian>

#include <array>

#include "audiometa_p.h"

using namespace AudioMeta;

static bool operator==(AudioField const &a, AudioField const &b) {
    return a.key == b.key && a.value == b.value;
}

namespace {

constexpr int kAll = AudioMetadataReader::All;
constexpr int kTags = AudioMetadataReader::Tags;
constexpr int kCover = AudioMetadataReader::Cover;

QString dataPath(QString const &name) {
    return QStringLiteral(QIMGV_TEST_DATA_DIR) + QStringLiteral("/audio/") + name;
}

QByteArray readFile(QString const &name) {
    QFile file(dataPath(name));
    if(!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QByteArray redPng() {
    static const QByteArray png = readFile(QStringLiteral("cover-red.png"));
    return png;
}

// ---------------------------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------------------------

template<typename T>
QByteArray bigEndian(T value) {
    QByteArray bytes(sizeof(T), Qt::Uninitialized);
    qToBigEndian(value, bytes.data());
    return bytes;
}

template<typename T>
QByteArray littleEndian(T value) {
    QByteArray bytes(sizeof(T), Qt::Uninitialized);
    qToLittleEndian(value, bytes.data());
    return bytes;
}

QByteArray u16be(quint16 v) {
    return bigEndian(v);
}
QByteArray u32be(quint32 v) {
    return bigEndian(v);
}
QByteArray u64be(quint64 v) {
    return bigEndian(v);
}
QByteArray u16le(quint16 v) {
    return littleEndian(v);
}
QByteArray u32le(quint32 v) {
    return littleEndian(v);
}
QByteArray u64le(quint64 v) {
    return littleEndian(v);
}
QByteArray u24be(quint32 v) {
    return u32be(v).mid(1);
}

QByteArray utf16le(QString const &text, bool terminated = true) {
    QByteArray bytes;
    for(QChar c : text)
        bytes += u16le(c.unicode());
    if(terminated)
        bytes += u16le(0);
    return bytes;
}

// Picks out a field by the key the info panel shows.
QString fieldValue(AudioMetadata const &meta, QString const &key) {
    for(AudioField const &field : meta.fields) {
        if(field.key == key)
            return field.value;
    }
    return {};
}

// --- FLAC and Vorbis comments ---

QByteArray flacBlock(quint8 type, QByteArray const &body, bool last = false) {
    return char(type | (last ? 0x80 : 0)) + u24be(quint32(body.size())) + body;
}

QByteArray flacPicture(quint32 type, QByteArray const &mime, QByteArray const &description, QByteArray const &data) {
    return u32be(type) + u32be(quint32(mime.size())) + mime + u32be(quint32(description.size())) + description +
           u32be(16) + u32be(16) + u32be(24) + u32be(0) + u32be(quint32(data.size())) + data;
}

QByteArray vorbisComment(QList<QByteArray> const &entries, QByteArray const &vendor = "qimgv test") {
    QByteArray out = u32le(quint32(vendor.size())) + vendor + u32le(quint32(entries.size()));
    for(QByteArray const &entry : entries)
        out += u32le(quint32(entry.size())) + entry;
    return out;
}

// 4096-sample blocks, 44.1 kHz, stereo, 16-bit, length unknown, no MD5. The parsers only need the
// block's length to be right; real values keep the samples valid FLAC for other tools.
QByteArray streamInfo() {
    const quint64 format = (quint64(44100) << 44) | (quint64(2 - 1) << 41) | (quint64(16 - 1) << 36);
    return u16be(4096) + u16be(4096) + u24be(0) + u24be(0) + u64be(format) + QByteArray(16, '\0');
}

QByteArray flacFile(QByteArray const &blocks) {
    return "fLaC" + flacBlock(0, streamInfo()) + blocks + QByteArray("\xFF\xF8\x69\x08", 4) + QByteArray(60, 'a');
}

QByteArray flacBackThenFront() {
    return flacFile(flacBlock(4, vorbisComment({"TITLE=Back then front"})) +
                    flacBlock(6, flacPicture(4, "image/jpeg", "back", "not really a jpeg")) +
                    flacBlock(1, QByteArray(100, '\0')) +
                    flacBlock(6, flacPicture(3, "image/png", "front", redPng()), true));
}

QByteArray metadataBlockPicture(QByteArray const &block) {
    return "METADATA_BLOCK_PICTURE=" + block.toBase64();
}

// --- Ogg ---

quint32 oggCrc(QByteArray const &page) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for(quint32 i = 0; i < 256; i++) {
            quint32 r = i << 24;
            for(int bit = 0; bit < 8; bit++)
                r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
            t[i] = r;
        }
        return t;
    }();
    quint32 crc = 0;
    for(char c : page)
        crc = (crc << 8) ^ table[((crc >> 24) ^ quint8(c)) & 0xFF];
    return crc;
}

// A complete page with a valid CRC, so the samples are real Ogg (ffprobe reads them), not merely what
// this parser accepts.
QByteArray oggPage(quint8 flags, quint32 serial, quint32 sequence, QByteArray const &lacing, QByteArray const &body) {
    QByteArray page = QByteArray("OggS", 4) + char(0) + char(flags) + u64le(0) + u32le(serial) + u32le(sequence) +
                      u32le(0) + char(lacing.size()) + lacing + body;
    const QByteArray crc = u32le(oggCrc(page));
    page.replace(22, 4, crc);
    return page;
}

QByteArray lacingFor(QByteArray const &packet) {
    return QByteArray(packet.size() / 255, '\xFF') + char(packet.size() % 255);
}

// Lays `packets` out as one logical stream: the first packet alone on the BOS page, as the spec requires,
// then the rest `segmentsPerPage` lacing values to a page, with the continued flag wherever a page starts
// inside a packet and EOS on the last page.
QList<QByteArray> oggStream(quint32 serial, QList<QByteArray> const &packets, int segmentsPerPage) {
    QList<QByteArray> pages;
    quint32 sequence = 0;
    pages.append(oggPage(0x02, serial, sequence++, lacingFor(packets.first()), packets.first()));
    QByteArray lacing, data;
    for(qsizetype i = 1; i < packets.size(); i++) {
        lacing += lacingFor(packets.at(i));
        data += packets.at(i);
    }
    qsizetype segment = 0, at = 0;
    bool continued = false;
    while(segment < lacing.size()) {
        const QByteArray pageLacing = lacing.mid(segment, segmentsPerPage);
        qsizetype bodySize = 0;
        for(char value : pageLacing)
            bodySize += quint8(value);
        segment += pageLacing.size();
        const quint8 flags = (continued ? 0x01 : 0) | (segment == lacing.size() ? 0x04 : 0);
        pages.append(oggPage(flags, serial, sequence++, pageLacing, data.mid(at, bodySize)));
        at += bodySize;
        continued = quint8(pageLacing.back()) == 255;
    }
    return pages;
}

QByteArray vorbisIdentification() {
    return QByteArray("\x01vorbis", 7) + u32le(0) + char(1) + u32le(44100) + u32le(0) + u32le(128000) + u32le(0) +
           char(0xB8) + char(1);
}

// "\x03vorbis", the comment, and the framing bit -- made exactly `length` bytes long by padding an entry,
// so a test can put the packet's end exactly where it wants it.
QByteArray vorbisCommentPacket(QList<QByteArray> entries, qsizetype length) {
    auto build = [&](QByteArray const &pad) {
        QList<QByteArray> all = entries;
        all.append("PADDING=" + pad);
        return QByteArray("\x03vorbis", 7) + vorbisComment(all) + char(1);
    };
    const qsizetype base = build({}).size();
    return build(QByteArray(qMax<qsizetype>(0, length - base), 'p'));
}

QByteArray joinPages(QList<QByteArray> const &pages) {
    QByteArray out;
    for(QByteArray const &page : pages)
        out += page;
    return out;
}

bool pageContinues(QByteArray const &page) {
    return page.at(5) & 0x01;
}

QByteArray pageLacing(QByteArray const &page) {
    return page.mid(27, quint8(page.at(26)));
}

// The comment packet spans pages of 255-runs and ends exactly at a page boundary, so the setup packet
// starts on a fresh, not-continued page.
QList<QByteArray> oggVorbisSpanning() {
    const QByteArray picture = flacPicture(3, "image/png", {}, redPng());
    const QByteArray comment = vorbisCommentPacket(
        {"TITLE=Spanning", "ARTIST=qimgv", "ALBUM=Pages", metadataBlockPicture(picture)}, 255 * 20 + 100);
    const QByteArray setup = QByteArray("\x05vorbis", 7) + QByteArray(300, 's');
    // 21 lacing values for the comment, 7 to a page: the comment's last value is the third page's last.
    return oggStream(0x1234, {vorbisIdentification(), comment, setup, QByteArray(40, 'a')}, 7);
}

// OpusTags exactly 12 * 255 bytes long: its lacing is twelve 255s and a 0, and the 0 that ends it is the
// first value of the third page.
QList<QByteArray> oggOpusZeroTerminated() {
    const QByteArray head =
        QByteArray("OpusHead", 8) + char(1) + char(2) + u16le(312) + u32le(48000) + u16le(0) + char(0);
    QByteArray tags = QByteArray("OpusTags", 8) + vorbisComment({"TITLE=Zero", "ARTIST=Opus"});
    tags += QByteArray(12 * 255 - tags.size(), '\0');
    return oggStream(0x99, {head, tags, QByteArray(20, 'a'), QByteArray(20, 'b')}, 6);
}

// A Theora stream first, whose own comment header must not be taken for the audio's.
QByteArray buildOggMultiplexed() {
    const QByteArray theoraId = QByteArray("\x80theora", 7) + QByteArray(35, 't');
    const QByteArray theoraComment = QByteArray("\x81theora", 7) + vorbisComment({"TITLE=Video title"});
    const QByteArray theoraSetup = QByteArray("\x82theora", 7) + QByteArray(600, 'v');
    const QList<QByteArray> video = oggStream(1, {theoraId, theoraComment, theoraSetup, QByteArray(500, 'f')}, 2);
    const QByteArray comment = vorbisCommentPacket({"TITLE=Audio title", "ARTIST=Muxed"}, 900);
    const QList<QByteArray> audio =
        oggStream(2, {vorbisIdentification(), comment, QByteArray("\x05vorbis", 7) + QByteArray(50, 's')}, 2);
    // Both BOS pages first, then the streams' pages interleaved.
    QByteArray out = video.at(0) + audio.at(0);
    for(qsizetype i = 1; i < qMax(video.size(), audio.size()); i++) {
        if(i < video.size())
            out += video.at(i);
        if(i < audio.size())
            out += audio.at(i);
    }
    return out;
}

QByteArray oggFlacMapping(quint16 headerPackets) {
    return QByteArray("\x7F", 1) + "FLAC" + char(1) + char(0) + u16be(headerPackets) + "fLaC" +
           flacBlock(0, streamInfo());
}

QByteArray oggFlacWithPicture(quint16 headerPackets) {
    return joinPages(
        oggStream(7,
                  {oggFlacMapping(headerPackets), flacBlock(4, vorbisComment({"TITLE=Ogg FLAC", "ARTIST=qimgv"})),
                   flacBlock(6, flacPicture(3, "image/png", "front", redPng()), true),
                   QByteArray("\xFF\xF8", 2) + QByteArray(30, 'a')},
                  255));
}

// --- MP4 ---

QByteArray box(QByteArray const &type, QByteArray const &payload) {
    return u32be(quint32(8 + payload.size())) + type + payload;
}

QByteArray largeBox(QByteArray const &type, QByteArray const &payload) {
    return u32be(1) + type + u64be(quint64(16 + payload.size())) + payload;
}

QByteArray fullBox(QByteArray const &type, QByteArray const &payload) {
    return box(type, u32be(0) + payload);
}

QByteArray dataBox(quint32 type, QByteArray const &value) {
    return box("data", u32be(type) + u32be(0) + value);
}

QByteArray textItem(QByteArray const &type, QString const &value) {
    return box(type, dataBox(1, value.toUtf8()));
}

QByteArray hdlr(QByteArray const &handler) {
    return fullBox("hdlr", u32be(0) + handler + QByteArray(12, '\0') + char(0));
}

QByteArray ftyp() {
    return box("ftyp", "M4A " + u32be(0x200) + "M4A isom");
}

QByteArray standardItems() {
    return textItem("\251nam", "Fixture") + textItem("\251ART", "qimgv") + textItem("\251alb", "Test Album") +
           textItem("aART", "Various") + textItem("\251day", "2026") + textItem("\251gen", "Ambient") +
           box("trkn", dataBox(0, u16be(0) + u16be(3) + u16be(12) + u16be(0)));
}

QByteArray mp4File(QByteArray const &moovChildren, QByteArray const &mdat) {
    return ftyp() + mdat + box("moov", fullBox("mvhd", QByteArray(96, '\0')) + moovChildren);
}

// moov after an mdat with a 64-bit size, the layout of every file over 4 GiB.
QByteArray buildMp4LargeMdat() {
    return mp4File(box("udta", fullBox("meta", hdlr("mdir") + box("ilst", standardItems()))),
                   largeBox("mdat", QByteArray(200, 'm')));
}

// The meta box without its version and flags, as QuickTime writes it.
QByteArray buildMp4QuickTimeMeta() {
    return mp4File(box("udta", box("meta", hdlr("mdir") + box("ilst", standardItems()))), box("mdat", "abc"));
}

QByteArray freeform(QByteArray const &mean, QByteArray const &name, QByteArray const &value) {
    return box("----", fullBox("mean", mean) + fullBox("name", name) + dataBox(1, value));
}

QByteArray buildMp4Items() {
    const QByteArray items =
        textItem("\251nam", "Items") + box("\251ART", dataBox(1, "First") + dataBox(1, "Second")) +
        box("gnre", dataBox(0, u16be(18))) + box("disk", dataBox(0, u16be(0) + u16be(1) + u16be(2))) +
        box("trkn", dataBox(0, u16be(0) + u16be(7) + u16be(0) + u16be(0))) +
        freeform("com.apple.iTunes", "REPLAYGAIN_TRACK_GAIN", "-6.50 dB") +
        freeform("com.apple.iTunes", "iTunNORM", " 00000123 00000456") + box("tmpo", dataBox(21, u16be(120))) +
        box("cpil", dataBox(21, QByteArray(1, '\1'))) + textItem("\251too", "Lavf") + textItem("\251xyz", "Unknown") +
        box(QByteArray("\0\0\0\1", 4), dataBox(1, "keyed by index")) +
        box("covr", dataBox(13, "first, front") + dataBox(14, redPng()));
    return mp4File(box("udta", fullBox("meta", hdlr("mdir") + box("ilst", items))), box("mdat", "abc"));
}

QByteArray quickTimeText(QByteArray const &text, quint16 language) {
    return u16be(quint16(text.size())) + u16be(language) + text;
}

// User data text atoms and no iTunes list. 0x55C4 is "und" packed as ISO 639-2, so UTF-8; 0 is the Mac
// code for English, so Mac Roman (0x8E is e-acute) -- unless the writer put UTF-8 there anyway.
QByteArray buildMp4TextAtoms() {
    return mp4File(box("udta", box("\251nam", quickTimeText("Fixture \xC3\xBC", 0x55C4)) +
                                   box("\251ART", quickTimeText("", 0) + quickTimeText("qimgv", 0)) +
                                   box("\251day", quickTimeText("2026", 0x55C4)) +
                                   box("\251cmt", quickTimeText("caf\x8E", 0)) +
                                   box("\251alb", quickTimeText("Caf\xC3\xA9", 0))),
                   box("mdat", "abc"));
}

// --- ASF ---

QByteArray guid(quint32 data1, quint16 data2, quint16 data3, quint64 data4) {
    return u32le(data1) + u16le(data2) + u16le(data3) + u64be(data4);
}

const QByteArray kAsfHeader = guid(0x75B22630, 0x668E, 0x11CF, 0xA6D900AA0062CE6C);
const QByteArray kAsfContentDescription = guid(0x75B22633, 0x668E, 0x11CF, 0xA6D900AA0062CE6C);
const QByteArray kAsfExtendedContentDescription = guid(0xD2D0A440, 0xE307, 0x11D2, 0x97F000A0C95EA850);
const QByteArray kAsfHeaderExtension = guid(0x5FBF03B5, 0xA92E, 0x11CF, 0x8EE300C00C205365);
const QByteArray kAsfReserved1 = guid(0xABD3D211, 0xA9BA, 0x11CF, 0x8EE600C00C205365);
const QByteArray kAsfMetadata = guid(0xC5F8CBEA, 0x5BAF, 0x4877, 0x8467AA8C44FA4CCA);
const QByteArray kAsfMetadataLibrary = guid(0x44231C94, 0x9498, 0x49D1, 0xA1411D134E457054);
const QByteArray kAsfData = guid(0x75B22636, 0x668E, 0x11CF, 0xA6D900AA0062CE6C);

QByteArray asfObject(QByteArray const &id, QByteArray const &payload) {
    return id + u64le(quint64(24 + payload.size())) + payload;
}

QByteArray asfFile(QList<QByteArray> const &objects) {
    QByteArray children;
    for(QByteArray const &object : objects)
        children += object;
    const QByteArray data = asfObject(kAsfData, QByteArray(16, 'f') + u64le(0) + char(1) + char(1));
    return kAsfHeader + u64le(quint64(30 + children.size())) + u32le(quint32(objects.size())) + char(1) + char(2) +
           children + data;
}

QByteArray asfContentDescription(QStringList const &strings) {
    QByteArray lengths, text;
    for(QString const &s : strings) {
        const QByteArray encoded = utf16le(s);
        lengths += u16le(quint16(encoded.size()));
        text += encoded;
    }
    return asfObject(kAsfContentDescription, lengths + text);
}

struct Attribute {
    QString name;
    quint16 type;
    QByteArray value;
};

QByteArray asfString(QString const &s) {
    return utf16le(s);
}

QByteArray asfExtended(QList<Attribute> const &attributes) {
    QByteArray body = u16le(quint16(attributes.size()));
    for(Attribute const &a : attributes) {
        const QByteArray name = utf16le(a.name);
        body += u16le(quint16(name.size())) + name + u16le(a.type) + u16le(quint16(a.value.size())) + a.value;
    }
    return asfObject(kAsfExtendedContentDescription, body);
}

QByteArray asfRecords(QByteArray const &id, QList<Attribute> const &attributes) {
    QByteArray body = u16le(quint16(attributes.size()));
    for(Attribute const &a : attributes) {
        const QByteArray name = utf16le(a.name);
        body += u16le(0) + u16le(0) + u16le(quint16(name.size())) + u16le(a.type) + u32le(quint32(a.value.size())) +
                name + a.value;
    }
    return asfObject(id, body);
}

QByteArray asfHeaderExtension(QByteArray const &nested) {
    return asfObject(kAsfHeaderExtension, kAsfReserved1 + u16le(6) + u32le(quint32(nested.size())) + nested);
}

QByteArray wmPicture(quint8 type, QString const &mime, QString const &description, QByteArray const &data) {
    return char(type) + u32le(quint32(data.size())) + utf16le(mime) + utf16le(description) + data;
}

// A back cover in the Extended Content Description and the front in the Metadata Library, which is
// where pictures over 64 KiB have to go.
QByteArray buildAsfPictures() {
    return asfFile({asfContentDescription({"Pictures", "qimgv", "", "", ""}),
                    asfExtended({{"WM/Picture", 1, wmPicture(4, "image/jpeg", "back", "not really a jpeg")},
                                 {"WM/AlbumTitle", 0, asfString("Album")}}),
                    asfHeaderExtension(asfRecords(kAsfMetadataLibrary,
                                                  {{"WM/Picture", 1, wmPicture(3, "image/png", "front", redPng())}}))});
}

QByteArray buildAsfAttributes() {
    return asfFile({asfContentDescription({"Title", "Author", "(c) 2026", "A description", "PG"}),
                    asfExtended({{"WM/TrackNumber", 3, u32le(5)},
                                 {"WM/Track", 3, u32le(9)},
                                 {"WM/Year", 0, asfString("1999")},
                                 {"WM/GenreID", 0, asfString("(17)")},
                                 {"WM/PartOfSet", 0, asfString("1/2")},
                                 {"IsVBR", 2, u32le(1)},
                                 {"WM/EncodingTime", 4, u64le(1234567890123ull)},
                                 {"WM/MediaClassPrimaryID", 6, QByteArray(16, 'g')},
                                 {"WM/Publisher", 0, asfString("Label")}}),
                    asfHeaderExtension(asfRecords(kAsfMetadata, {{"WM/Composer", 0, asfString("Composer")},
                                                                 {"WM/Composer", 0, asfString("Another")},
                                                                 {"WM/BeatsPerMinute", 5, u16le(128)}}))});
}

// ---------------------------------------------------------------------------------------------------------
// Running the parsers
// ---------------------------------------------------------------------------------------------------------

enum class Parser { Flac, Ogg, Mp4, Asf, VorbisComment, FlacPicture };

bool run(Parser parser, ByteSource const &src, QByteArray const &bytes, AudioMetadata &meta, int parts) {
    switch(parser) {
    case Parser::Flac:
        return parseFlac(src, 0, meta, parts);
    case Parser::Ogg:
        return parseOgg(src, 0, meta, parts);
    case Parser::Mp4:
        return parseMp4(src, meta, parts);
    case Parser::Asf:
        return parseAsf(src, meta, parts);
    case Parser::VorbisComment:
        parseVorbisComment(bytes, meta, parts);
        return true;
    case Parser::FlacPicture:
        parseFlacPicture(bytes, meta, parts);
        return true;
    }
    return false;
}

AudioMetadata parse(Parser parser, QByteArray const &bytes, int parts = kAll, bool *recognised = nullptr) {
    AudioMetadata meta;
    const bool ok = run(parser, ByteSource(bytes), bytes, meta, parts);
    if(recognised)
        *recognised = ok;
    return meta;
}

struct Sample {
    QString name;
    Parser parser;
    QByteArray bytes;
};

} // namespace

Q_DECLARE_METATYPE(Parser)

class Test_Audiometa_B : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void fixtures_data();
    void fixtures();
    void foreignInputIsRejected();

    void flacFrontCoverWins();
    void flacStopsAtLastBlock();
    void flacTruncatedComment();
    void flacPictureLengthsAreChecked();

    void vorbisRepeatedKeysAndTotals();
    void vorbisAliasesAndUnknownKeys();
    void vorbisPictures();
    void vorbisLyingCounts();

    void oggCommentSpanningPages();
    void oggPacketEndingInZeroLacing();
    void oggMultiplexed();
    void oggFlacPicture_data();
    void oggFlacPicture();
    void oggWithoutAudio();
    void oggLargeCommentAcrossThousandsOfPages();
    void oggEmptyPagesAreBounded();

    void mp4MoovAfterLargeMdat();
    void mp4LargeMdatPastTheEnd();
    void mp4ChildInsideParent();
    void mp4QuickTimeMeta();
    void mp4Items();
    void mp4PartsMask();
    void mp4TextAtoms();

    void asfPictures();
    void asfAttributes();

    void flacAndOggFlacReadOneComment();
    void vorbisManyValuesOfOneKey();
    void mp4ManyValuesOfOneItem();
    void asfAttributesComeOutOfTheBudget();
    void asfManyValuesOfOneName();

    void robustness();

private:
    QList<Sample> samples() const;
};

void Test_Audiometa_B::initTestCase() {
    QVERIFY2(!redPng().isEmpty(), qPrintable(dataPath(QStringLiteral("cover-red.png"))));
}

// ---------------------------------------------------------------------------------------------------------
// ffmpeg's files: every tag make-fixtures.sh writes, and the cover where the container has one.
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::fixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<Parser>("parser");
    QTest::addColumn<QString>("tagType");
    QTest::addColumn<bool>("hasCover");

    QTest::newRow("flac") << "flac-picture.flac" << Parser::Flac << "Vorbis comment" << true;
    QTest::newRow("vorbis") << "vorbis-picture.ogg" << Parser::Ogg << "Vorbis comment" << true;
    QTest::newRow("opus") << "opus-picture.opus" << Parser::Ogg << "Vorbis comment" << true;
    QTest::newRow("ogg flac") << "flac-in-ogg.oga" << Parser::Ogg << "Vorbis comment" << false;
    QTest::newRow("speex") << "speex.spx" << Parser::Ogg << "Vorbis comment" << false;
    QTest::newRow("aac m4a") << "aac-cover.m4a" << Parser::Mp4 << "MP4" << true;
    QTest::newRow("alac m4a") << "alac.m4a" << Parser::Mp4 << "MP4" << false;
    QTest::newRow("aac isom mp4") << "aac-isom.mp4" << Parser::Mp4 << "MP4" << false;
    QTest::newRow("wma") << "wma.wma" << Parser::Asf << "ASF" << false;
}

void Test_Audiometa_B::fixtures() {
    QFETCH(QString, file);
    QFETCH(Parser, parser);
    QFETCH(QString, tagType);
    QFETCH(bool, hasCover);

    // Through the file, as the app reads it.
    const ByteSource src(dataPath(file));
    QVERIFY2(src.isValid(), qPrintable(dataPath(file)));
    AudioMetadata meta;
    QVERIFY(run(parser, src, {}, meta, kAll));

    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    QCOMPARE(meta.artist, QStringLiteral("qimgv"));
    QCOMPARE(meta.album, QStringLiteral("Test Album"));
    QCOMPARE(meta.albumArtist, QStringLiteral("Various"));
    QCOMPARE(meta.track, QStringLiteral("3/12"));
    QCOMPARE(meta.date, QStringLiteral("2026"));
    QCOMPARE(meta.genre, QStringLiteral("Ambient"));
    QCOMPARE(meta.tagTypes, QStringList{tagType});
    QCOMPARE(fieldValue(meta, fieldName(Field::Title)), QStringLiteral("Fixture"));
    QCOMPARE(fieldValue(meta, fieldName(Field::Track)), QStringLiteral("3/12"));
    // Each well-known field once, however many places the file names it in.
    QCOMPARE(meta.fields.count({fieldName(Field::Title), QStringLiteral("Fixture")}), qsizetype(1));
    if(hasCover) {
        QCOMPARE(meta.cover.data, redPng());
        QVERIFY(meta.cover.mimeType.isEmpty() || meta.cover.mimeType == QStringLiteral("image/png"));
    } else {
        QVERIFY(meta.cover.isNull());
    }

    // The parts mask: the thumbnailer asks for the cover alone, and gets no text.
    AudioMetadata coverOnly;
    QVERIFY(run(parser, src, {}, coverOnly, kCover));
    QVERIFY(coverOnly.fields.isEmpty());
    QCOMPARE(coverOnly.cover.data, meta.cover.data);
    AudioMetadata tagsOnly;
    QVERIFY(run(parser, src, {}, tagsOnly, kTags));
    QVERIFY(tagsOnly.cover.isNull());
    QCOMPARE(tagsOnly.fields.size(), meta.fields.size());
}

// Each parser, handed another format, says so and leaves the metadata alone.
void Test_Audiometa_B::foreignInputIsRejected() {
    const QByteArray flac = readFile(QStringLiteral("flac-picture.flac"));
    const QByteArray ogg = readFile(QStringLiteral("vorbis-picture.ogg"));
    const QByteArray m4a = readFile(QStringLiteral("aac-cover.m4a"));
    const QByteArray wma = readFile(QStringLiteral("wma.wma"));
    const QByteArray theora = readFile(QStringLiteral("theora-video.ogg"));
    const QList<std::pair<Parser, QByteArray>> cases = {
        {Parser::Flac, ogg}, {Parser::Flac, m4a}, {Parser::Ogg, flac}, {Parser::Ogg, wma}, {Parser::Ogg, theora},
        {Parser::Mp4, flac}, {Parser::Mp4, wma},  {Parser::Asf, m4a},  {Parser::Asf, ogg}, {Parser::Mp4, {}},
        {Parser::Asf, {}},   {Parser::Flac, {}},  {Parser::Ogg, {}},
    };
    for(auto const &[parser, bytes] : cases) {
        bool recognised = true;
        const AudioMetadata meta = parse(parser, bytes, kAll, &recognised);
        QVERIFY(!recognised);
        QVERIFY(meta.isEmpty());
        QVERIFY(meta.tagTypes.isEmpty());
    }
}

// ---------------------------------------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::flacFrontCoverWins() {
    const AudioMetadata meta = parse(Parser::Flac, flacBackThenFront());
    QCOMPARE(meta.title, QStringLiteral("Back then front"));
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.mimeType, QStringLiteral("image/png"));
    QCOMPARE(meta.cover.description, QStringLiteral("front"));

    // Behind an ID3v2 tag the dispatcher passes the offset of "fLaC".
    const QByteArray prefixed = QByteArray(100, 'x') + flacBackThenFront();
    AudioMetadata offsetMeta;
    QVERIFY(parseFlac(ByteSource(prefixed), 100, offsetMeta, kAll));
    QCOMPARE(offsetMeta.cover.data, redPng());
    QVERIFY(!parseFlac(ByteSource(prefixed), 0, offsetMeta, kAll));
}

void Test_Audiometa_B::flacStopsAtLastBlock() {
    // Anything after the block flagged last is audio, however much it looks like a block.
    const QByteArray afterLast =
        "fLaC" + flacBlock(0, streamInfo(), true) + flacBlock(4, vorbisComment({"TITLE=Too late"}));
    AudioMetadata meta = parse(Parser::Flac, afterLast);
    QVERIFY(meta.isEmpty());

    // Type 127 is invalid: the walk stops there.
    const QByteArray invalid = "fLaC" + flacBlock(0, streamInfo()) + flacBlock(127, "zzzz") +
                               flacBlock(4, vorbisComment({"TITLE=Too late"}), true);
    meta = parse(Parser::Flac, invalid);
    QVERIFY(meta.isEmpty());

    // A picture whose data is a URL is not a picture.
    const QByteArray url = flacFile(flacBlock(6, flacPicture(3, "-->", {}, "https://example.com/cover.png"), true));
    meta = parse(Parser::Flac, url);
    QVERIFY(meta.cover.isNull());
}

void Test_Audiometa_B::flacTruncatedComment() {
    // A partly downloaded file: the comment block runs past the end, and the entries that made it count.
    const QByteArray full =
        flacFile(flacBlock(4, vorbisComment({"TITLE=Whole", "ARTIST=Also whole", "ALBUM=Cut off here"}), true));
    const QByteArray cut = full.left(full.indexOf("Cut off") + 3);
    const AudioMetadata meta = parse(Parser::Flac, cut);
    QCOMPARE(meta.title, QStringLiteral("Whole"));
    QCOMPARE(meta.artist, QStringLiteral("Also whole"));
    QVERIFY(meta.album.isEmpty());
}

void Test_Audiometa_B::flacPictureLengthsAreChecked() {
    const QByteArray good = flacPicture(3, "image/png", "d", redPng());
    AudioMetadata meta = parse(Parser::FlacPicture, good);
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.description, QStringLiteral("d"));

    // Each length in turn made to claim more than there is.
    const qsizetype mimeLengthAt = 4;
    const qsizetype descriptionLengthAt = 8 + 9;
    const qsizetype dataLengthAt = descriptionLengthAt + 4 + 1 + 16;
    for(qsizetype at : {mimeLengthAt, descriptionLengthAt, dataLengthAt}) {
        for(quint32 lie : {quint32(good.size()), 0x7FFFFFFFu, 0xFFFFFFFFu}) {
            QByteArray bad = good;
            bad.replace(at, 4, u32be(lie));
            meta = parse(Parser::FlacPicture, bad);
            QVERIFY2(meta.cover.isNull(), qPrintable(QStringLiteral("length at %1 = %2").arg(at).arg(lie)));
        }
    }
    // Without Cover in the mask nothing is taken.
    QVERIFY(parse(Parser::FlacPicture, good, kTags).cover.isNull());
}

// ---------------------------------------------------------------------------------------------------------
// Vorbis comments
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::vorbisRepeatedKeysAndTotals() {
    const AudioMetadata meta =
        parse(Parser::VorbisComment, vorbisComment({"ARTIST=a", "title=Mixed case", "Artist=b", "ARTIST=a",
                                                    "TRACKNUMBER=3", "TRACKTOTAL=12", "DISCNUMBER=1", "TOTALDISCS=2"}));
    QCOMPARE(meta.artist, QStringLiteral("a; b"));
    QCOMPARE(meta.title, QStringLiteral("Mixed case"));
    QCOMPARE(meta.track, QStringLiteral("3/12"));
    QCOMPARE(meta.disc, QStringLiteral("1/2"));
    QCOMPARE(meta.tagTypes, QStringList{QStringLiteral("Vorbis comment")});
    // The totals went into the numbers, not into fields of their own.
    QVERIFY(fieldValue(meta, QStringLiteral("TRACKTOTAL")).isEmpty());

    // A number that carries its total already keeps it.
    AudioMetadata both = parse(Parser::VorbisComment, vorbisComment({"TRACKNUMBER=3/12", "TOTALTRACKS=12"}));
    QCOMPARE(both.track, QStringLiteral("3/12"));
    // A total alone is just a field.
    AudioMetadata totalOnly = parse(Parser::VorbisComment, vorbisComment({"DISCTOTAL=4"}));
    QVERIFY(totalOnly.disc.isEmpty());
    QCOMPARE(fieldValue(totalOnly, QStringLiteral("DISCTOTAL")), QStringLiteral("4"));
}

void Test_Audiometa_B::vorbisAliasesAndUnknownKeys() {
    const AudioMetadata meta =
        parse(Parser::VorbisComment,
              vorbisComment({"ALBUM ARTIST=Spaced", "YEAR=1999", "COMMENT=Said", "DESCRIPTION=Described", "GENRE=Jazz",
                             "COMPOSER=Bach", "replaygain_track_gain=-6.50 dB", "NOEQUALS", "=no key",
                             "EMPTY=", "ALBUMARTIST=Joined", "TITLE=Unicode \xC3\xBC"}));
    // ALBUMARTIST is the first choice even when it comes later.
    QCOMPARE(meta.albumArtist, QStringLiteral("Joined"));
    QCOMPARE(fieldValue(meta, QStringLiteral("ALBUM ARTIST")), QStringLiteral("Spaced"));
    QCOMPARE(meta.date, QStringLiteral("1999"));
    QCOMPARE(meta.comment, QStringLiteral("Said"));
    QCOMPARE(fieldValue(meta, QStringLiteral("DESCRIPTION")), QStringLiteral("Described"));
    QCOMPARE(meta.genre, QStringLiteral("Jazz"));
    QCOMPARE(meta.composer, QStringLiteral("Bach"));
    QCOMPARE(meta.title, QStringLiteral("Unicode \u00FC"));
    QCOMPARE(fieldValue(meta, QStringLiteral("replaygain_track_gain")), QStringLiteral("-6.50 dB"));
    QVERIFY(fieldValue(meta, QStringLiteral("EMPTY")).isEmpty());
    for(AudioField const &field : meta.fields)
        QVERIFY(!field.key.isEmpty() && !field.value.isEmpty());

    // Well-known fields first, the rest after them in the order they came.
    QCOMPARE(meta.fields.size(), qsizetype(9));
    QCOMPARE(meta.fields.at(0).key, fieldName(Field::Title));
}

void Test_Audiometa_B::vorbisPictures() {
    const QByteArray front = flacPicture(3, "image/png", "front", redPng());
    // Line-wrapped base64, as some taggers write it.
    QByteArray wrapped = front.toBase64();
    for(qsizetype at = 76; at < wrapped.size(); at += 78)
        wrapped.insert(at, "\r\n");
    AudioMetadata meta =
        parse(Parser::VorbisComment, vorbisComment({"COVERART=" + QByteArray("legacy bytes").toBase64(),
                                                    "COVERARTMIME=image/jpeg", "METADATA_BLOCK_PICTURE=" + wrapped}));
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.type, 3);
    // Pictures are never listed as text.
    QVERIFY(meta.fields.isEmpty());

    // The legacy form alone: the bare image, its type from COVERARTMIME.
    meta = parse(Parser::VorbisComment, vorbisComment({"COVERARTMIME=image/png", "COVERART=" + redPng().toBase64()}));
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.type, -1);
    QCOMPARE(meta.cover.mimeType, QStringLiteral("image/png"));

    // Not base64: no picture rather than a garbage one.
    meta = parse(Parser::VorbisComment, vorbisComment({"METADATA_BLOCK_PICTURE=***not base64***"}));
    QVERIFY(meta.cover.isNull());

    // Without Cover in the mask the picture is not decoded; without Tags the text is not read.
    meta = parse(Parser::VorbisComment, vorbisComment({"TITLE=t", metadataBlockPicture(front)}), kTags);
    QVERIFY(meta.cover.isNull());
    QCOMPARE(meta.title, QStringLiteral("t"));
    meta = parse(Parser::VorbisComment, vorbisComment({"TITLE=t", metadataBlockPicture(front)}), kCover);
    QCOMPARE(meta.cover.data, redPng());
    QVERIFY(meta.fields.isEmpty());
}

void Test_Audiometa_B::vorbisLyingCounts() {
    // A count of four billion over two entries: the data runs out first.
    QByteArray huge = vorbisComment({"TITLE=One", "ARTIST=Two"});
    huge.replace(4 + 10, 4, u32le(0xFFFFFFFFu));
    QElapsedTimer timer;
    timer.start();
    AudioMetadata meta = parse(Parser::VorbisComment, huge);
    QVERIFY(timer.elapsed() < 1000);
    QCOMPARE(meta.title, QStringLiteral("One"));
    QCOMPARE(meta.artist, QStringLiteral("Two"));

    // An entry claiming more than there is ends the walk; what came before stays.
    QByteArray lying = vorbisComment({"TITLE=One", "ARTIST=Two"});
    lying.replace(4 + 10 + 4 + 4 + 9, 4, u32le(0x7FFFFFFF));
    meta = parse(Parser::VorbisComment, lying);
    QCOMPARE(meta.title, QStringLiteral("One"));
    QVERIFY(meta.artist.isEmpty());

    // So does a vendor string longer than the comment: nothing is recognised.
    QByteArray vendor = vorbisComment({"TITLE=One"});
    vendor.replace(0, 4, u32le(1000));
    meta = parse(Parser::VorbisComment, vendor);
    QVERIFY(meta.isEmpty());
    QVERIFY(meta.tagTypes.isEmpty());
}

// ---------------------------------------------------------------------------------------------------------
// Ogg
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::oggCommentSpanningPages() {
    // The layout the test is about: the comment fills pages 1 and 2 with 255s and ends with the last
    // value of page 3; the setup packet then starts page 4, which does not continue anything.
    const QList<QByteArray> pages = oggVorbisSpanning();
    QCOMPARE(pages.size(), qsizetype(5));
    QVERIFY(!pageContinues(pages.at(1)) && pageContinues(pages.at(2)) && pageContinues(pages.at(3)));
    QCOMPARE(pageLacing(pages.at(1)), QByteArray(7, '\xFF'));
    QCOMPARE(quint8(pageLacing(pages.at(3)).back()), quint8(100));
    QVERIFY(!pageContinues(pages.at(4)));

    const AudioMetadata meta = parse(Parser::Ogg, joinPages(pages));
    QCOMPARE(meta.title, QStringLiteral("Spanning"));
    QCOMPARE(meta.artist, QStringLiteral("qimgv"));
    QCOMPARE(meta.album, QStringLiteral("Pages"));
    QCOMPARE(meta.cover.data, redPng());
    QVERIFY(fieldValue(meta, QStringLiteral("PADDING")).endsWith(QStringLiteral("ppp")));
}

void Test_Audiometa_B::oggPacketEndingInZeroLacing() {
    // Twelve 255s over pages 1 and 2, and the 0 that ends the packet opens page 3.
    const QList<QByteArray> pages = oggOpusZeroTerminated();
    QCOMPARE(pages.size(), qsizetype(4));
    QVERIFY(pageContinues(pages.at(3)));
    QCOMPARE(pageLacing(pages.at(3)).front(), '\0');

    const AudioMetadata meta = parse(Parser::Ogg, joinPages(pages));
    QCOMPARE(meta.title, QStringLiteral("Zero"));
    QCOMPARE(meta.artist, QStringLiteral("Opus"));
}

void Test_Audiometa_B::oggMultiplexed() {
    bool recognised = false;
    const AudioMetadata meta = parse(Parser::Ogg, buildOggMultiplexed(), kAll, &recognised);
    QVERIFY(recognised);
    QCOMPARE(meta.title, QStringLiteral("Audio title"));
    QCOMPARE(meta.artist, QStringLiteral("Muxed"));
}

void Test_Audiometa_B::oggFlacPicture_data() {
    QTest::addColumn<int>("headerPackets");
    QTest::newRow("count given") << 2;
    // 0: the encoder did not know; the last-block flag ends the headers.
    QTest::newRow("count unknown") << 0;
}

void Test_Audiometa_B::oggFlacPicture() {
    QFETCH(int, headerPackets);
    const QByteArray file = oggFlacWithPicture(quint16(headerPackets));
    AudioMetadata meta = parse(Parser::Ogg, file);
    QCOMPARE(meta.title, QStringLiteral("Ogg FLAC"));
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.description, QStringLiteral("front"));

    meta = parse(Parser::Ogg, file, kTags);
    QCOMPARE(meta.title, QStringLiteral("Ogg FLAC"));
    QVERIFY(meta.cover.isNull());
}

void Test_Audiometa_B::oggWithoutAudio() {
    // Theora alone, and a stream of a codec we do not read.
    const QByteArray unknown =
        joinPages(oggStream(5, {QByteArray("\x80kate\0\0\0", 8) + QByteArray(40, 'k'), "x"}, 255));
    for(QByteArray const &file : {readFile(QStringLiteral("theora-video.ogg")), unknown}) {
        bool recognised = true;
        const AudioMetadata meta = parse(Parser::Ogg, file, kAll, &recognised);
        QVERIFY(!recognised);
        QVERIFY(meta.isEmpty());
    }
}

// A cover of a few MB in METADATA_BLOCK_PICTURE, split into pages well under 1 KiB: thousands of pages,
// more than kMaxStructures, and all of them must be read.
void Test_Audiometa_B::oggLargeCommentAcrossThousandsOfPages() {
    QByteArray picture(3 * 1024 * 1024 + 512 * 1024, Qt::Uninitialized);
    QRandomGenerator rng(2026);
    for(char &c : picture)
        c = char(rng.bounded(256));
    const QByteArray comment =
        QByteArray("\x03vorbis", 7) +
        vorbisComment({"TITLE=Large", metadataBlockPicture(flacPicture(3, "image/png", {}, picture))}) + char(1);
    const QList<QByteArray> pages = oggStream(3, {vorbisIdentification(), comment, "setup"}, 3);
    QVERIFY(pages.size() > kMaxStructures);

    QElapsedTimer timer;
    timer.start();
    const AudioMetadata meta = parse(Parser::Ogg, joinPages(pages));
    qInfo() << pages.size() << "pages read in" << timer.elapsed() << "ms";
    QCOMPARE(meta.title, QStringLiteral("Large"));
    QVERIFY(meta.cover.data == picture);
}

// Pages that deliver no bytes cannot keep a parse going: past kMaxStructures of them it gives up.
void Test_Audiometa_B::oggEmptyPagesAreBounded() {
    const QByteArray id = vorbisIdentification();
    const QByteArray comment = vorbisCommentPacket({"TITLE=Never"}, 100);
    QByteArray file = oggPage(0x02, 9, 0, lacingFor(id), id);
    quint32 sequence = 1;
    for(int i = 0; i < kMaxStructures + 10; i++)
        file += oggPage(0, 9, sequence++, {}, {});
    file += oggPage(0x04, 9, sequence++, lacingFor(comment), comment);
    QElapsedTimer timer;
    timer.start();
    bool recognised = false;
    const AudioMetadata meta = parse(Parser::Ogg, file, kAll, &recognised);
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(recognised);
    QVERIFY(meta.title.isEmpty());

    // The same comment after a handful of empty pages is read.
    const QByteArray few = oggPage(0x02, 9, 0, lacingFor(id), id) + oggPage(0, 9, 1, {}, {}) +
                           oggPage(0, 9, 2, {}, {}) + oggPage(0x04, 9, 3, lacingFor(comment), comment);
    QCOMPARE(parse(Parser::Ogg, few).title, QStringLiteral("Never"));
}

// ---------------------------------------------------------------------------------------------------------
// MP4
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::mp4MoovAfterLargeMdat() {
    bool recognised = false;
    const AudioMetadata meta = parse(Parser::Mp4, buildMp4LargeMdat(), kAll, &recognised);
    QVERIFY(recognised);
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    QCOMPARE(meta.artist, QStringLiteral("qimgv"));
    QCOMPARE(meta.albumArtist, QStringLiteral("Various"));
    QCOMPARE(meta.track, QStringLiteral("3/12"));
    QCOMPARE(meta.tagTypes, QStringList{QStringLiteral("MP4")});

    // moov of size 0: it runs to the end of the file.
    QByteArray sizeZero = buildMp4LargeMdat();
    const qsizetype moov = sizeZero.indexOf("moov") - 4;
    sizeZero.replace(moov, 4, u32be(0));
    QCOMPARE(parse(Parser::Mp4, sizeZero).title, QStringLiteral("Fixture"));
}

void Test_Audiometa_B::mp4LargeMdatPastTheEnd() {
    // An mdat that says it is 5 GiB, in a file of a few hundred bytes: the walk stops, nothing is read.
    QByteArray file = buildMp4LargeMdat();
    const qsizetype mdat = file.indexOf("mdat");
    file.replace(mdat + 4, 8, u64be(5ull * 1024 * 1024 * 1024));
    bool recognised = true;
    const AudioMetadata meta = parse(Parser::Mp4, file, kAll, &recognised);
    QVERIFY(!recognised);
    QVERIFY(meta.isEmpty());
}

void Test_Audiometa_B::mp4ChildInsideParent() {
    // The last item says it is larger than what is left of ilst. Believing it would read on into the
    // box after ilst, which here holds a data box of its own.
    const QByteArray artist = textItem("\251ART", "qimgv");
    QByteArray title = textItem("\251nam", "Fixture");
    const QByteArray after = box("free", dataBox(1, "Smuggled"));
    title.replace(0, 4, u32be(quint32(title.size() + after.size())));
    const QByteArray file =
        mp4File(box("udta", fullBox("meta", hdlr("mdir") + box("ilst", artist + title) + after)), box("mdat", "abc"));
    const AudioMetadata meta = parse(Parser::Mp4, file);
    QCOMPARE(meta.artist, QStringLiteral("qimgv"));
    QVERIFY(meta.title.isEmpty());
}

void Test_Audiometa_B::mp4QuickTimeMeta() {
    AudioMetadata meta = parse(Parser::Mp4, buildMp4QuickTimeMeta());
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    QCOMPARE(meta.genre, QStringLiteral("Ambient"));
    QCOMPARE(meta.track, QStringLiteral("3/12"));

    // An ISO meta with no handler box: the zero version and flags give it away.
    const QByteArray noHandler =
        mp4File(box("udta", fullBox("meta", box("ilst", standardItems()))), box("mdat", "abc"));
    meta = parse(Parser::Mp4, noHandler);
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    // An ISO meta with flags set that should not be: the handler box still shows where the children are.
    const QByteArray flagged =
        mp4File(box("udta", box("meta", u32be(1) + hdlr("mdir") + box("ilst", standardItems()))), box("mdat", "abc"));
    meta = parse(Parser::Mp4, flagged);
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    // And a meta straight inside moov.
    const QByteArray inMoov = mp4File(fullBox("meta", hdlr("mdir") + box("ilst", standardItems())), box("mdat", "abc"));
    meta = parse(Parser::Mp4, inMoov);
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
}

void Test_Audiometa_B::mp4Items() {
    const AudioMetadata meta = parse(Parser::Mp4, buildMp4Items());
    QCOMPARE(meta.title, QStringLiteral("Items"));
    QCOMPARE(meta.artist, QStringLiteral("First; Second"));
    // gnre holds the ID3v1 index plus one: 18 is Rock.
    QCOMPARE(meta.genre, QStringLiteral("Rock"));
    QCOMPARE(meta.disc, QStringLiteral("1/2"));
    QCOMPARE(meta.track, QStringLiteral("7"));
    QCOMPARE(fieldValue(meta, QStringLiteral("REPLAYGAIN_TRACK_GAIN")), QStringLiteral("-6.50 dB"));
    QVERIFY(fieldValue(meta, QStringLiteral("iTunNORM")).isEmpty());
    QCOMPARE(fieldValue(meta, QStringLiteral("BPM")), QStringLiteral("120"));
    QCOMPARE(fieldValue(meta, QStringLiteral("Encoder")), QStringLiteral("Lavf"));
    QCOMPARE(fieldValue(meta, QString::fromLatin1("\251xyz")), QStringLiteral("Unknown"));
    for(AudioField const &field : meta.fields) {
        QVERIFY2(field.value != QStringLiteral("keyed by index"), qPrintable(field.key));
        QVERIFY2(field.key != QStringLiteral("cpil"), "integer flags are not listed");
    }
    // Two pictures in covr: the first is the front cover.
    QCOMPARE(meta.cover.data, QByteArray("first, front"));
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.mimeType, QStringLiteral("image/jpeg"));
}

void Test_Audiometa_B::mp4PartsMask() {
    AudioMetadata meta = parse(Parser::Mp4, buildMp4Items(), kCover);
    QVERIFY(meta.fields.isEmpty());
    QCOMPARE(meta.cover.data, QByteArray("first, front"));
    meta = parse(Parser::Mp4, buildMp4Items(), kTags);
    QVERIFY(meta.cover.isNull());
    QCOMPARE(meta.title, QStringLiteral("Items"));
}

void Test_Audiometa_B::mp4TextAtoms() {
    AudioMetadata meta = parse(Parser::Mp4, buildMp4TextAtoms());
    QCOMPARE(meta.title, QStringLiteral("Fixture \u00FC"));
    // The first entry is empty; the next language's is used.
    QCOMPARE(meta.artist, QStringLiteral("qimgv"));
    QCOMPARE(meta.date, QStringLiteral("2026"));
    // Mac Roman under a Mac language code, but UTF-8 when that is what is there.
    QCOMPARE(meta.comment, QStringLiteral("caf\u00E9"));
    QCOMPARE(meta.album, QStringLiteral("Caf\u00E9"));
    QCOMPARE(meta.tagTypes, QStringList{QStringLiteral("QuickTime")});

    // With an iTunes list as well, the list is the tag and the atoms are not read.
    const QByteArray both = mp4File(box("udta", box("\251nam", quickTimeText("Atom", 0)) +
                                                    fullBox("meta", hdlr("mdir") + box("ilst", standardItems()))),
                                    box("mdat", "abc"));
    meta = parse(Parser::Mp4, both);
    QCOMPARE(meta.title, QStringLiteral("Fixture"));
    QCOMPARE(meta.tagTypes, QStringList{QStringLiteral("MP4")});
}

// ---------------------------------------------------------------------------------------------------------
// ASF
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_B::asfPictures() {
    AudioMetadata meta = parse(Parser::Asf, buildAsfPictures());
    QCOMPARE(meta.title, QStringLiteral("Pictures"));
    QCOMPARE(meta.album, QStringLiteral("Album"));
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.mimeType, QStringLiteral("image/png"));
    QCOMPARE(meta.cover.description, QStringLiteral("front"));
    QCOMPARE(meta.tagTypes, QStringList{QStringLiteral("ASF")});

    // The back cover alone still beats no cover.
    const QByteArray backOnly =
        asfFile({asfExtended({{"WM/Picture", 1, wmPicture(4, "image/jpeg", "back", "not really a jpeg")}})});
    meta = parse(Parser::Asf, backOnly);
    QCOMPARE(meta.cover.data, QByteArray("not really a jpeg"));
    QCOMPARE(meta.cover.type, 4);

    meta = parse(Parser::Asf, buildAsfPictures(), kCover);
    QVERIFY(meta.fields.isEmpty());
    QCOMPARE(meta.cover.data, redPng());
    meta = parse(Parser::Asf, buildAsfPictures(), kTags);
    QVERIFY(meta.cover.isNull());

    // A picture whose length runs past its value is dropped.
    QByteArray lying = wmPicture(3, "image/png", "front", redPng());
    lying.replace(1, 4, u32le(quint32(redPng().size() + 1)));
    meta = parse(Parser::Asf, asfFile({asfExtended({{"WM/Picture", 1, lying}})}));
    QVERIFY(meta.cover.isNull());
}

void Test_Audiometa_B::asfAttributes() {
    const AudioMetadata meta = parse(Parser::Asf, buildAsfAttributes());
    QCOMPARE(meta.title, QStringLiteral("Title"));
    QCOMPARE(meta.artist, QStringLiteral("Author"));
    QCOMPARE(meta.comment, QStringLiteral("A description"));
    QCOMPARE(fieldValue(meta, QStringLiteral("Copyright")), QStringLiteral("(c) 2026"));
    QCOMPARE(fieldValue(meta, QStringLiteral("Rating")), QStringLiteral("PG"));
    // WM/TrackNumber wins over the zero-based WM/Track.
    QCOMPARE(meta.track, QStringLiteral("5"));
    QCOMPARE(meta.date, QStringLiteral("1999"));
    QCOMPARE(meta.genre, QStringLiteral("Rock"));
    QCOMPARE(meta.disc, QStringLiteral("1/2"));
    // From the Metadata object in the Header Extension, both values.
    QCOMPARE(meta.composer, QStringLiteral("Composer; Another"));
    QCOMPARE(fieldValue(meta, QStringLiteral("WM/BeatsPerMinute")), QStringLiteral("128"));
    QCOMPARE(fieldValue(meta, QStringLiteral("WM/EncodingTime")), QStringLiteral("1234567890123"));
    QCOMPARE(fieldValue(meta, QStringLiteral("WM/Publisher")), QStringLiteral("Label"));
    // Booleans and GUIDs are not text.
    QVERIFY(fieldValue(meta, QStringLiteral("IsVBR")).isEmpty());
    QVERIFY(fieldValue(meta, QStringLiteral("WM/MediaClassPrimaryID")).isEmpty());

    // WM/Track alone counts from zero.
    const AudioMetadata legacy = parse(Parser::Asf, asfFile({asfExtended({{"WM/Track", 3, u32le(9)}})}));
    QCOMPARE(legacy.track, QStringLiteral("10"));
}

// ---------------------------------------------------------------------------------------------------------
// Bounds: shapes that once made a parse's work grow faster than the file -- a few MB that took minutes
// ---------------------------------------------------------------------------------------------------------

// The format allows one comment; a second is ignored. Thousands of them, each of thousands of entries, would
// otherwise add fields by the ten thousand.
void Test_Audiometa_B::flacAndOggFlacReadOneComment() {
    const AudioMetadata meta =
        parse(Parser::Flac, flacFile(flacBlock(4, vorbisComment({"TITLE=First"})) +
                                     flacBlock(4, vorbisComment({"TITLE=Second", "ARTIST=Second"}), true)));
    QCOMPARE(meta.title, QStringLiteral("First"));
    QVERIFY(meta.artist.isEmpty());

    const AudioMetadata ogg =
        parse(Parser::Ogg, joinPages(oggStream(7,
                                               {oggFlacMapping(0), flacBlock(4, vorbisComment({"TITLE=First"})),
                                                flacBlock(4, vorbisComment({"ARTIST=Second"})),
                                                flacBlock(6, flacPicture(3, "image/png", "front", redPng()), true),
                                                QByteArray("\xFF\xF8", 2) + QByteArray(30, 'a')},
                                               255)));
    QCOMPARE(ogg.title, QStringLiteral("First"));
    QVERIFY(ogg.artist.isEmpty());
    QCOMPARE(ogg.cover.data, redPng());

    QByteArray blocks;
    for(int block = 0; block < 4; block++) {
        QList<QByteArray> entries;
        for(int i = 0; i < 2048; i++)
            entries.append("K" + QByteArray::number(block * 2048 + i) + "=v");
        blocks += flacBlock(4, vorbisComment(entries));
    }
    QElapsedTimer timer;
    timer.start();
    const AudioMetadata many = parse(Parser::Flac, flacFile(blocks));
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(many.fields.size() <= kMaxStructures);
}

// 4096 artists of 4 KB that differ only at the end: grouped through a set rather than each checked against
// all before it.
void Test_Audiometa_B::vorbisManyValuesOfOneKey() {
    QList<QByteArray> entries;
    for(int i = 0; i < 4096; i++)
        entries.append("ARTIST=" + QByteArray(4000, 'a') + QByteArray::number(i));
    const QByteArray comment = vorbisComment(entries);
    QElapsedTimer timer;
    timer.start();
    const AudioMetadata meta = parse(Parser::VorbisComment, comment, kTags);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(meta.artist.count(QStringLiteral("; ")), qsizetype(4095));
}

// The same through MP4: 4000 data boxes of 3 KB in one item, and in one freeform item.
void Test_Audiometa_B::mp4ManyValuesOfOneItem() {
    QByteArray values;
    for(int i = 0; i < 4000; i++)
        values += dataBox(1, QByteArray(3000, 'v') + QByteArray::number(i));
    const QByteArray freeformHead = fullBox("mean", "com.apple.iTunes") + fullBox("name", "MANY");
    for(QByteArray const &item : {box("\251ART", values), box("----", freeformHead + values)}) {
        const QByteArray file =
            mp4File(box("udta", fullBox("meta", hdlr("mdir") + box("ilst", item))), box("mdat", "abc"));
        QElapsedTimer timer;
        timer.start();
        const AudioMetadata meta = parse(Parser::Mp4, file, kTags);
        QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(meta.fields.size(), qsizetype(1));
        QCOMPARE(meta.fields.first().value.count(QStringLiteral("; ")), qsizetype(3999));
    }
}

// A description object counts up to 65535 attributes, and a file may hold any number of these objects: the
// attributes come out of the parse's one budget like the objects do.
void Test_Audiometa_B::asfAttributesComeOutOfTheBudget() {
    QList<Attribute> attributes;
    for(int i = 0; i < 8000; i++)
        attributes.append({QStringLiteral("A%1").arg(i), 0, asfString(QStringLiteral("v"))});
    for(QByteArray const &file : {asfFile({asfExtended(attributes)}),
                                  asfFile({asfHeaderExtension(asfRecords(kAsfMetadataLibrary, attributes))})}) {
        QElapsedTimer timer;
        timer.start();
        const AudioMetadata meta = parse(Parser::Asf, file);
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(meta.fields.size() <= kMaxStructures);
        QCOMPARE(fieldValue(meta, QStringLiteral("A0")), QStringLiteral("v"));
    }
}

// 4000 authors of 8 KB that differ only at the end, through a set as in Vorbis comments.
void Test_Audiometa_B::asfManyValuesOfOneName() {
    QList<Attribute> attributes;
    for(int i = 0; i < 4000; i++)
        attributes.append({QStringLiteral("Author"), 0, asfString(QString(4000, u'a') + QString::number(i))});
    const QByteArray file = asfFile({asfExtended(attributes)});
    QElapsedTimer timer;
    timer.start();
    const AudioMetadata meta = parse(Parser::Asf, file, kTags);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(meta.artist.count(QStringLiteral("; ")), qsizetype(3999));
}

// ---------------------------------------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------------------------------------

QList<Sample> Test_Audiometa_B::samples() const {
    QList<Sample> list = {
        {"flac-picture.flac", Parser::Flac, readFile(QStringLiteral("flac-picture.flac"))},
        {"vorbis-picture.ogg", Parser::Ogg, readFile(QStringLiteral("vorbis-picture.ogg"))},
        {"opus-picture.opus", Parser::Ogg, readFile(QStringLiteral("opus-picture.opus"))},
        {"flac-in-ogg.oga", Parser::Ogg, readFile(QStringLiteral("flac-in-ogg.oga"))},
        {"speex.spx", Parser::Ogg, readFile(QStringLiteral("speex.spx"))},
        {"aac-cover.m4a", Parser::Mp4, readFile(QStringLiteral("aac-cover.m4a"))},
        {"alac.m4a", Parser::Mp4, readFile(QStringLiteral("alac.m4a"))},
        {"aac-isom.mp4", Parser::Mp4, readFile(QStringLiteral("aac-isom.mp4"))},
        {"wma.wma", Parser::Asf, readFile(QStringLiteral("wma.wma"))},
        {"flac back then front", Parser::Flac, flacBackThenFront()},
        {"flac picture block", Parser::FlacPicture, flacPicture(3, "image/png", "front", redPng())},
        {"vorbis comment", Parser::VorbisComment,
         vorbisComment({"ARTIST=a", "ARTIST=b", "TRACKNUMBER=3", "TRACKTOTAL=12",
                        metadataBlockPicture(flacPicture(3, "image/png", {}, redPng())), "COVERARTMIME=image/png",
                        "COVERART=" + redPng().toBase64()})},
        {"ogg spanning pages", Parser::Ogg, joinPages(oggVorbisSpanning())},
        {"ogg zero lacing", Parser::Ogg, joinPages(oggOpusZeroTerminated())},
        {"ogg multiplexed", Parser::Ogg, buildOggMultiplexed()},
        {"ogg flac picture", Parser::Ogg, oggFlacWithPicture(0)},
        {"mp4 large mdat", Parser::Mp4, buildMp4LargeMdat()},
        {"mp4 quicktime meta", Parser::Mp4, buildMp4QuickTimeMeta()},
        {"mp4 items", Parser::Mp4, buildMp4Items()},
        {"mp4 text atoms", Parser::Mp4, buildMp4TextAtoms()},
        {"asf pictures", Parser::Asf, buildAsfPictures()},
        {"asf attributes", Parser::Asf, buildAsfAttributes()},
    };
    return list;
}

// Every sample cut short at a spread of offsets, with each byte in turn set to 0x00 and 0xFF (which turns
// any length it is part of into nothing or into something huge), and with seeded random damage. Each
// parse must come back, with nothing out of bounds, and quickly: a parse that takes long on a few KB is a
// walk without progress or without a bound.
void Test_Audiometa_B::robustness() {
    constexpr qint64 kMaxParseNs = 250 * 1000 * 1000;
    constexpr int kRandomRounds = 300;
    const int partsCycle[] = {kAll, kTags, kCover};
    QRandomGenerator rng(0x51D0);
    qint64 parses = 0;
    qint64 slowest = 0;
    QString slowestCase;
    QElapsedTimer total;
    total.start();

    for(Sample const &sample : samples()) {
        QVERIFY2(!sample.bytes.isEmpty(), qPrintable(sample.name));
        auto check = [&](QByteArray const &bytes, char const *damage, qint64 detail) {
            const int parts = partsCycle[parses % 3];
            QElapsedTimer timer;
            timer.start();
            AudioMetadata meta;
            run(sample.parser, ByteSource(bytes), bytes, meta, parts);
            const qint64 elapsed = timer.nsecsElapsed();
            parses++;
            if(elapsed > slowest) {
                slowest = elapsed;
                slowestCase = QStringLiteral("%1, %2 %3").arg(sample.name, QLatin1String(damage)).arg(detail);
            }
            QVERIFY(meta.cover.data.size() <= kMaxPictureBytes);
            if(!(parts & kCover))
                QVERIFY(meta.cover.isNull());
            if(!(parts & kTags))
                QVERIFY(meta.fields.isEmpty());
        };

        const QByteArray &bytes = sample.bytes;
        const qsizetype size = bytes.size();
        const qsizetype step = size < 4096 ? 1 : size / 300;
        for(qsizetype cut = 0; cut < size; cut += step)
            check(bytes.left(cut), "cut at", cut);
        for(qsizetype at = 0; at < size; at += step) {
            for(char value : {'\x00', '\xFF'}) {
                QByteArray damaged = bytes;
                damaged[at] = value;
                check(damaged, value ? "0xFF at" : "0x00 at", at);
            }
        }
        for(int round = 0; round < kRandomRounds; round++) {
            QByteArray damaged = bytes;
            const int hits = 1 + int(rng.bounded(8));
            for(int i = 0; i < hits; i++)
                damaged[qsizetype(rng.bounded(quint32(size)))] = char(rng.bounded(256));
            check(damaged, "random round", round);
        }
        if(QTest::currentTestFailed())
            return;
    }
    qInfo().noquote() << parses << "parses in" << total.elapsed() << "ms; slowest" << slowest / 1000
                      << "us:" << slowestCase;
    QVERIFY2(slowest < kMaxParseNs, qPrintable(slowestCase));
}

QTEST_GUILESS_MAIN(Test_Audiometa_B)
#include "test_audiometa_b.moc"
