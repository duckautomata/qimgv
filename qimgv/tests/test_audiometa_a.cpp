// The ID3v2/ID3v1, APE, RIFF/AIFF/DSF and Matroska tag parsers. Real files from data/audio (made by
// make-fixtures.sh, which lists the tags they all carry) check the common case; hand-built byte sequences cover
// what ffmpeg cannot write and the corners real files rarely show: unsynchronisation, extended headers, iTunes'
// broken v2.4 sizes, APE before ID3v1, RF64, DSF, a SeekHead leading past the Clusters. Every sample is then
// truncated and corrupted, because these parsers read files from anywhere on worker threads.
#include <QtTest>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QRandomGenerator>
#include <functional>

#include "audiometa_p.h"

using namespace AudioMeta;
using namespace Qt::StringLiterals;

namespace {

constexpr int kAll = AudioMetadataReader::All;
constexpr int kTagsOnly = AudioMetadataReader::Tags;
constexpr int kCoverOnly = AudioMetadataReader::Cover;

QString dataPath(QString const &name) {
    return QStringLiteral(QIMGV_TEST_DATA_DIR "/audio/") + name;
}

QByteArray readFixture(QString const &name) {
    QFile file(dataPath(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString fieldValue(AudioMetadata const &meta, QString const &key) {
    for(AudioField const &field : meta.fields) {
        if(field.key == key)
            return field.value;
    }
    return {};
}

bool hasValue(AudioMetadata const &meta, QString const &value) {
    for(AudioField const &field : meta.fields) {
        if(field.value.contains(value))
            return true;
    }
    return false;
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

// What an ID3v2 writer does: a zero after every 0xFF that a sync word could start with, and after a final one.
QByteArray unsynchronise(QByteArray const &data) {
    QByteArray out;
    for(qsizetype i = 0; i < data.size(); i++) {
        out.append(data.at(i));
        const bool ff = quint8(data.at(i)) == 0xFF;
        if(ff && (i + 1 == data.size() || (quint8(data.at(i + 1)) & 0xE0) == 0xE0 || data.at(i + 1) == 0))
            out.append('\0');
    }
    return out;
}

QByteArray utf16(QString const &text, bool bigEndian) {
    QByteArray out;
    for(QChar c : text) {
        const quint16 u = c.unicode();
        out.append(bigEndian ? be(u, 2) : le(u, 2));
    }
    return out;
}

// An ID3v2 text frame body in encoding 0 (Latin-1), 1 (UTF-16 with a little-endian BOM), 2 (UTF-16BE) or 3.
QByteArray textData(QString const &text, int encoding) {
    switch(encoding) {
    case 0:
        return '\0' + text.toLatin1();
    case 1:
        return "\x01\xFF\xFE"_ba + utf16(text, false);
    case 2:
        return '\x02' + utf16(text, true);
    default:
        return '\x03' + text.toUtf8();
    }
}

QByteArray id3Tag(int version, QByteArray const &body, quint8 flags = 0) {
    return "ID3"_ba + char(version) + '\0' + char(flags) + syncsafe(quint32(body.size())) + body;
}

QByteArray frame22(QByteArray const &id, QByteArray const &data) {
    return id + be(quint64(data.size()), 3) + data;
}

QByteArray frame23(QByteArray const &id, QByteArray const &data, quint16 flags = 0) {
    return id + be(quint64(data.size()), 4) + be(flags, 2) + data;
}

QByteArray frame24(QByteArray const &id, QByteArray const &data, quint16 flags = 0) {
    return id + syncsafe(quint32(data.size())) + be(flags, 2) + data;
}

QByteArray apicData(quint8 type, QByteArray const &image, QByteArray const &mime = "image/png"_ba,
                    QByteArray const &description = {}) {
    return '\0' + mime + '\0' + char(type) + description + '\0' + image;
}

QByteArray id3v1(QByteArray const &title, QByteArray const &artist, QByteArray const &album, QByteArray const &year,
                 QByteArray const &comment, int track, int genre) {
    auto field = [](QByteArray value, int size) {
        value.truncate(size);
        return value + QByteArray(size - value.size(), '\0');
    };
    QByteArray tag = "TAG"_ba + field(title, 30) + field(artist, 30) + field(album, 30) + field(year, 4);
    if(track > 0)
        tag += field(comment, 28) + '\0' + char(track);
    else
        tag += field(comment, 30);
    return tag + char(genre);
}

QByteArray apeItem(QByteArray const &key, QByteArray const &value, quint32 flags = 0) {
    return le(quint64(value.size()), 4) + le(flags, 4) + key + '\0' + value;
}

QByteArray apeTag(QList<QByteArray> const &items, quint32 version = 2000, bool withHeader = true) {
    const QByteArray body = items.join();
    auto block = [&](quint32 flags) {
        return "APETAGEX"_ba + le(version, 4) + le(quint64(body.size() + 32), 4) + le(quint64(items.size()), 4) +
               le(flags, 4) + QByteArray(8, '\0');
    };
    if(!withHeader)
        return body + block(0);
    // Footer flags: "has a header"; the header adds "is the header".
    return block(0xA0000000) + body + block(0x80000000);
}

QByteArray riffChunk(QByteArray const &id, QByteArray const &data) {
    QByteArray out = id + le(quint64(data.size()), 4) + data;
    if(data.size() % 2)
        out += '\0';
    return out;
}

QByteArray aiffChunk(QByteArray const &id, QByteArray const &data) {
    QByteArray out = id + be(quint64(data.size()), 4) + data;
    if(data.size() % 2)
        out += '\0';
    return out;
}

// Matroska element IDs the samples use.
constexpr quint32 kEbml = 0x1A45DFA3, kEbmlVersion = 0x4286, kDocType = 0x4282, kSegment = 0x18538067;
constexpr quint32 kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC;
constexpr quint32 kInfo = 0x1549A966, kTitle = 0x7BA9, kCluster = 0x1F43B675, kTimecode = 0xE7;
constexpr quint32 kSimpleBlock = 0xA3, kTags = 0x1254C367, kTag = 0x7373, kTargets = 0x63C0;
constexpr quint32 kTargetTypeValue = 0x68CA, kTagAttachmentUid = 0x63C6, kSimpleTag = 0x67C8;
constexpr quint32 kTagName = 0x45A3, kTagString = 0x4487, kAttachments = 0x1941A469, kAttachedFile = 0x61A7;
constexpr quint32 kFileName = 0x466E, kFileMimeType = 0x4660, kFileData = 0x465C, kFileUid = 0x46AE;

QByteArray ebmlId(quint32 id) {
    QByteArray out;
    for(int shift = 24; shift >= 0; shift -= 8) {
        if(!out.isEmpty() || ((id >> shift) & 0xFF))
            out.append(char((id >> shift) & 0xFF));
    }
    return out;
}

QByteArray element(quint32 id, QByteArray const &data) {
    // One size byte when it fits, else the 8-byte form.
    const QByteArray size =
        data.size() < 0x7F ? QByteArray(1, char(0x80 | data.size())) : '\x01' + be(quint64(data.size()), 7);
    return ebmlId(id) + size + data;
}

QByteArray unknownSize(quint32 id) {
    return ebmlId(id) + "\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF"_ba;
}

QByteArray ebmlHeader(QByteArray const &docType = "matroska"_ba) {
    return element(kEbml, element(kEbmlVersion, "\x01"_ba) + element(kDocType, docType));
}

QByteArray simpleTag(QByteArray const &name, QByteArray const &value, QByteArray const &nested = {}) {
    return element(kSimpleTag, element(kTagName, name) + element(kTagString, value) + nested);
}

QByteArray tag(int level, QByteArray const &simpleTags, QByteArray const &extraTargets = {}) {
    return element(kTag,
                   element(kTargets, element(kTargetTypeValue, be(quint64(level), 1)) + extraTargets) + simpleTags);
}

QByteArray attachedFile(QByteArray const &name, QByteArray const &mime, QByteArray const &data, quint8 uid) {
    return element(kAttachedFile, element(kFileName, name) + element(kFileMimeType, mime) + element(kFileData, data) +
                                      element(kFileUid, be(uid, 1)));
}

// A fixed 4-byte position, so a SeekHead's size is known before the positions it holds.
QByteArray seekEntry(quint32 id, qint64 position) {
    return element(kSeek, element(kSeekId, ebmlId(id)) + element(kSeekPosition, be(quint64(position), 4)));
}

// Media of unknown size, as a live muxer writes it: nothing tells where it ends but the next top-level ID.
QByteArray fakeCluster() {
    return unknownSize(kCluster) + element(kTimecode, "\x00"_ba) + element(kSimpleBlock, QByteArray(40, '\x55'));
}

QByteArray redPng() {
    return readFixture(QStringLiteral("cover-red.png"));
}

// Not decodable, and does not need to be: parsers only carry the bytes.
QByteArray fakeImage(char fill, int size = 64) {
    return "\x89PNG\r\n\x1A\n"_ba + QByteArray(size, fill);
}

// ---------------------------------------------------------------------------------------------------------
// Hand-built samples, shared by the tests that check what they hold and by the robustness test.
// ---------------------------------------------------------------------------------------------------------

QByteArray sampleId3v22() {
    const QByteArray frames = frame22("TT2", textData(u"Two Two"_s, 0)) + frame22("TP1", textData(u"Singer"_s, 1)) +
                              frame22("TCO", textData(u"(17)"_s, 0)) + frame22("TS2", textData(u"Sorted"_s, 0)) +
                              frame22("PIC", "\0PNG\x03"_ba + "desc\0"_ba + redPng());
    return id3Tag(2, frames + QByteArray(16, '\0'));
}

// Bytes a JPEG would have, with every pattern unsynchronisation changes: FF E0, FF 00, FF FF and a final FF.
QByteArray syncPayload() {
    return "\xFF\xD8\xFF\xE0\x00\x10"
           "JFIF\x00\xFF\x00\xFF\xFF\xE1"
           "data\xFF\xD9\xFF"_ba;
}

QByteArray sampleId3v23Unsync() {
    // Extended header with a CRC full of 0xFF, so it changes length when unsynchronised: undoing the
    // unsynchronisation after reading it would land the frames in the wrong place.
    const QByteArray extended = be(10, 4) + be(0x8000, 2) + be(0, 4) + "\xFF\xFF\xFF\xE0"_ba;
    const QByteArray frames = frame23("TIT2", textData(u"Unsynced"_s, 0)) +
                              frame23("APIC", apicData(3, syncPayload(), "image/jpeg"_ba)) +
                              frame23("TPE1", textData(u"After"_s, 0));
    return id3Tag(3, unsynchronise(extended + frames + QByteArray(8, '\0')), 0x80 | 0x40);
}

QByteArray sampleId3v24FrameFlags() {
    const QByteArray picture = apicData(3, syncPayload(), "image/jpeg"_ba);
    const QByteArray frames = frame24("TIT2", textData(u"Per frame"_s, 3)) +
                              // unsynchronised, with a data length indicator
                              frame24("APIC", syncsafe(quint32(picture.size())) + unsynchronise(picture), 0x0003) +
                              // grouped: one group byte in front of the data
                              frame24("TPE1", '\x07' + textData(u"Grouped"_s, 3), 0x0040) +
                              // compressed and encrypted frames cannot be read and are skipped
                              frame24("TALB", syncsafe(100) + "\x78\x9C garbage"_ba, 0x0009) +
                              frame24("TCOM", '\x01' + textData(u"Encrypted"_s, 3), 0x0004) +
                              frame24("TCON", textData(u"Ambient"_s, 3));
    return id3Tag(4, frames + QByteArray(32, '\0'));
}

// A v2.4 extended header (syncsafe, counting itself) and the tag-wide unsynchronisation flag, which v2.4
// readers must apply to every frame even when the frame's own flag is clear.
QByteArray sampleId3v24TagUnsync() {
    const QByteArray extended = syncsafe(6) + "\x01\x00"_ba;
    const QByteArray frames = frame24("TIT2", textData(u"Tag-wide"_s, 3)) +
                              frame24("APIC", unsynchronise(apicData(3, syncPayload(), "image/jpeg"_ba)));
    return id3Tag(4, extended + frames + QByteArray(4, '\0'), 0x80 | 0x40);
}

QByteArray sampleId3v23FrameFlags() {
    const QByteArray frames = frame23("TALB", be(100, 4) + "\x78\x9C garbage"_ba, 0x0080) +
                              frame23("TCOM", '\x01' + textData(u"Encrypted"_s, 0), 0x0040) +
                              frame23("TPE1", '\x05' + textData(u"Grouped"_s, 0), 0x0020) +
                              frame23("TIT2", textData(u"Plain"_s, 0), 0xC000);
    return id3Tag(3, frames);
}

// iTunes' v2.4 frame sizes as plain integers. 201 has a high bit in its last byte, so it is not syncsafe at
// all; 383 is 00 00 01 7F, which reads as a syncsafe 255 and lands inside the title.
QByteArray sampleId3v24ITunes() {
    auto plainFrame = [](QByteArray const &id, QByteArray const &data) {
        return id + be(quint64(data.size()), 4) + "\0\0"_ba + data;
    };
    const QByteArray frames =
        plainFrame("TIT2", textData(QString(382, u'x'), 0)) + plainFrame("TPE1", textData(u"Artist"_s, 0)) +
        plainFrame("TALB", textData(QString(200, u'y'), 0)) + plainFrame("TCON", textData(u"Pop"_s, 0));
    return id3Tag(4, frames + QByteArray(10, '\0'));
}

QByteArray sampleId3v24Misc() {
    const QByteArray frames = frame24("TPE1", "\0A\0B\0"_ba) +
                              frame24("TXXX", "\x03REPLAYGAIN_TRACK_GAIN\0-6.50 dB"_ba) +
                              frame24("COMM", "\0eng"
                                              "iTunNORM\0 00000001 00000002"_ba) +
                              frame24("COMM", "\0eng"
                                              "Notes\0described"_ba) +
                              frame24("COMM", "\0eng\0main comment"_ba) + frame24("TSOP", textData(u"Sorted"_s, 0)) +
                              frame24("TLEN", textData(u"1234"_s, 0)) + frame24("TBPM", textData(u"120"_s, 0)) +
                              frame24("TZZZ", textData(u"Custom"_s, 0)) + frame24("PRIV", "owner\0private"_ba) +
                              frame24("TRCK", textData(u"3/12"_s, 0)) + frame24("TPOS", textData(u"1/2"_s, 0)) +
                              frame24("TCOM", textData(u"Composer"_s, 0)) +
                              frame24("TDRC", textData(u"2026-01-02"_s, 0)) + frame24("TPE2", textData(u"Band"_s, 0));
    return id3Tag(4, frames);
}

QByteArray sampleId3Pictures() {
    const QByteArray frames = frame23("APIC", apicData(0, fakeImage('o'), "image/png"_ba, "other"_ba)) +
                              frame23("APIC", apicData(3, redPng(), "image/png"_ba, "front"_ba)) +
                              frame23("APIC", apicData(3, fakeImage('s'), "image/png"_ba, "second front"_ba)) +
                              frame23("APIC", apicData(4, fakeImage('b'), "image/png"_ba, "back"_ba));
    return id3Tag(3, frames);
}

QByteArray sampleId3v1(QByteArray const &audio) {
    return audio + id3v1("Hand Title", "Hand Artist   ", "Hand Album", "1999", "A comment", 7, 26);
}

QByteArray sampleId3v1Plus(QByteArray const &audio) {
    const QByteArray plus = "TAG+"_ba + "continued"_ba.leftJustified(60, '\0') + QByteArray(60, '\0') +
                            QByteArray(60, '\0') + '\0' + "Synthwave"_ba.leftJustified(30, '\0') + QByteArray(12, '\0');
    return audio + plus + id3v1("123456789012345678901234567890", "Artist", "", "", "", 0, 255);
}

QByteArray sampleApe(QByteArray const &audio) {
    const QList<QByteArray> items = {
        apeItem("Title", "Ape Title"),
        apeItem("Artist", "Ape Artist"),
        apeItem("Album Artist", "Ape Album Artist"),
        apeItem("Year", "2001"),
        apeItem("Genre", "Rock\0Pop\0Rock"_ba),
        apeItem("Track", "4/10"),
        apeItem("REPLAYGAIN_ALBUM_GAIN", "-3.00 dB"),
        apeItem("Related", "https://example.org", 2 << 1),
        apeItem("Cover Art (Back)", "back.png\0"_ba + fakeImage('b'), 1 << 1),
        apeItem("Cover Art (Front)", "front.png\0"_ba + redPng(), 1 << 1),
        apeItem("Cover Art (Other)", fakeImage('o'), 1 << 1),
    };
    return audio + apeTag(items) + id3v1("V1 Title", "V1 Artist", "V1 Album", "1990", "V1 comment", 0, 17);
}

QByteArray sampleApeV1(QByteArray const &audio) {
    return audio + apeTag({apeItem("Title", "Old Title"), apeItem("Artist", "Old Artist")}, 1000, false);
}

QByteArray sampleApeLyrics3(QByteArray const &audio) {
    const QByteArray lyrics = "LYRICSBEGIN"_ba + "IND00002"
                                                 "10"_ba;
    return audio + apeTag({apeItem("TITLE", "Before Lyrics")}) + lyrics +
           QByteArray::number(lyrics.size()).rightJustified(6, '0') + "LYRICS200"_ba +
           id3v1("V1 Title", "", "", "", "", 0, 255);
}

QByteArray sampleDsf(quint64 pointerOverride = 0) {
    const QByteArray fmt = "fmt "_ba + le(52, 8) + QByteArray(40, '\0');
    const QByteArray data = "data"_ba + le(16, 8) + QByteArray(4, '\x69');
    const QByteArray id3 =
        id3Tag(4, frame24("TIT2", textData(u"DSD Title"_s, 3)) + frame24("APIC", apicData(3, redPng())));
    const quint64 pointer = pointerOverride ? pointerOverride : quint64(28 + fmt.size() + data.size());
    return "DSD "_ba + le(28, 8) + le(quint64(28 + fmt.size() + data.size() + id3.size()), 8) + le(pointer, 8) + fmt +
           data + id3;
}

QByteArray infoList(QList<QPair<QByteArray, QByteArray>> const &entries) {
    QByteArray body = "INFO"_ba;
    for(auto const &[id, text] : entries)
        body += riffChunk(id, text + '\0');
    return riffChunk("LIST", body);
}

QByteArray sampleRf64(QByteArray const &magic = "RF64"_ba) {
    const QByteArray audio(6, '\x11');
    const QByteArray ds64 = riffChunk("ds64", le(0, 8) + le(quint64(audio.size()), 8) + le(3, 8) + le(0, 4));
    const QByteArray fmt = riffChunk("fmt ", le(1, 2) + le(1, 2) + le(8000, 4) + le(16000, 4) + le(2, 2) + le(16, 2));
    const QByteArray data = "data"_ba + le(0xFFFFFFFF, 4) + audio;
    const QByteArray info = infoList({{"INAM", "RF Title"}, {"IART", "RF Artist"}});
    return magic + le(0xFFFFFFFF, 4) + "WAVE"_ba + ds64 + fmt + data + info;
}

// The ID3v2 chunk comes first and disagrees with INFO, which still wins; the ID3v2 tag fills what INFO lacks.
QByteArray sampleWavId3() {
    const QByteArray id3 =
        id3Tag(3, frame23("TIT2", textData(u"ID3 Title"_s, 0)) + frame23("TPE2", textData(u"ID3 Album Artist"_s, 0)) +
                      frame23("APIC", apicData(3, redPng())) + QByteArray(1, '\0'));
    const QByteArray fmt = riffChunk("fmt ", le(1, 2) + le(1, 2) + le(8000, 4) + le(16000, 4) + le(2, 2) + le(16, 2));
    const QByteArray info = infoList({{"INAM", "Info Title"},
                                      {"IART", "Bj\xC3\xB6rk"},
                                      {"ICMT", "\xE9t\xE9"},
                                      {"ITRK", "5"},
                                      {"IPRT", "6"},
                                      {"ISFT", "Encoder X"},
                                      {"IZZZ", "Unknown"}});
    const QByteArray body = "WAVE"_ba + fmt + riffChunk("id3 ", id3) + riffChunk("data", QByteArray(10, '\0')) + info;
    return "RIFF"_ba + le(quint64(body.size()), 4) + body;
}

QByteArray sampleAiff(QByteArray const &form = "AIFF"_ba) {
    const QByteArray comm =
        aiffChunk("COMM", be(1, 2) + be(4, 4) + be(16, 2) + "\x40\x0D\xAC\x44"_ba + QByteArray(6, '\0'));
    const QByteArray body = form + comm + aiffChunk("NAME", "Aiff Name") + aiffChunk("AUTH", "Aiff Author") +
                            aiffChunk("(c) ", "2026 Someone") + aiffChunk("ANNO", "First note") +
                            aiffChunk("ANNO", "Second note") + aiffChunk("SSND", QByteArray(16, '\0'));
    return "FORM"_ba + be(quint64(body.size()), 4) + body;
}

QByteArray sampleAiffWithId3() {
    const QByteArray id3 = id3Tag(4, frame24("TIT2", textData(u"ID3 Title"_s, 3)));
    const QByteArray body = "AIFF"_ba + aiffChunk("NAME", "Stale Name") + aiffChunk("AUTH", "Aiff Author") +
                            aiffChunk("SSND", QByteArray(8, '\0')) + aiffChunk("ID3 ", id3);
    return "FORM"_ba + be(quint64(body.size()), 4) + body;
}

QByteArray matroskaTags() {
    const QByteArray track = tag(30, simpleTag("TITLE", "Track Title") + simpleTag("ARTIST", "Track Artist") +
                                         simpleTag("PART_NUMBER", "5") +
                                         simpleTag("COMPOSER", "Comp", simpleTag("URL", "https://example.org")) +
                                         simpleTag("ARTIST", "Second Artist", simpleTag("SORT_WITH", "Artist, S")));
    const QByteArray album =
        tag(50, simpleTag("TITLE", "Album Title") + simpleTag("ARTIST", "Album Artist") +
                    simpleTag("TOTAL_PARTS", "9") + simpleTag("GENRE", "Jazz") + simpleTag("DATE_RELEASED", "2020") +
                    simpleTag("DATE_RECORDED", "2019") + simpleTag("CUSTOM", "Value"));
    const QByteArray elsewhere =
        tag(50, simpleTag("TITLE", "About an attachment"), element(kTagAttachmentUid, "\x02"_ba));
    return element(kTags, track + album + elsewhere);
}

QByteArray matroskaAttachments() {
    return element(kAttachments, attachedFile("notes.txt", "text/plain", "not a picture", 1) +
                                     attachedFile("small_cover.png", "image/png", fakeImage('s'), 2) +
                                     attachedFile("cover_land.png", "image/png", fakeImage('l'), 3) +
                                     attachedFile("cover.png", "image/png", redPng(), 4) +
                                     attachedFile("poster.jpg", "image/jpeg", fakeImage('p'), 5));
}

// SeekHead, Info, a Cluster of unknown size, then Tags and Attachments that only the SeekHead leads to.
QByteArray sampleMatroska() {
    const QByteArray info = element(kInfo, element(kTitle, "Info Title"));
    const QByteArray cluster = fakeCluster();
    const QByteArray tags = matroskaTags();
    const QByteArray attachments = matroskaAttachments();
    auto seekHead = [](qint64 info, qint64 tags, qint64 attachments) {
        return element(kSeekHead,
                       seekEntry(kInfo, info) + seekEntry(kTags, tags) + seekEntry(kAttachments, attachments));
    };
    const qint64 infoAt = seekHead(0, 0, 0).size();
    const qint64 tagsAt = infoAt + info.size() + cluster.size();
    const qint64 attachmentsAt = tagsAt + tags.size();
    return ebmlHeader() + unknownSize(kSegment) + seekHead(infoAt, tagsAt, attachmentsAt) + info + cluster + tags +
           attachments;
}

// The first SeekHead lists itself and a second one behind the Cluster, which lists the Tags.
QByteArray sampleMatroskaSecondSeekHead() {
    const QByteArray cluster = fakeCluster();
    const QByteArray tags = element(kTags, tag(50, simpleTag("ARTIST", "Found Late") + simpleTag("TITLE", "Album")));
    const qint64 firstSize = element(kSeekHead, seekEntry(kSeekHead, 0) + seekEntry(kSeekHead, 0)).size();
    const qint64 secondAt = firstSize + cluster.size();
    const QByteArray second = element(kSeekHead, seekEntry(kTags, 0));
    const QByteArray secondFixed = element(kSeekHead, seekEntry(kTags, secondAt + second.size()));
    const QByteArray first = element(kSeekHead, seekEntry(kSeekHead, 0) + seekEntry(kSeekHead, secondAt));
    return ebmlHeader("webm"_ba) + element(kSegment, first + cluster + secondFixed + tags);
}

// ---------------------------------------------------------------------------------------------------------
// Hostile input: sizes and counts chosen to make a careless parser allocate, overflow, loop or recurse.
// ---------------------------------------------------------------------------------------------------------

QList<QPair<QString, QByteArray>> hostileSamples() {
    QList<QPair<QString, QByteArray>> samples;
    samples.append({u"id3 frame size near 4G"_s,
                    "ID3\x03\0\0\0\0\0\x20"_ba + "TIT2\xFF\xFF\xFF\xF0\0\0"_ba + QByteArray(22, 'x')});
    samples.append({u"id3 tag size past EOF"_s, "ID3\x04\0\0\x7F\x7F\x7F\x7F"_ba + frame24("TIT2", "\0cut"_ba)});
    samples.append(
        {u"id3 extended header past EOF"_s, "ID3\x03\0\x40\0\0\0\x20"_ba + be(0x7FFFFFF0, 4) + QByteArray(28, '\0')});
    samples.append({u"id3 all FF"_s, "ID3\x03\0\x80\0\0\x10\0"_ba + QByteArray(2048, '\xFF')});
    samples.append({u"id3 thousands of empty frames"_s, id3Tag(3, [] {
                        QByteArray frames;
                        for(int i = 0; i < 9000; i++)
                            frames += frame23("TXXX", {});
                        return frames;
                    }())});
    samples.append({u"id3 text of NULs"_s, id3Tag(4, frame24("TPE1", QByteArray(20000, '\0')))});
    samples.append({u"ape item count huge"_s, QByteArray(64, 'a') + "APETAGEX"_ba + le(2000, 4) + le(64, 4) +
                                                  le(0xFFFFFFFF, 4) + le(0, 4) + QByteArray(8, '\0')});
    samples.append({u"ape tag size huge"_s, QByteArray(64, 'a') + "APETAGEX"_ba + le(2000, 4) + le(0xFFFFFFFF, 4) +
                                                le(1, 4) + le(0, 4) + QByteArray(8, '\0')});
    {
        QByteArray ape = apeTag({apeItem("Title", "x")});
        // Value size of the only item: far past the tag.
        ape.replace(32, 4, le(0xFFFFFFF0, 4));
        samples.append({u"ape value size huge"_s, ape});
    }
    samples.append({u"ape lyrics3 size past start"_s, "999999LYRICS200"_ba + id3v1("t", "", "", "", "", 0, 0)});
    samples.append({u"riff chunk size max"_s,
                    "RIFF\xFF\xFF\xFF\xFFWAVE"_ba + "LIST\xFF\xFF\xFF\xFFINFO"_ba + "INAM\xFF\xFF\xFF\x7F"_ba});
    samples.append({u"riff many empty chunks"_s, "RIFF\0\0\0\0WAVE"_ba + QByteArray("JUNK\0\0\0\0", 8).repeated(9000)});
    samples.append({u"rf64 ds64 size huge"_s, "RF64\xFF\xFF\xFF\xFFWAVE"_ba +
                                                  riffChunk("ds64", le(~0ull, 8) + le(~0ull, 8) + le(0, 8) + le(0, 4)) +
                                                  "data\xFF\xFF\xFF\xFF"_ba + QByteArray(8, '\0')});
    samples.append({u"aiff chunk size max"_s, "FORM\xFF\xFF\xFF\xFF"
                                              "AIFFNAME\xFF\xFF\xFF\xFFname"_ba});
    samples.append({u"dsf pointer huge"_s, "DSD "_ba + le(28, 8) + le(~0ull, 8) + le(~0ull, 8)});
    samples.append({u"dsf without metadata"_s, "DSD "_ba + le(28, 8) + le(28, 8) + le(0, 8)});
    {
        // A SeekHead whose 300 entries all point back at itself.
        QByteArray entries;
        for(int i = 0; i < 300; i++)
            entries += seekEntry(kSeekHead, 0);
        samples.append(
            {u"matroska seekhead loop"_s, ebmlHeader() + unknownSize(kSegment) + element(kSeekHead, entries)});
    }
    {
        QByteArray nested = simpleTag("DEEP", "value");
        for(int i = 0; i < 200; i++)
            nested = simpleTag("DEEP", "value", nested);
        samples.append(
            {u"matroska deep simpletags"_s, ebmlHeader() + element(kSegment, element(kTags, element(kTag, nested)))});
    }
    samples.append({u"matroska sizes near 2^56"_s, ebmlHeader() + unknownSize(kSegment) + ebmlId(kTags) +
                                                       "\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFE"_ba + QByteArray(16, '\0')});
    samples.append(
        {u"matroska seek position huge"_s,
         ebmlHeader() + unknownSize(kSegment) +
             element(kSeekHead, element(kSeek, element(kSeekId, ebmlId(kTags)) +
                                                   element(kSeekPosition, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"_ba)))});
    {
        QByteArray files;
        for(int i = 0; i < 3000; i++)
            files += attachedFile("f.bin", "application/octet-stream", "x", 1);
        samples.append(
            {u"matroska thousands of attachments"_s, ebmlHeader() + element(kSegment, element(kAttachments, files))});
    }
    samples.append({u"matroska attachment data past EOF"_s,
                    ebmlHeader() + unknownSize(kSegment) + ebmlId(kAttachments) + "\x1F\xFF\xFF\xF0"_ba +
                        ebmlId(kAttachedFile) + "\x1F\xFF\xFF\xF0"_ba + element(kFileName, "cover.jpg") +
                        ebmlId(kFileData) + "\x1F\xFF\xFF\xF0\xFF\xD8\xFF"_ba});
    samples.append({u"ebml header only"_s, ebmlHeader()});
    samples.append({u"empty"_s, QByteArray()});
    return samples;
}

// Every parser of this group on the same bytes. Each must stay inside them, whatever they are.
void parseAll(ByteSource const &src, AudioMetadata &meta) {
    AudioMeta::id3v2TagSize(src, 0);
    parseId3v2(src, 0, meta, kAll);
    parseApeTag(src, meta, kAll);
    parseId3v1(src, meta, kAll);
    parseRiffWave(src, meta, kAll);
    parseAiff(src, meta, kAll);
    parseDsf(src, meta, kAll);
    parseMatroska(src, meta, kAll);
}

} // namespace

class Test_Audiometa_A : public QObject {
    Q_OBJECT

private slots:
    void id3v2Fixtures_data();
    void id3v2Fixtures();
    void id3v1Fixture();
    void partsMask();
    void foreignFormatsRejected_data();
    void foreignFormatsRejected();
    void riffInfoFixtures_data();
    void riffInfoFixtures();
    void aiffFixture();
    void apeFixtures_data();
    void apeFixtures();
    void matroskaFixtures_data();
    void matroskaFixtures();

    void id3v22Picture();
    void id3v23UnsyncAndExtendedHeader();
    void id3v24FrameFlags();
    void id3v24TagUnsyncAndExtendedHeader();
    void id3v23FrameFlags();
    void id3v24ITunesSizes();
    void id3TextEncodings_data();
    void id3TextEncodings();
    void id3Genres_data();
    void id3Genres();
    void id3FieldsAndComments();
    void id3FrontCoverWins();
    void id3TruncatedTagKeepsFirstFrames();
    void id3v2TagSize_data();
    void id3v2TagSize();
    void id3v1HandBuilt();
    void id3v1Plus();
    void apeWithCoverBeforeId3v1();
    void apeV1();
    void apeBeforeLyrics3();
    void dsfMetadataPointer();
    void rf64DataSizeFromDs64_data();
    void rf64DataSizeFromDs64();
    void wavInfoWinsOverId3Chunk();
    void aiffTextChunks_data();
    void aiffTextChunks();
    void aiffId3WinsOverTextChunks();
    void matroskaSeekHeadPastCluster();
    void matroskaSecondSeekHead();
    void matroskaDocType();
    void hostileInputs_data();
    void hostileInputs();

    void apeTextValuesAreBounded();
    void id3GenreReferencesAreBounded();
    void riffOnlyFirstInfoListAndId3Chunk();
    void aiffTextIsBounded();
    void matroskaLongTagNamesIgnored();
    void matroskaManyValuesOfOneTag();

    void robustness();
};

// ---------------------------------------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::id3v2Fixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("tagType");
    QTest::addColumn<QString>("mimeType");
    QTest::newRow("v2.3, blue JPEG") << "mp3-id3v23.mp3" << "ID3v2.3" << "image/jpeg";
    QTest::newRow("v2.4, red PNG") << "mp3-id3v24.mp3" << "ID3v2.4" << "image/png";
}

void Test_Audiometa_A::id3v2Fixtures() {
    QFETCH(QString, file);
    QFETCH(QString, tagType);
    QFETCH(QString, mimeType);
    const QByteArray bytes = readFixture(file);
    QVERIFY2(!bytes.isEmpty(), qPrintable(dataPath(file)));

    ByteSource src(dataPath(file));
    AudioMetadata meta;
    const qint64 size = parseId3v2(src, 0, meta, kAll);
    QCOMPARE(size, AudioMeta::id3v2TagSize(src, 0));
    QVERIFY(size > 10 && size < bytes.size());
    // The MPEG audio starts right after the tag.
    QCOMPARE(quint8(bytes.at(size)), quint8(0xFF));

    QCOMPARE(meta.tagTypes, QStringList{tagType});
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.albumArtist, u"Various"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    QVERIFY(fieldValue(meta, u"Encoder"_s).startsWith(u"Lavf"_s));
    QCOMPARE(fieldValue(meta, u"Title"_s), u"Fixture"_s);

    // ffmpeg stores an attached picture as type 0, "other".
    QCOMPARE(meta.cover.type, 0);
    QCOMPARE(meta.cover.mimeType, mimeType);
    if(mimeType == u"image/png"_s) {
        QCOMPARE(meta.cover.data, redPng());
    } else {
        const qsizetype start = bytes.indexOf("\xFF\xD8\xFF"_ba);
        const qsizetype end = bytes.indexOf("\xFF\xD9"_ba, start);
        QVERIFY(start > 0 && end > start);
        QCOMPARE(meta.cover.data, bytes.mid(start, end + 2 - start));
        if(!QImageReader::supportedImageFormats().contains("jpeg"))
            QSKIP("no 'jpeg' image plugin to decode the cover with");
        const QImage image = QImage::fromData(meta.cover.data);
        QCOMPARE(image.size(), QSize(16, 16));
        const QRgb pixel = image.pixel(8, 8);
        QVERIFY2(qBlue(pixel) > 200 && qRed(pixel) < 40 && qGreen(pixel) < 40, "the JPEG cover is solid blue");
    }
}

void Test_Audiometa_A::id3v1Fixture() {
    const QByteArray bytes = readFixture(u"mp3-id3v1.mp3"_s);
    QVERIFY(!bytes.isEmpty());
    // ffmpeg 8 ignores -write_id3v1 together with -id3v2_version 0, which make-fixtures.sh asks for.
    if(!bytes.right(128).startsWith("TAG"))
        QSKIP("mp3-id3v1.mp3 has no ID3v1 tag; regenerate it with an ffmpeg that writes one");
    ByteSource src(dataPath(u"mp3-id3v1.mp3"_s));
    AudioMetadata meta;
    QVERIFY(parseId3v1(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"ID3v1"_s});
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    QCOMPARE(meta.track, u"3"_s);
}

void Test_Audiometa_A::partsMask() {
    ByteSource src(dataPath(u"mp3-id3v24.mp3"_s));
    AudioMetadata coverOnly;
    QVERIFY(parseId3v2(src, 0, coverOnly, kCoverOnly) > 0);
    QVERIFY(coverOnly.fields.isEmpty());
    QVERIFY(coverOnly.title.isEmpty());
    QCOMPARE(coverOnly.cover.data, redPng());

    AudioMetadata tagsOnly;
    QVERIFY(parseId3v2(src, 0, tagsOnly, kTagsOnly) > 0);
    QCOMPARE(tagsOnly.title, u"Fixture"_s);
    QVERIFY(tagsOnly.cover.isNull());

    const ByteSource ape(sampleApe(QByteArray(32, '\0')));
    AudioMetadata apeCover, apeTags;
    QVERIFY(parseApeTag(ape, apeCover, kCoverOnly));
    QVERIFY(apeCover.fields.isEmpty());
    QCOMPARE(apeCover.cover.data, redPng());
    QVERIFY(parseApeTag(ape, apeTags, kTagsOnly));
    QCOMPARE(apeTags.title, u"Ape Title"_s);
    QVERIFY(apeTags.cover.isNull());

    const ByteSource mkv(sampleMatroska());
    AudioMetadata mkvCover, mkvTags;
    QVERIFY(parseMatroska(mkv, mkvCover, kCoverOnly));
    QVERIFY(mkvCover.fields.isEmpty());
    QCOMPARE(mkvCover.cover.data, redPng());
    QVERIFY(parseMatroska(mkv, mkvTags, kTagsOnly));
    QCOMPARE(mkvTags.title, u"Track Title"_s);
    QVERIFY(mkvTags.cover.isNull());
}

void Test_Audiometa_A::foreignFormatsRejected_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("accepts");
    QTest::newRow("mp3 v2.3") << "mp3-id3v23.mp3" << "id3v2";
    QTest::newRow("mp3 v2.4") << "mp3-id3v24.mp3" << "id3v2";
    QTest::newRow("mp3 bare") << "mp3-bare.mp3" << "";
    QTest::newRow("wav") << "wav-info.wav" << "riff";
    QTest::newRow("aiff") << "aiff-id3.aiff" << "aiff";
    QTest::newRow("wavpack") << "wavpack-ape.wv" << "ape";
    QTest::newRow("tta") << "tta.tta" << "ape";
    QTest::newRow("mka") << "matroska-cover.mka" << "matroska";
    QTest::newRow("webm") << "opus-audio-only.webm" << "matroska";
    // Other groups' formats: nothing here may claim them.
    for(const char *other : {"flac-picture.flac", "vorbis-picture.ogg", "opus-picture.opus", "aac-cover.m4a", "wma.wma",
                             "apple.caf", "sun.au", "ac3.ac3", "speex.spx", "flac-in-ogg.oga"})
        QTest::newRow(other) << QString::fromLatin1(other) << "";
}

// A parser that does not recognise a file must say so and leave the metadata alone, or the reader's dispatch
// would stack one format's garbage on top of another's tags.
void Test_Audiometa_A::foreignFormatsRejected() {
    QFETCH(QString, file);
    QFETCH(QString, accepts);
    QVERIFY2(QFile::exists(dataPath(file)), qPrintable(dataPath(file)));
    const ByteSource src(dataPath(file));
    const QList<QPair<QString, std::function<bool(AudioMetadata &)>>> parsers = {
        {u"id3v2"_s, [&](AudioMetadata &m) { return parseId3v2(src, 0, m, kAll) > 0; }},
        {u"id3v1"_s, [&](AudioMetadata &m) { return parseId3v1(src, m, kAll); }},
        {u"ape"_s, [&](AudioMetadata &m) { return parseApeTag(src, m, kAll); }},
        {u"riff"_s, [&](AudioMetadata &m) { return parseRiffWave(src, m, kAll); }},
        {u"aiff"_s, [&](AudioMetadata &m) { return parseAiff(src, m, kAll); }},
        {u"dsf"_s, [&](AudioMetadata &m) { return parseDsf(src, m, kAll); }},
        {u"matroska"_s, [&](AudioMetadata &m) { return parseMatroska(src, m, kAll); }},
    };
    for(auto const &[name, parse] : parsers) {
        AudioMetadata meta;
        const bool accepted = parse(meta);
        QVERIFY2(accepted == (name == accepts), qPrintable(name));
        if(!accepted)
            QVERIFY2(meta.isEmpty() && meta.tagTypes.isEmpty(), qPrintable(name));
    }
}

void Test_Audiometa_A::riffInfoFixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::newRow("INFO") << "wav-info.wav";
    QTest::newRow("INFO and id3 chunk") << "wav-id3.wav";
}

void Test_Audiometa_A::riffInfoFixtures() {
    QFETCH(QString, file);
    ByteSource src(dataPath(file));
    AudioMetadata meta;
    QVERIFY(parseRiffWave(src, meta, kAll));
    QCOMPARE(meta.tagTypes.value(0), u"RIFF INFO"_s);
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    QVERIFY(fieldValue(meta, u"Encoder"_s).startsWith(u"Lavf"_s));
    if(file != u"wav-id3.wav"_s)
        return;
    // The wav muxer has no -write_id3v2 option; ffmpeg 8 drops it without a word, which make-fixtures.sh
    // relies on. The hand-built sample in wavInfoWinsOverId3Chunk() covers the id3 chunk meanwhile.
    if(!readFixture(file).contains("id3 ") && !readFixture(file).contains("ID3 "))
        QSKIP("wav-id3.wav has no id3 chunk; regenerate it with a writer that adds one");
    QVERIFY(meta.tagTypes.contains(u"ID3v2.3"_s) || meta.tagTypes.contains(u"ID3v2.4"_s));
    QCOMPARE(meta.albumArtist, u"Various"_s);
}

void Test_Audiometa_A::aiffFixture() {
    ByteSource src(dataPath(u"aiff-id3.aiff"_s));
    AudioMetadata meta;
    QVERIFY(parseAiff(src, meta, kAll));
    QCOMPARE(meta.tagTypes, (QStringList{u"ID3v2.4"_s, u"AIFF"_s}));
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.albumArtist, u"Various"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.mimeType, u"image/png"_s);
}

void Test_Audiometa_A::apeFixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::newRow("WavPack") << "wavpack-ape.wv";
    QTest::newRow("TTA") << "tta.tta";
}

// ffmpeg writes APEv2 with lower-case keys, "album_artist" and "date" among them.
void Test_Audiometa_A::apeFixtures() {
    QFETCH(QString, file);
    ByteSource src(dataPath(file));
    AudioMetadata meta;
    QVERIFY(parseApeTag(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"APEv2"_s});
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.albumArtist, u"Various"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    QVERIFY(fieldValue(meta, u"encoder"_s).startsWith(u"Lavf"_s));
    QVERIFY(meta.cover.isNull());

    AudioMetadata none;
    QCOMPARE(parseId3v2(src, 0, none, kAll), 0);
    QVERIFY(!parseId3v1(src, none, kAll));
    QVERIFY(none.isEmpty());
}

void Test_Audiometa_A::matroskaFixtures_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("hasCover");
    QTest::newRow("mka, cover.png attached") << "matroska-cover.mka" << true;
    QTest::newRow("audio-only webm") << "opus-audio-only.webm" << false;
}

// ffmpeg puts the title in Segment Info, everything else in one Tag with empty Targets (level 50), and the
// track number as PART_NUMBER "3/12" at that level.
void Test_Audiometa_A::matroskaFixtures() {
    QFETCH(QString, file);
    QFETCH(bool, hasCover);
    ByteSource src(dataPath(file));
    AudioMetadata meta;
    QVERIFY(parseMatroska(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"Matroska"_s});
    QCOMPARE(meta.title, u"Fixture"_s);
    QCOMPARE(meta.artist, u"qimgv"_s);
    QCOMPARE(meta.album, u"Test Album"_s);
    QCOMPARE(meta.albumArtist, u"Various"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.date, u"2026"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);
    // The muxer's and the encoder's, from the global and the per-track tag.
    QVERIFY(fieldValue(meta, u"ENCODER"_s).startsWith(u"Lavf"_s));
    QVERIFY(fieldValue(meta, u"ENCODER"_s).contains(u"Lavc"_s));
    QVERIFY(fieldValue(meta, u"DURATION"_s).isEmpty());
    if(hasCover) {
        QCOMPARE(meta.cover.data, redPng());
        QCOMPARE(meta.cover.mimeType, u"image/png"_s);
        QCOMPARE(meta.cover.type, 3);
    } else {
        QVERIFY(meta.cover.isNull());
    }
}

// ---------------------------------------------------------------------------------------------------------
// ID3v2, hand-built
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::id3v22Picture() {
    const QByteArray bytes = sampleId3v22();
    const ByteSource src(bytes);
    AudioMetadata meta;
    QCOMPARE(parseId3v2(src, 0, meta, kAll), qint64(bytes.size()));
    QCOMPARE(meta.tagTypes, QStringList{u"ID3v2.2"_s});
    QCOMPARE(meta.title, u"Two Two"_s);
    QCOMPARE(meta.artist, u"Singer"_s);
    QCOMPARE(meta.genre, u"Rock"_s);
    QVERIFY(!hasValue(meta, u"Sorted"_s));
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.mimeType, u"image/png"_s);
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.description, u"desc"_s);
}

void Test_Audiometa_A::id3v23UnsyncAndExtendedHeader() {
    const QByteArray bytes = sampleId3v23Unsync();
    QVERIFY(bytes.contains("\xFF\x00"_ba)); // the sample really is unsynchronised
    const ByteSource src(bytes);
    AudioMetadata meta;
    QCOMPARE(parseId3v2(src, 0, meta, kAll), qint64(bytes.size()));
    QCOMPARE(meta.title, u"Unsynced"_s);
    QCOMPARE(meta.artist, u"After"_s);
    QCOMPARE(meta.cover.data, syncPayload());
    QCOMPARE(meta.cover.mimeType, u"image/jpeg"_s);
}

void Test_Audiometa_A::id3v24FrameFlags() {
    const ByteSource src(sampleId3v24FrameFlags());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.tagTypes, QStringList{u"ID3v2.4"_s});
    QCOMPARE(meta.title, u"Per frame"_s);
    QCOMPARE(meta.cover.data, syncPayload());
    QCOMPARE(meta.artist, u"Grouped"_s);
    QVERIFY(meta.album.isEmpty());
    QVERIFY(meta.composer.isEmpty());
    // Skipping a frame it cannot read does not lose the ones after it.
    QCOMPARE(meta.genre, u"Ambient"_s);
}

