#include "audiometa_p.h"

// APEv2 and APEv1 (hydrogenaud.io wiki, "APEv2 specification"): a 32-byte footer describes the items in front
// of it. It ends the file, or sits before an ID3v1 tag, or before a Lyrics3v2 block, which then precedes the
// ID3v1 tag. Monkey's Audio, WavPack, Musepack, TTA and OptimFROG files carry it, and some MP3s.

namespace AudioMeta {

namespace {

constexpr qint64 kFooterSize = 32;
constexpr qint64 kId3v1Size = 128;
constexpr quint32 kIsHeader = 1u << 29;
// Value size, flags, a two-character key and its NUL, and an empty value.
constexpr qint64 kMinItemSize = 11;
constexpr qsizetype kMaxKeyLength = 255;

enum ItemType { Text = 0, Binary = 1, Locator = 2 };

bool readFooter(ByteSource const &src, qint64 at, QByteArray &footer) {
    if(at < 0)
        return false;
    footer = src.read(at, kFooterSize);
    if(footer.size() != kFooterSize || !footer.startsWith("APETAGEX"))
        return false;
    const quint32 version = le32(footer, 8);
    return (version == 1000 || version == 2000) && !(le32(footer, 20) & kIsHeader);
}

// Where a Lyrics3v2 block ending at `end` starts, or -1. Its last 15 bytes are a six-digit size, which counts
// from "LYRICSBEGIN" up to itself, and "LYRICS200".
qint64 lyrics3v2Start(ByteSource const &src, qint64 end) {
    if(end < 15 + 11)
        return -1;
    const QByteArray trailer = src.read(end - 15, 15);
    if(trailer.size() != 15 || !trailer.endsWith("LYRICS200"))
        return -1;
    qint64 size = 0;
    for(int i = 0; i < 6; i++) {
        const char c = trailer.at(i);
        if(c < '0' || c > '9')
            return -1;
        size = size * 10 + (c - '0');
    }
    if(size < 11 || size > end - 15)
        return -1;
    const qint64 start = end - 15 - size;
    return src.read(start, 11) == "LYRICSBEGIN" ? start : -1;
}

// The offset of the footer, trying each place the APE spec and common practice allow, or -1.
qint64 findFooter(ByteSource const &src, QByteArray &footer) {
    qint64 ends[2] = {src.size(), -1};
    if(src.size() >= kId3v1Size && src.read(src.size() - kId3v1Size, 3) == "TAG")
        ends[1] = src.size() - kId3v1Size;
    for(qint64 end : ends) {
        if(end < kFooterSize)
            continue;
        if(readFooter(src, end - kFooterSize, footer))
            return end - kFooterSize;
        const qint64 lyrics = lyrics3v2Start(src, end);
        if(lyrics >= kFooterSize && readFooter(src, lyrics - kFooterSize, footer))
            return lyrics - kFooterSize;
    }
    return -1;
}

struct KeyField {
    const char *key;
    Field field;
};
// Keys are case-insensitive; ffmpeg writes them in lower case, foobar2000 and Mp3tag capitalised.
constexpr KeyField kKeys[] = {
    {"title", Field::Title},
    {"artist", Field::Artist},
    {"album", Field::Album},
    {"album artist", Field::AlbumArtist},
    {"albumartist", Field::AlbumArtist},
    {"album_artist", Field::AlbumArtist},
    {"year", Field::Date},
    {"date", Field::Date},
    {"genre", Field::Genre},
    {"track", Field::Track},
    {"tracknumber", Field::Track},
    {"disc", Field::Disc},
    {"discnumber", Field::Disc},
    {"composer", Field::Composer},
    {"comment", Field::Comment},
};

bool startsLikeImage(QByteArray const &data) {
    return data.startsWith("\xFF\xD8\xFF") || data.startsWith("\x89PNG\r\n\x1A\n") || data.startsWith("GIF87a") ||
           data.startsWith("GIF89a") || (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP");
}

// "Cover Art (Front)" and its siblings hold a file name, a NUL, then the image. Some writers leave the name
// out; an image signature at the start gives that away, and has to be checked first because a PNG signature
// contains a NUL of its own.
void addPicture(AudioMetadata &meta, QByteArray const &lowerKey, QByteArray const &value) {
    AudioPicture picture;
    if(lowerKey == "cover art (front)")
        picture.type = 3;
    else if(lowerKey == "cover art (back)")
        picture.type = 4;
    if(startsLikeImage(value)) {
        picture.data = value;
    } else {
        const qsizetype nul = value.indexOf('\0');
        if(nul < 0)
            return;
        picture.description = QString::fromUtf8(value.left(nul)).trimmed();
        picture.data = value.sliced(nul + 1);
    }
    offerPicture(meta, std::move(picture));
}

void addText(AudioMetadata &meta, QByteArray const &key, QByteArray const &lowerKey, QByteArray const &value) {
    // Walked in place and capped rather than split(): a value of nothing but NULs would otherwise become
    // millions of empty arrays, and checking each part against the rest would take quadratic time.
    QStringList values;
    qsizetype from = 0;
    for(int count = 0; from <= value.size() && count < kMaxStructures; count++) {
        qsizetype end = value.indexOf('\0', from);
        if(end < 0)
            end = value.size();
        const QString text = QString::fromUtf8(value.constData() + from, end - from).trimmed();
        if(!text.isEmpty())
            values.append(text);
        from = end + 1;
    }
    values.removeDuplicates();
    Field field = Field::None;
    for(KeyField const &known : kKeys) {
        if(lowerKey == known.key) {
            field = known.field;
            break;
        }
    }
    addField(meta, field, QString::fromLatin1(key), values.join(QStringLiteral("; ")));
}

} // namespace

bool parseApeTag(ByteSource const &src, AudioMetadata &meta, int parts) {
    QByteArray footer;
    const qint64 footerAt = findFooter(src, footer);
    if(footerAt < 0)
        return false;
    // The size counts the items and the footer, never the optional header in front of them.
    const qint64 tagSize = le32(footer, 12);
    const quint32 itemCount = le32(footer, 16);
    if(tagSize < kFooterSize || tagSize - kFooterSize > footerAt || tagSize - kFooterSize > kMaxBlockBytes)
        return false;
    const qint64 itemsSize = tagSize - kFooterSize;
    if(itemCount > static_cast<quint64>(itemsSize / kMinItemSize))
        return false;
    const bool v1 = le32(footer, 8) == 1000;
    addTagType(meta, v1 ? QStringLiteral("APEv1") : QStringLiteral("APEv2"));

    const QByteArray items = src.read(footerAt - itemsSize, itemsSize);
    qsizetype pos = 0;
    for(quint32 i = 0; i < itemCount && i < quint32(kMaxStructures); i++) {
        if(items.size() - pos < kMinItemSize)
            break;
        const qint64 valueSize = le32(items, pos);
        // APEv1 has no item flags; whatever is there means nothing.
        const int type = v1 ? Text : int((le32(items, pos + 4) >> 1) & 3);
        const qsizetype keyStart = pos + 8;
        const qsizetype keyEnd = items.indexOf('\0', keyStart);
        const qsizetype keyLength = keyEnd - keyStart;
        if(keyEnd < 0 || keyLength < 2 || keyLength > kMaxKeyLength)
            break;
        const QByteArray key = items.mid(keyStart, keyLength);
        bool printable = true;
        for(char c : key)
            printable = printable && c >= 0x20 && c <= 0x7E;
        const qsizetype valueStart = keyStart + keyLength + 1;
        if(!printable || valueSize > items.size() - valueStart)
            break;
        pos = valueStart + static_cast<qsizetype>(valueSize);

        const QByteArray lowerKey = key.toLower();
        // Some writers flag pictures as text, so the key decides, not the type.
        if(lowerKey.startsWith("cover art (")) {
            if(type != Locator && (parts & AudioMetadataReader::Cover))
                addPicture(meta, lowerKey, items.mid(valueStart, valueSize));
        } else if(type == Text && (parts & AudioMetadataReader::Tags)) {
            addText(meta, key, lowerKey, items.mid(valueStart, valueSize));
        }
    }
    return true;
}

} // namespace AudioMeta
