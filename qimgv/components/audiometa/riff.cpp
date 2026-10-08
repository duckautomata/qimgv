#include "audiometa_p.h"

#include <QCoreApplication>
#include <QStringDecoder>
#include <algorithm>

// RIFF WAVE with its 64-bit forms RF64 (EBU Tech 3306) and BW64 (ITU-R BS.2088), AIFF/AIFF-C, and DSF. WAVE
// and AIFF have text chunks of their own and, by a convention foobar2000 and Mp3tag started, a whole ID3v2 tag
// in an "id3 " or "ID3 " chunk. DSF has no text of its own: its header points at an ID3v2 tag.

namespace AudioMeta {

namespace {

constexpr qint64 kChunkHeaderSize = 8;
// Text chunks are a few bytes; this only stops a corrupt size from reading megabytes of audio as text.
constexpr qint64 kMaxTextBytes = 1024 * 1024;

struct ChunkRef {
    QByteArray id;
    qint64 offset; // of the data
    qint64 size;   // clamped to the file
};

bool isChunkId(QByteArray const &id) {
    if(id.size() != 4)
        return false;
    for(char c : id) {
        if(c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

// Text that is nominally Latin-1 but that current tools, ffmpeg among them, write as UTF-8. Latin-1 text
// practically never forms valid multi-byte UTF-8, so valid UTF-8 is taken to be UTF-8.
QString latin1OrUtf8(QByteArray const &bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    return decoder.hasError() ? QString::fromLatin1(bytes) : text;
}

// Chunk text is NUL-terminated, sometimes with garbage after the terminator.
QString chunkText(QByteArray const &data) {
    const qsizetype nul = data.indexOf('\0');
    return latin1OrUtf8(nul < 0 ? data : data.left(nul)).trimmed();
}

// One allowance covers all of a file's text chunks, of which there can be thousands.
QString readChunkText(ByteSource const &src, ChunkRef const &chunk, qint64 &allowance) {
    const qint64 length = std::min(chunk.size, allowance);
    allowance -= length;
    return chunkText(src.read(chunk.offset, length));
}

struct InfoKey {
    char id[5];
    Field field;
    const char *name;
};
constexpr InfoKey kInfoKeys[] = {
    {"INAM", Field::Title, nullptr},
    {"IART", Field::Artist, nullptr},
    {"IPRD", Field::Album, nullptr},
    {"ICRD", Field::Date, nullptr},
    {"IGNR", Field::Genre, nullptr},
    // ITRK is the de facto track number; ffmpeg and Mp3tag write "part" for it.
    {"ITRK", Field::Track, nullptr},
    {"IPRT", Field::Track, nullptr},
    {"ICMT", Field::Comment, nullptr},
    {"ICOP", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Copyright")},
    {"ISFT", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Encoder")},
    {"IENG", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Engineer")},
    {"ITCH", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Technician")},
    {"IKEY", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Keywords")},
    {"ISBJ", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Subject")},
    {"ILNG", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Language")},
    {"ISRC", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Source")},
    {"IMED", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Medium")},
    {"IARL", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Archival location")},
    {"ICMS", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Commissioned by")},
};

void addInfoField(AudioMetadata &meta, QByteArray const &id, QByteArray const &data) {
    Field field = Field::None;
    QString key = QString::fromLatin1(id);
    for(InfoKey const &known : kInfoKeys) {
        if(id == known.id) {
            field = known.field;
            if(known.name)
                key = QCoreApplication::translate("AudioMetadata", known.name);
            break;
        }
    }
    addField(meta, field, key, chunkText(data));
}

// The subchunks of a LIST/INFO chunk, each an ID, a little-endian size and text padded to an even length.
void parseInfoList(ByteSource const &src, ChunkRef const &list, AudioMetadata &meta, int parts) {
    addTagType(meta, QStringLiteral("RIFF INFO"));
    if(!(parts & AudioMetadataReader::Tags))
        return;
    const QByteArray data = src.read(list.offset, std::min(list.size, kMaxTextBytes));
    qsizetype pos = 0;
    for(int count = 0; count < kMaxStructures && data.size() - pos >= kChunkHeaderSize; count++) {
        const QByteArray id = data.mid(pos, 4);
        const qint64 size = le32(data, pos + 4);
        const qsizetype start = pos + kChunkHeaderSize;
        if(!isChunkId(id) || size > data.size() - start)
            break;
        addInfoField(meta, id, data.mid(start, size));
        pos = start + static_cast<qsizetype>(size + (size & 1));
    }
}

} // namespace

bool parseRiffWave(ByteSource const &src, AudioMetadata &meta, int parts) {
    const QByteArray head = src.read(0, 12);
    if(head.size() != 12 || head.mid(8, 4) != "WAVE")
        return false;
    const QByteArray magic = head.left(4);
    const bool is64 = magic == "RF64" || magic == "BW64";
    if(magic != "RIFF" && !is64)
        return false;

    QList<ChunkRef> infoLists, id3Chunks;
    qint64 dataSize64 = -1;
    qint64 pos = 12;
    for(int count = 0; count < kMaxStructures && src.size() - pos >= kChunkHeaderSize; count++) {
        const QByteArray header = src.read(pos, kChunkHeaderSize);
        const QByteArray id = header.left(4);
        if(!isChunkId(id))
            break;
        qint64 size = le32(header, 4);
        const qint64 start = pos + kChunkHeaderSize;
        if(is64 && id == "ds64") {
            // The real 64-bit sizes of the RIFF and of 'data', whose own 32-bit fields then say 0xFFFFFFFF.
            const QByteArray ds64 = src.read(start, 16);
            const quint64 dataSize = le64(ds64, 8);
            if(ds64.size() == 16 && dataSize <= quint64(src.size()))
                dataSize64 = qint64(dataSize);
        } else if(is64 && id == "data" && size == 0xFFFFFFFF && dataSize64 >= 0) {
            size = dataSize64;
        }
        const qint64 available = std::min(size, src.size() - start);
        // Only the first of each, as TagLib does: real files have one, and every further one could bring
        // thousands more fields.
        if(id == "LIST" && available >= 4 && infoLists.isEmpty() && src.read(start, 4) == "INFO")
            infoLists.append({id, start + 4, available - 4});
        else if((id == "id3 " || id == "ID3 ") && id3Chunks.isEmpty())
            id3Chunks.append({id, start, available});
        // Past the end: a truncated file, or the 'data' chunk of a recording that was never finalised.
        if(size > src.size() - start)
            break;
        pos = start + size + (size & 1);
    }

    // RIFF INFO is the container's own tag and wins over the ID3v2 one, whatever order the chunks come in.
    for(ChunkRef const &list : std::as_const(infoLists))
        parseInfoList(src, list, meta, parts);
    for(ChunkRef const &chunk : std::as_const(id3Chunks))
        parseId3v2(src, chunk.offset, meta, parts);
    return true;
}

bool parseAiff(ByteSource const &src, AudioMetadata &meta, int parts) {
    const QByteArray head = src.read(0, 12);
    if(head.size() != 12 || !head.startsWith("FORM") || (head.mid(8, 4) != "AIFF" && head.mid(8, 4) != "AIFC"))
        return false;

    QList<ChunkRef> texts, id3Chunks;
    qint64 pos = 12;
    for(int count = 0; count < kMaxStructures && src.size() - pos >= kChunkHeaderSize; count++) {
        const QByteArray header = src.read(pos, kChunkHeaderSize);
        const QByteArray id = header.left(4);
        if(!isChunkId(id))
            break;
        const qint64 size = be32(header, 4);
        const qint64 start = pos + kChunkHeaderSize;
        const qint64 available = std::min(size, src.size() - start);
        if(id == "NAME" || id == "AUTH" || id == "(c) " || id == "ANNO")
            texts.append({id, start, available});
        else if((id == "ID3 " || id == "id3 ") && id3Chunks.isEmpty())
            id3Chunks.append({id, start, available});
        if(size > src.size() - start)
            break;
        pos = start + size + (size & 1);
    }

    // Unlike WAVE's INFO, taggers leave AIFF's text chunks alone and write ID3v2, so the ID3v2 tag is the one
    // that is up to date when the two disagree.
    for(ChunkRef const &chunk : std::as_const(id3Chunks))
        parseId3v2(src, chunk.offset, meta, parts);
    if(texts.isEmpty())
        return true;
    addTagType(meta, QStringLiteral("AIFF"));
    if(!(parts & AudioMetadataReader::Tags))
        return true;
    QStringList annotations;
    qint64 allowance = kMaxTextBytes;
    for(ChunkRef const &chunk : std::as_const(texts)) {
        const QString text = readChunkText(src, chunk, allowance);
        if(chunk.id == "NAME")
            addField(meta, Field::Title, {}, text);
        else if(chunk.id == "AUTH")
            addField(meta, Field::Artist, {}, text);
        else if(chunk.id == "(c) ")
            addField(meta, Field::None, QCoreApplication::translate("AudioMetadata", "Copyright"), text);
        else if(!text.isEmpty())
            annotations.append(text);
    }
    annotations.removeDuplicates();
    addField(meta, Field::Comment, {}, annotations.join(QStringLiteral("; ")));
    return true;
}

bool parseDsf(ByteSource const &src, AudioMetadata &meta, int parts) {
    // "DSD ", the chunk's size (always 28), the file size and the offset of the metadata chunk, 0 for none.
    const QByteArray head = src.read(0, 28);
    if(head.size() != 28 || !head.startsWith("DSD ") || le64(head, 4) != 28)
        return false;
    const quint64 metadata = le64(head, 20);
    if(metadata >= 28 && metadata < quint64(src.size()))
        parseId3v2(src, qint64(metadata), meta, parts);
    return true;
}

} // namespace AudioMeta