void Test_Audiometa_A::id3v24TagUnsyncAndExtendedHeader() {
    const ByteSource src(sampleId3v24TagUnsync());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.title, u"Tag-wide"_s);
    QCOMPARE(meta.cover.data, syncPayload());
}

void Test_Audiometa_A::id3v23FrameFlags() {
    const ByteSource src(sampleId3v23FrameFlags());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QVERIFY(meta.album.isEmpty());
    QVERIFY(meta.composer.isEmpty());
    QCOMPARE(meta.artist, u"Grouped"_s);
    // Status flags (tag/file alter preservation) change nothing about reading.
    QCOMPARE(meta.title, u"Plain"_s);
}

void Test_Audiometa_A::id3v24ITunesSizes() {
    const ByteSource src(sampleId3v24ITunes());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.title, QString(382, u'x'));
    QCOMPARE(meta.artist, u"Artist"_s);
    QCOMPARE(meta.album, QString(200, u'y'));
    QCOMPARE(meta.genre, u"Pop"_s);

    // The heuristic must not misread a correct tag whose plain reading would also be plausible: syncsafe 383
    // is 00 00 02 7F, which read plainly is 639 and runs past the end.
    const ByteSource correct(
        id3Tag(4, frame24("TIT2", textData(QString(382, u'z'), 0)) + frame24("TPE1", textData(u"Next"_s, 0))));
    AudioMetadata correctMeta;
    QVERIFY(parseId3v2(correct, 0, correctMeta, kAll) > 0);
    QCOMPARE(correctMeta.title, QString(382, u'z'));
    QCOMPARE(correctMeta.artist, u"Next"_s);
}

