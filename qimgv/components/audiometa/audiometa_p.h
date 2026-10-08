#pragma once

// Internal to components/audiometa: the bounded byte source every parser reads through, the helpers they
// share, and the parsers themselves. Nothing outside this directory should include it.

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtGlobal>
#include "audiometadata.h"

namespace AudioMeta {

// Hard limits. No real tag comes near them; they stop a corrupt length field from allocating gigabytes or
// sending a parser on a walk through a whole multi-gigabyte file.
constexpr qint64 kMaxPictureBytes = 32 * 1024 * 1024; // one embedded picture
constexpr qint64 kMaxBlockBytes = 64 * 1024 * 1024;   // anything read whole: a tag, a box, a packet
constexpr int kMaxStructures = 4096;                  // boxes/frames/pages/elements visited by one parser
constexpr int kMaxFields = 1024;                      // fields kept beyond the well-known ones; a panel's worth

// A file, or a block of memory, that can only be read in exact, in-range pieces.
class ByteSource {
public:
    explicit ByteSource(QString const &path);
    // For data a parser has already pulled out (an Ogg packet, a base64 picture block), and for tests.
    explicit ByteSource(QByteArray data);

    bool isValid() const { return mValid; }
    qint64 size() const { return mSize; }
    // Exactly `length` bytes starting at `offset`. Empty when that range is not entirely inside the
    // source, when `length` is negative or larger than kMaxBlockBytes, or when the read comes up short --
    // so a caller never sees a partial read, and an empty result for a non-zero length always means "no".
    QByteArray read(qint64 offset, qint64 length) const;

private:
    mutable QFile mFile;
    QByteArray mData;
    bool mInMemory = false;
    bool mValid = false;
    qint64 mSize = 0;
};

// Integer reads from a byte array that never index out of range: past the end they return 0. Callers
// check lengths first; these are the second line of defence, not the first.
quint16 be16(QByteArray const &d, qsizetype at);
quint32 be24(QByteArray const &d, qsizetype at);
quint32 be32(QByteArray const &d, qsizetype at);
quint64 be64(QByteArray const &d, qsizetype at);
quint16 le16(QByteArray const &d, qsizetype at);
quint32 le32(QByteArray const &d, qsizetype at);
quint64 le64(QByteArray const &d, qsizetype at);
// ID3v2 "syncsafe" integer: 4 bytes of 7 bits each. Returns false (and leaves `out` alone) if any byte has
// its high bit set, which a valid syncsafe integer never does.
bool syncsafe32(QByteArray const &d, qsizetype at, quint32 &out);

enum class TextEncoding {
    Latin1,
    Utf16Bom, // UTF-16 with a byte order mark; little-endian if the BOM is missing
    Utf16BE,
    Utf16LE,
    Utf8
};
// Decodes `bytes` and drops trailing NULs (tags pad and terminate inconsistently).
QString decodeText(QByteArray const &bytes, TextEncoding encoding);
// Reads one NUL-terminated string starting at `pos` and moves `pos` past its terminator (one zero byte for
// Latin-1/UTF-8, a 2-byte-aligned zero pair for UTF-16). Without a terminator it takes the rest of the data
// and sets `pos` to data.size().
QString readTerminated(QByteArray const &data, qsizetype &pos, TextEncoding encoding);

// The ID3v1 genre list, Winamp extensions included (0..191). ID3v1, ID3v2 "(17)" references and MP4 'gnre'
// atoms all index it. Empty for an index outside the list.
QString id3GenreName(int index);

// The fields every format shares. Parsers map their own keys onto these.
enum class Field { None, Title, Artist, Album, AlbumArtist, Date, Genre, Track, Disc, Composer, Comment };
// The translated, readable name the info panel shows for a well-known field.
QString fieldName(Field field);

// Adds one text field. For a well-known field (`field` != None) it fills the matching AudioMetadata member
// and records it under fieldName(field) -- but only if no earlier tag already did, which is how the
// priority order in AudioMetadataReader::read() is implemented: parse the winning tag first. For other
// fields it records `key` as given, skipping an exact duplicate key/value pair, and stops recording them
// once there are kMaxFields. Values are trimmed; empty ones are dropped.
void addField(AudioMetadata &meta, Field field, QString const &key, QString const &value);
// Keeps `picture` if there is no cover yet, or if it is a front cover (type 3) and the current one is not.
// Ignores empty pictures and ones larger than kMaxPictureBytes.
void offerPicture(AudioMetadata &meta, AudioPicture picture);
// Records a tag type ("ID3v2.4") once.
void addTagType(AudioMetadata &meta, QString const &type);

// AudioMetadataReader::read() and loadCover() from the point where the audio file is open, or already read.
// The robustness test calls these with bytes in memory: far more variants than it could write to disk.
AudioMetadata read(ByteSource const &src, int parts);
QImage loadCover(QByteArray const &embedded, QString const &audioPath, QSize maxSize, QSize *originalSize,
                 AudioCoverSource *source = nullptr);

// ---------------------------------------------------------------------------------------------------------
// Parsers. Each merges what it finds into `meta` (through addField/offerPicture/addTagType) and must stay
// inside `src` whatever the bytes say. `parts` is an AudioMetadataReader::Part mask: without Cover, do not
// copy picture data; without Tags, do not decode text. A parser that finds nothing it recognises returns
// false (or 0) and leaves `meta` untouched.
// ---------------------------------------------------------------------------------------------------------

// ID3v2.2/2.3/2.4 tag starting at `offset`. Returns the tag's total size in bytes (header, frames, padding
// and footer), or 0 if there is no valid tag there.
qint64 parseId3v2(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts);
// The size id3v2 would return without parsing frames: just validates the 10-byte header. 0 if invalid.
qint64 id3v2TagSize(ByteSource const &src, qint64 offset);
// ID3v1 / ID3v1.1 in the last 128 bytes.
bool parseId3v1(ByteSource const &src, AudioMetadata &meta, int parts);
// APEv2 (or APEv1) tag at the end of the file, directly before an ID3v1 tag if there is one.
bool parseApeTag(ByteSource const &src, AudioMetadata &meta, int parts);

// Native FLAC: "fLaC" at `offset`, then metadata blocks.
bool parseFlac(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts);
// The body of a FLAC PICTURE block (also what a Vorbis comment's METADATA_BLOCK_PICTURE decodes to).
void parseFlacPicture(QByteArray const &block, AudioMetadata &meta, int parts);
// A Vorbis comment body: vendor string, count, then "KEY=value" entries. No packet header, no framing bit.
void parseVorbisComment(QByteArray const &data, AudioMetadata &meta, int parts);
// Ogg Vorbis / Opus / FLAC / Speex: walks pages from `offset` and reassembles the comment packet.
bool parseOgg(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts);

// MP4 / M4A / QuickTime: moov.udta.meta.ilst (and a QuickTime-style meta without a version field).
bool parseMp4(ByteSource const &src, AudioMetadata &meta, int parts);
// ASF / WMA: Content Description, Extended Content Description, Metadata (Library) objects; WM/Picture.
bool parseAsf(ByteSource const &src, AudioMetadata &meta, int parts);
// RIFF WAVE (LIST/INFO, "id3 "/"ID3 " chunk), RF64/BW64 alike.
bool parseRiffWave(ByteSource const &src, AudioMetadata &meta, int parts);
// AIFF / AIFF-C (NAME, AUTH, "(c) ", ANNO, "ID3 " chunk).
bool parseAiff(ByteSource const &src, AudioMetadata &meta, int parts);
// DSF: the metadata pointer in the DSD chunk leads to an ID3v2 tag.
bool parseDsf(ByteSource const &src, AudioMetadata &meta, int parts);
// Matroska / WebM: Segment Info title, Tags/SimpleTag, and a cover image among the Attachments.
bool parseMatroska(ByteSource const &src, AudioMetadata &meta, int parts);

} // namespace AudioMeta