void Test_Audiometa_A::id3TextEncodings_data() {
    QTest::addColumn<int>("version");
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<QString>("expected");
    const QString text = u"Ünïcode ✓ 音楽"_s;
    QTest::newRow("UTF-16, little-endian BOM") << 3 << textData(text, 1) << text;
    QTest::newRow("UTF-16, big-endian BOM") << 3 << "\x01\xFE\xFF"_ba + utf16(text, true) << text;
    QTest::newRow("UTF-16, terminated") << 3 << textData(text, 1) + "\0\0"_ba << text;
    QTest::newRow("UTF-16BE") << 4 << textData(text, 2) << text;
    QTest::newRow("UTF-8") << 4 << textData(text, 3) << text;
    QTest::newRow("Latin-1") << 3 << textData(u"Ünïcode"_s, 0) << u"Ünïcode"_s;
    QTest::newRow("UTF-16, two values, BOM on each")
        << 4 << "\x01\xFE\xFF"_ba + utf16(u"A"_s, true) + "\0\0"_ba + "\xFF\xFE"_ba + utf16(u"B"_s, false) << u"A; B"_s;
    QTest::newRow("UTF-16, two values, BOM on the first only")
        << 4 << "\x01\xFE\xFF"_ba + utf16(u"A"_s, true) + "\0\0"_ba + utf16(u"B"_s, true) << u"A; B"_s;
    QTest::newRow("unknown encoding byte") << 4 << "\x07Text"_ba << QString();
}

void Test_Audiometa_A::id3TextEncodings() {
    QFETCH(int, version);
    QFETCH(QByteArray, data);
    QFETCH(QString, expected);
    const ByteSource src(id3Tag(version, version == 4 ? frame24("TIT2", data) : frame23("TIT2", data)));
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.title, expected);
}

void Test_Audiometa_A::id3Genres_data() {
    QTest::addColumn<QByteArray>("raw");
    QTest::addColumn<QString>("expected");
    QTest::newRow("bare number") << "17"_ba << u"Rock"_s;
    QTest::newRow("parenthesised") << "(17)"_ba << u"Rock"_s;
    QTest::newRow("reference and its own name") << "(17)Rock"_ba << u"Rock"_s;
    QTest::newRow("refinement wins") << "(4)Eurodisco"_ba << u"Eurodisco"_s;
    QTest::newRow("two references") << "(17)(26)"_ba << u"Rock; Ambient"_s;
    QTest::newRow("remix") << "RX"_ba << u"Remix"_s;
    QTest::newRow("cover") << "(CR)"_ba << u"Cover"_s;
    QTest::newRow("escaped parenthesis") << "((Foo)"_ba << u"(Foo)"_s;
    QTest::newRow("v2.4 values") << "17\0Ambient\0"_ba << u"Rock; Ambient"_s;
    QTest::newRow("v2.4 duplicate values") << "17\0Rock"_ba << u"Rock"_s;
    QTest::newRow("free text") << "Synthwave"_ba << u"Synthwave"_s;
    QTest::newRow("number outside the list") << "255"_ba << u"255"_s;
    QTest::newRow("reference outside the list") << "(255)"_ba << u"(255)"_s;
    QTest::newRow("unclosed") << "(17"_ba << u"(17"_s;
}

void Test_Audiometa_A::id3Genres() {
    QFETCH(QByteArray, raw);
    QFETCH(QString, expected);
    const ByteSource src(id3Tag(4, frame24("TCON", '\0' + raw)));
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.genre, expected);
}

void Test_Audiometa_A::id3FieldsAndComments() {
    const ByteSource src(sampleId3v24Misc());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.artist, u"A; B"_s);
    QCOMPARE(meta.track, u"3/12"_s);
    QCOMPARE(meta.disc, u"1/2"_s);
    QCOMPARE(meta.composer, u"Composer"_s);
    QCOMPARE(meta.date, u"2026-01-02"_s);
    QCOMPARE(meta.albumArtist, u"Band"_s);
    QCOMPARE(fieldValue(meta, u"REPLAYGAIN_TRACK_GAIN"_s), u"-6.50 dB"_s);
    // The comment without a description wins; iTunes' machine data is not shown at all.
    QCOMPARE(meta.comment, u"main comment"_s);
    QCOMPARE(fieldValue(meta, u"Notes"_s), u"described"_s);
    QVERIFY(!hasValue(meta, u"00000001"_s));
    QVERIFY(!hasValue(meta, u"Sorted"_s));
    QVERIFY(!hasValue(meta, u"1234"_s));
    QVERIFY(!hasValue(meta, u"private"_s));
    QCOMPARE(fieldValue(meta, u"BPM"_s), u"120"_s);
    QCOMPARE(fieldValue(meta, u"TZZZ"_s), u"Custom"_s);
}

void Test_Audiometa_A::id3FrontCoverWins() {
    const ByteSource src(sampleId3Pictures());
    AudioMetadata meta;
    QVERIFY(parseId3v2(src, 0, meta, kAll) > 0);
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.description, u"front"_s);
}

// A file cut short inside its tag still shows what came before the cut, and the size stays the declared one.
void Test_Audiometa_A::id3TruncatedTagKeepsFirstFrames() {
    const QByteArray full = sampleId3v24Misc();
    const ByteSource src(full.left(full.indexOf("TSOP")));
    AudioMetadata meta;
    QCOMPARE(parseId3v2(src, 0, meta, kAll), qint64(full.size()));
    QCOMPARE(meta.artist, u"A; B"_s);
    QCOMPARE(meta.comment, u"main comment"_s);
    QVERIFY(meta.track.isEmpty());
}

void Test_Audiometa_A::id3v2TagSize_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<qint64>("offset");
    QTest::addColumn<qint64>("expected");
    const QByteArray body(100, '\0');
    QTest::newRow("v2.3") << id3Tag(3, body) << qint64(0) << qint64(110);
    QTest::newRow("v2.4 with a footer") << id3Tag(4, body, 0x10) << qint64(0) << qint64(120);
    QTest::newRow("v2.2") << id3Tag(2, body) << qint64(0) << qint64(110);
    QTest::newRow("after other data") << "junk"_ba + id3Tag(3, body) << qint64(4) << qint64(110);
    QTest::newRow("size past EOF is still the size") << id3Tag(3, body).left(20) << qint64(0) << qint64(110);
    QTest::newRow("v2.5") << id3Tag(5, body) << qint64(0) << qint64(0);
    QTest::newRow("revision 0xFF") << "ID3\x03\xFF\0\0\0\0\x10"_ba << qint64(0) << qint64(0);
    QTest::newRow("size not syncsafe") << "ID3\x03\0\0\0\0\x80\0"_ba << qint64(0) << qint64(0);
    QTest::newRow("short") << "ID3\x03\0"_ba << qint64(0) << qint64(0);
    QTest::newRow("no tag") << QByteArray(64, 'x') << qint64(0) << qint64(0);
    QTest::newRow("negative offset") << id3Tag(3, body) << qint64(-1) << qint64(0);
}

void Test_Audiometa_A::id3v2TagSize() {
    QFETCH(QByteArray, bytes);
    QFETCH(qint64, offset);
    QFETCH(qint64, expected);
    const ByteSource src(bytes);
    QCOMPARE(AudioMeta::id3v2TagSize(src, offset), expected);
    AudioMetadata meta;
    QCOMPARE(parseId3v2(src, offset, meta, kAll), expected);
}

// ---------------------------------------------------------------------------------------------------------
// ID3v1 and APE
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::id3v1HandBuilt() {
    const ByteSource src(sampleId3v1(readFixture(u"mp3-bare.mp3"_s)));
    AudioMetadata meta;
    QVERIFY(parseId3v1(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"ID3v1"_s});
    QCOMPARE(meta.title, u"Hand Title"_s);
    QCOMPARE(meta.artist, u"Hand Artist"_s);
    QCOMPARE(meta.album, u"Hand Album"_s);
    QCOMPARE(meta.date, u"1999"_s);
    QCOMPARE(meta.comment, u"A comment"_s);
    QCOMPARE(meta.track, u"7"_s);
    QCOMPARE(meta.genre, u"Ambient"_s);

    // ffmpeg writes ID3v1 as UTF-8; older writers as Latin-1.
    const ByteSource utf8(QByteArray(16, '\0') + id3v1("\xC3\x9C"
                                                       "ber",
                                                       "\xDC"
                                                       "ber",
                                                       "", "", "", 0, 255));
    AudioMetadata utf8Meta;
    QVERIFY(parseId3v1(utf8, utf8Meta, kAll));
    QCOMPARE(utf8Meta.title, u"Über"_s);
    QCOMPARE(utf8Meta.artist, u"Über"_s);
    QVERIFY(utf8Meta.genre.isEmpty());
    QVERIFY(utf8Meta.track.isEmpty());

    const ByteSource tooShort("TAG"_ba + QByteArray(100, 'x'));
    AudioMetadata none;
    QVERIFY(!parseId3v1(tooShort, none, kAll));
    QVERIFY(none.isEmpty() && none.tagTypes.isEmpty());
}

void Test_Audiometa_A::id3v1Plus() {
    const ByteSource src(sampleId3v1Plus(QByteArray(10, '\0')));
    AudioMetadata meta;
    QVERIFY(parseId3v1(src, meta, kAll));
    QCOMPARE(meta.title, u"123456789012345678901234567890continued"_s);
    QCOMPARE(meta.artist, u"Artist"_s);
    QCOMPARE(meta.genre, u"Synthwave"_s);
}

void Test_Audiometa_A::apeWithCoverBeforeId3v1() {
    const ByteSource src(sampleApe(readFixture(u"mp3-bare.mp3"_s)));
    AudioMetadata meta;
    QVERIFY(parseApeTag(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"APEv2"_s});
    QCOMPARE(meta.title, u"Ape Title"_s);
    QCOMPARE(meta.artist, u"Ape Artist"_s);
    QCOMPARE(meta.albumArtist, u"Ape Album Artist"_s);
    QCOMPARE(meta.date, u"2001"_s);
    QCOMPARE(meta.genre, u"Rock; Pop"_s);
    QCOMPARE(meta.track, u"4/10"_s);
    QCOMPARE(fieldValue(meta, u"REPLAYGAIN_ALBUM_GAIN"_s), u"-3.00 dB"_s);
    QVERIFY(!hasValue(meta, u"example.org"_s)); // an external locator, not text
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.description, u"front.png"_s);

    // The ID3v1 tag behind it is still there, and only fills what APE did not.
    QVERIFY(parseId3v1(src, meta, kAll));
    QCOMPARE(meta.title, u"Ape Title"_s);
    QCOMPARE(meta.album, u"V1 Album"_s);
    QCOMPARE(meta.tagTypes, (QStringList{u"APEv2"_s, u"ID3v1"_s}));

    // A picture item without a file name is taken whole.
    const ByteSource bare(QByteArray(8, '\0') + apeTag({apeItem("Cover Art (Front)", redPng(), 1 << 1)}));
    AudioMetadata bareMeta;
    QVERIFY(parseApeTag(bare, bareMeta, kAll));
    QCOMPARE(bareMeta.cover.data, redPng());
}

void Test_Audiometa_A::apeV1() {
    const ByteSource src(sampleApeV1(QByteArray(100, '\0')));
    AudioMetadata meta;
    QVERIFY(parseApeTag(src, meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"APEv1"_s});
    QCOMPARE(meta.title, u"Old Title"_s);
    QCOMPARE(meta.artist, u"Old Artist"_s);
}

void Test_Audiometa_A::apeBeforeLyrics3() {
    const ByteSource src(sampleApeLyrics3(QByteArray(100, '\0')));
    AudioMetadata meta;
    QVERIFY(parseApeTag(src, meta, kAll));
    QCOMPARE(meta.title, u"Before Lyrics"_s);
}

// ---------------------------------------------------------------------------------------------------------
// RIFF, AIFF, DSF
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::dsfMetadataPointer() {
    const QByteArray bytes = sampleDsf();
    AudioMetadata meta;
    QVERIFY(parseDsf(ByteSource(bytes), meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"ID3v2.4"_s});
    QCOMPARE(meta.title, u"DSD Title"_s);
    QCOMPARE(meta.cover.data, redPng());

    for(quint64 pointer : {quint64(1), quint64(bytes.size()), ~quint64(0)}) {
        AudioMetadata none;
        QVERIFY(parseDsf(ByteSource(sampleDsf(pointer)), none, kAll));
        QVERIFY(none.isEmpty());
    }
}

void Test_Audiometa_A::rf64DataSizeFromDs64_data() {
    QTest::addColumn<QByteArray>("magic");
    QTest::newRow("RF64") << "RF64"_ba;
    QTest::newRow("BW64") << "BW64"_ba;
}

// The 'data' chunk says 0xFFFFFFFF; only ds64 knows where it ends and the INFO list after it begins.
void Test_Audiometa_A::rf64DataSizeFromDs64() {
    QFETCH(QByteArray, magic);
    AudioMetadata meta;
    QVERIFY(parseRiffWave(ByteSource(sampleRf64(magic)), meta, kAll));
    QCOMPARE(meta.title, u"RF Title"_s);
    QCOMPARE(meta.artist, u"RF Artist"_s);
}

void Test_Audiometa_A::wavInfoWinsOverId3Chunk() {
    AudioMetadata meta;
    QVERIFY(parseRiffWave(ByteSource(sampleWavId3()), meta, kAll));
    QCOMPARE(meta.tagTypes, (QStringList{u"RIFF INFO"_s, u"ID3v2.3"_s}));
    QCOMPARE(meta.title, u"Info Title"_s);
    QCOMPARE(meta.albumArtist, u"ID3 Album Artist"_s);
    QCOMPARE(meta.cover.data, redPng());
    // INFO text is UTF-8 when it is valid UTF-8, Latin-1 otherwise.
    QCOMPARE(meta.artist, u"Björk"_s);
    QCOMPARE(meta.comment, u"été"_s);
    QCOMPARE(meta.track, u"5"_s);
    QCOMPARE(fieldValue(meta, u"Encoder"_s), u"Encoder X"_s);
    QCOMPARE(fieldValue(meta, u"IZZZ"_s), u"Unknown"_s);
}

void Test_Audiometa_A::aiffTextChunks_data() {
    QTest::addColumn<QByteArray>("form");
    QTest::newRow("AIFF") << "AIFF"_ba;
    QTest::newRow("AIFF-C") << "AIFC"_ba;
}

void Test_Audiometa_A::aiffTextChunks() {
    QFETCH(QByteArray, form);
    AudioMetadata meta;
    QVERIFY(parseAiff(ByteSource(sampleAiff(form)), meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"AIFF"_s});
    QCOMPARE(meta.title, u"Aiff Name"_s);
    QCOMPARE(meta.artist, u"Aiff Author"_s);
    QCOMPARE(meta.comment, u"First note; Second note"_s);
    QCOMPARE(fieldValue(meta, u"Copyright"_s), u"2026 Someone"_s);
}

void Test_Audiometa_A::aiffId3WinsOverTextChunks() {
    AudioMetadata meta;
    QVERIFY(parseAiff(ByteSource(sampleAiffWithId3()), meta, kAll));
    QCOMPARE(meta.title, u"ID3 Title"_s);
    QCOMPARE(meta.artist, u"Aiff Author"_s);
}

// ---------------------------------------------------------------------------------------------------------
// Matroska, hand-built
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::matroskaSeekHeadPastCluster() {
    AudioMetadata meta;
    QVERIFY(parseMatroska(ByteSource(sampleMatroska()), meta, kAll));
    QCOMPARE(meta.tagTypes, QStringList{u"Matroska"_s});
    // Track level (30) for the track, album level (50) for the album; the Segment title loses to TITLE.
    QCOMPARE(meta.title, u"Track Title"_s);
    QCOMPARE(meta.artist, u"Track Artist; Second Artist"_s);
    QCOMPARE(meta.album, u"Album Title"_s);
    QCOMPARE(meta.albumArtist, u"Album Artist"_s);
    QCOMPARE(meta.track, u"5/9"_s);
    QCOMPARE(meta.genre, u"Jazz"_s);
    QCOMPARE(meta.date, u"2020"_s);
    QCOMPARE(meta.composer, u"Comp"_s);
    QCOMPARE(fieldValue(meta, u"DATE_RECORDED"_s), u"2019"_s);
    QCOMPARE(fieldValue(meta, u"CUSTOM"_s), u"Value"_s);
    QCOMPARE(fieldValue(meta, u"COMPOSER/URL"_s), u"https://example.org"_s);
    QVERIFY(!hasValue(meta, u"Artist, S"_s));
    QVERIFY(!hasValue(meta, u"About an attachment"_s));
    QVERIFY(!hasValue(meta, u"Info Title"_s));
    // cover.png beats cover_land, small_cover and the first image.
    QCOMPARE(meta.cover.data, redPng());
    QCOMPARE(meta.cover.type, 3);
    QCOMPARE(meta.cover.mimeType, u"image/png"_s);
    QCOMPARE(meta.cover.description, u"cover.png"_s);
}

void Test_Audiometa_A::matroskaSecondSeekHead() {
    AudioMetadata meta;
    QVERIFY(parseMatroska(ByteSource(sampleMatroskaSecondSeekHead()), meta, kAll));
    // No level-30 tags: the level-50 ARTIST is the track's artist too, TITLE the album's.
    QCOMPARE(meta.artist, u"Found Late"_s);
    QCOMPARE(meta.album, u"Album"_s);
    QVERIFY(meta.title.isEmpty());
    QVERIFY(meta.albumArtist.isEmpty());
}

void Test_Audiometa_A::matroskaDocType() {
    const QByteArray segment = element(kSegment, element(kInfo, element(kTitle, "Title")));
    AudioMetadata webm, foreign;
    QVERIFY(parseMatroska(ByteSource(ebmlHeader("webm"_ba) + segment), webm, kAll));
    QCOMPARE(webm.title, u"Title"_s);
    QVERIFY(!parseMatroska(ByteSource(ebmlHeader("notmkv"_ba) + segment), foreign, kAll));
    QVERIFY(foreign.isEmpty() && foreign.tagTypes.isEmpty());

    // Recognised with nothing in it: true, and nothing added.
    AudioMetadata empty;
    QVERIFY(parseMatroska(ByteSource(ebmlHeader() + element(kSegment, {})), empty, kAll));
    QVERIFY(empty.isEmpty() && empty.tagTypes.isEmpty());
}

// ---------------------------------------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------------------------------------

void Test_Audiometa_A::hostileInputs_data() {
    QTest::addColumn<QByteArray>("bytes");
    for(auto const &[name, bytes] : hostileSamples())
        QTest::newRow(qPrintable(name)) << bytes;
}

// Each must come back fast with nothing absurd in it.
void Test_Audiometa_A::hostileInputs() {
    QFETCH(QByteArray, bytes);
    QElapsedTimer timer;
    timer.start();
    const ByteSource src(bytes);
    AudioMetadata meta;
    parseAll(src, meta);
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(meta.fields.size() < kMaxStructures);
    QVERIFY(meta.cover.data.size() <= bytes.size());
}

// Shapes that once made a parser's work or memory grow faster than the file: a few MB that took minutes or
// gigabytes. Each now has a bound; these hold it to that.

// Tens of thousands of NUL-separated values in one item, and an item of nothing but NULs.
void Test_Audiometa_A::apeTextValuesAreBounded() {
    QByteArray many;
    for(int i = 0; i < 60000; i++)
        many += QByteArray::number(i).rightJustified(6, '0') + '\0';
    const QByteArray bytes =
        QByteArray(16, 'a') + apeTag({apeItem("Artist", many), apeItem("Album", QByteArray(4 * 1024 * 1024, '\0')),
                                      apeItem("Title", "After")});
    QElapsedTimer timer;
    timer.start();
    AudioMetadata meta;
    QVERIFY(parseApeTag(ByteSource(bytes), meta, kAll));
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(meta.artist.count(u"; "_s), qsizetype(kMaxStructures - 1));
    QVERIFY(meta.artist.startsWith(u"000000; 000001; "_s));
    QVERIFY(meta.album.isEmpty());
    QCOMPARE(meta.title, u"After"_s);
}

// "(17)" over and over: past the frame's budget the rest is more of the same, not a refinement to show instead,
// and a 16 MB frame of them no longer costs a string per reference. A plain value after them still counts.
void Test_Audiometa_A::id3GenreReferencesAreBounded() {
    const QByteArray refs = QByteArray("(17)").repeated(kMaxStructures + 100);
    AudioMetadata meta;
    QVERIFY(parseId3v2(ByteSource(id3Tag(4, frame24("TCON", '\0' + refs + "Custom" + '\0' + "Jazz"))), 0, meta, kAll) >
            0);
    QCOMPARE(meta.genre, u"Rock; Jazz"_s);

    QElapsedTimer timer;
    timer.start();
    AudioMetadata large;
    QVERIFY(parseId3v2(ByteSource(id3Tag(4, frame24("TCON", '\0' + QByteArray("(17)").repeated(4 * 1024 * 1024)))), 0,
                       large, kAll) > 0);
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(large.genre, u"Rock"_s);
}

// One INFO list and one ID3v2 chunk, as in real files; further ones are ignored, so a file of many cannot add
// thousands of fields with each.
void Test_Audiometa_A::riffOnlyFirstInfoListAndId3Chunk() {
    const QByteArray first = id3Tag(3, frame23("TPE2", textData(u"First Album Artist"_s, 0)));
    const QByteArray second = id3Tag(3, frame23("TCOM", textData(u"Second Composer"_s, 0)));
    const QByteArray body = "WAVE"_ba + infoList({{"INAM", "First"}}) + riffChunk("id3 ", first) +
                            infoList({{"INAM", "Second"}, {"IART", "Second Artist"}}) + riffChunk("id3 ", second);
    AudioMetadata meta;
    QVERIFY(parseRiffWave(ByteSource("RIFF"_ba + le(quint64(body.size()), 4) + body), meta, kAll));
    QCOMPARE(meta.title, u"First"_s);
    QCOMPARE(meta.albumArtist, u"First Album Artist"_s);
    QVERIFY(meta.artist.isEmpty());
    QVERIFY(meta.composer.isEmpty());

    QByteArray lists = "WAVE";
    for(int list = 0; list < 4; list++) {
        QList<QPair<QByteArray, QByteArray>> entries;
        for(int i = 0; i < 2048; i++)
            entries.append({"IZZZ", QByteArray::number(list * 2048 + i)});
        lists += infoList(entries);
    }
    QElapsedTimer timer;
    timer.start();
    AudioMetadata many;
    QVERIFY(parseRiffWave(ByteSource("RIFF"_ba + le(quint64(lists.size()), 4) + lists), many, kAll));
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(many.fields.size() <= kMaxStructures);
}

// AIFF likewise reads one ID3v2 chunk, and all its text chunks together are held to what one may hold.
void Test_Audiometa_A::aiffTextIsBounded() {
    QByteArray body = "AIFF"_ba + aiffChunk("ID3 ", id3Tag(4, frame24("TIT2", textData(u"First"_s, 3)))) +
                      aiffChunk("ID3 ", id3Tag(4, frame24("TCOM", textData(u"Second Composer"_s, 3))));
    for(int i = 0; i < 2000; i++)
        body += aiffChunk("ANNO", QByteArray::number(i).rightJustified(1000, 'n'));
    AudioMetadata meta;
    QVERIFY(parseAiff(ByteSource("FORM"_ba + be(quint64(body.size()), 4) + body), meta, kAll));
    QCOMPARE(meta.title, u"First"_s);
    QVERIFY(meta.composer.isEmpty());
    // About half of the 2000 annotations fit in a megabyte.
    QVERIFY(meta.comment.startsWith(QString(999, u'n') + u'0'));
    QVERIFY2(meta.comment.size() > 1000 * 1000 && meta.comment.size() < 1100 * 1000,
             qPrintable(QString::number(meta.comment.size())));
}

// A nested tag's name is its parent's path, so a long name was copied into every tag below it: 16 KB times a
// thousand children. Names are short identifiers; a long one is not a tag name.
void Test_Audiometa_A::matroskaLongTagNamesIgnored() {
    QByteArray children;
    for(int i = 0; i < 1000; i++)
        children += simpleTag("CHILD", "child " + QByteArray::number(i));
    const QByteArray simpleTags = simpleTag(QByteArray(16 * 1024, 'N'), "parent", children) +
                                  simpleTag(QByteArray(256, 'K'), "longest kept") +
                                  simpleTag(QByteArray(257, 'L'), "too long");
    AudioMetadata meta;
    QVERIFY(
        parseMatroska(ByteSource(ebmlHeader() + element(kSegment, element(kTags, tag(50, simpleTags)))), meta, kAll));
    QCOMPARE(fieldValue(meta, QString(256, u'K')), u"longest kept"_s);
    QVERIFY(!hasValue(meta, u"parent"_s));
    QVERIFY(!hasValue(meta, u"child"_s));
    QVERIFY(!hasValue(meta, u"too long"_s));
}

// 1300 artists of 12 KB each, which differ only at the end: joined after a hash pass rather than each checked
// against all before it.
void Test_Audiometa_A::matroskaManyValuesOfOneTag() {
    QByteArray simpleTags;
    for(int i = 0; i < 1300; i++)
        simpleTags += simpleTag("ARTIST", QByteArray(12000, 'a') + QByteArray::number(i));
    const QByteArray bytes = ebmlHeader() + element(kSegment, element(kTags, tag(50, simpleTags)));
    QElapsedTimer timer;
    timer.start();
    AudioMetadata meta;
    QVERIFY(parseMatroska(ByteSource(bytes), meta, kTagsOnly));
    QVERIFY2(timer.elapsed() < 150, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(meta.artist.count(u"; "_s), qsizetype(1299));
}

// Every fixture and every hand-built sample, cut short at many points (and cut from the front, which moves an
// end-anchored tag's offsets around) and corrupted at random with a fixed seed. Nothing may crash, trip an
// assertion or take long; the sanitizer build turns any out-of-range read into a failure here.
void Test_Audiometa_A::robustness() {
    QList<QPair<QString, QByteArray>> samples;
    for(const char *file :
        {"mp3-id3v23.mp3", "mp3-id3v24.mp3", "mp3-id3v1.mp3", "mp3-bare.mp3", "wav-info.wav", "wav-id3.wav",
         "aiff-id3.aiff", "wavpack-ape.wv", "tta.tta", "matroska-cover.mka", "opus-audio-only.webm"}) {
        const QByteArray bytes = readFixture(QString::fromLatin1(file));
        QVERIFY2(!bytes.isEmpty(), file);
        samples.append({QString::fromLatin1(file), bytes});
    }
    const QByteArray audio(200, '\x55');
    samples += QList<QPair<QString, QByteArray>>{
        {u"id3v2.2"_s, sampleId3v22()},
        {u"id3v2.3 unsync"_s, sampleId3v23Unsync()},
        {u"id3v2.4 frame flags"_s, sampleId3v24FrameFlags()},
        {u"id3v2.4 tag-wide unsync"_s, sampleId3v24TagUnsync()},
        {u"id3v2.3 frame flags"_s, sampleId3v23FrameFlags()},
        {u"id3v2.4 itunes"_s, sampleId3v24ITunes()},
        {u"id3v2.4 misc"_s, sampleId3v24Misc()},
        {u"id3 pictures"_s, sampleId3Pictures()},
        {u"id3v1"_s, sampleId3v1(audio)},
        {u"id3v1 plus"_s, sampleId3v1Plus(audio)},
        {u"ape"_s, sampleApe(audio)},
        {u"ape v1"_s, sampleApeV1(audio)},
        {u"ape lyrics3"_s, sampleApeLyrics3(audio)},
        {u"dsf"_s, sampleDsf()},
        {u"rf64"_s, sampleRf64()},
        {u"wav id3"_s, sampleWavId3()},
        {u"aiff"_s, sampleAiff()},
        {u"aiff id3"_s, sampleAiffWithId3()},
        {u"matroska"_s, sampleMatroska()},
        {u"matroska second seekhead"_s, sampleMatroskaSecondSeekHead()},
    };
    samples += hostileSamples();

    // Debug builds and busy CI machines are slow, but a parse of a few kilobytes that takes this long is a
    // loop gone wrong, not a slow machine.
    constexpr qint64 kMaxParseNs = 250'000'000;
    QRandomGenerator random(20261007);
    qint64 worst = 0;
    QString worstCase;
    int parses = 0;
    auto check = [&](QByteArray const &bytes, QString const &what) {
        QElapsedTimer timer;
        timer.start();
        const ByteSource src(bytes);
        AudioMetadata meta;
        parseAll(src, meta);
        const qint64 elapsed = timer.nsecsElapsed();
        parses++;
        if(elapsed > worst) {
            worst = elapsed;
            worstCase = what;
        }
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
            check(bytes.left(cut), name + u" cut at "_s + QString::number(cut));
            if(cut % 7 == 0 || size >= 4096)
                check(bytes.mid(cut), name + u" from "_s + QString::number(cut));
        }

        if(size == 0)
            continue;
        static constexpr quint8 kInteresting[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};
        for(int round = 0; round < 300; round++) {
            QByteArray corrupt = bytes;
            const int changes = 1 + random.bounded(4);
            for(int i = 0; i < changes; i++) {
                // Half anywhere, half in the first or last 512 bytes.
                qsizetype at = random.bounded(int(size));
                if(random.bounded(2)) {
                    const int window = int(qMin<qsizetype>(512, size));
                    at = random.bounded(2) ? random.bounded(window) : size - 1 - random.bounded(window);
                }
                corrupt[at] = random.bounded(2) ? char(random.bounded(256))
                                                : char(kInteresting[random.bounded(int(sizeof(kInteresting)))]);
            }
            check(corrupt, name + u" corruption "_s + QString::number(round));
        }
    }
    qInfo("%d parses, slowest %.2f ms (%s)", parses, double(worst) / 1e6, qPrintable(worstCase));
    QVERIFY2(worst < kMaxParseNs, qPrintable(worstCase));
}

QTEST_GUILESS_MAIN(Test_Audiometa_A)
#include "test_audiometa_a.moc"
