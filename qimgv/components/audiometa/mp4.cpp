#include "audiometa_p.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QList>

namespace AudioMeta {

namespace {

struct Box {
    QByteArray type;
    qint64 body = 0;
    qint64 end = 0;

    qint64 bodySize() const { return end - body; }
};

// One parse's walk over the box tree. Every box visited, at any depth, comes out of one budget, so no
// arrangement of boxes takes a parse past kMaxStructures. The bytes read out of them come out of another:
// moov is never read whole, so nothing else stops a file from feeding gigabytes of values into one parse.
class Boxes {
public:
    explicit Boxes(ByteSource const &src) : mSrc(src) {}

    // Calls `visit` on each box in [begin, end) until it returns false. A box whose size does not fit
    // what is left of its parent ends the walk: past it, offsets mean nothing.
    template<typename Visit>
    void children(qint64 begin, qint64 end, Visit &&visit) {
        qint64 pos = begin;
        Box box;
        while(mBudget > 0 && header(pos, end, box)) {
            mBudget--;
            if(!visit(box))
                return;
            pos = box.end;
        }
    }

    bool find(qint64 begin, qint64 end, QByteArrayView type, Box &found) {
        bool ok = false;
        children(begin, end, [&](Box const &box) {
            ok = box.type == type;
            if(ok)
                found = box;
            return !ok;
        });
        return ok;
    }

    QByteArray read(qint64 offset, qint64 length) {
        if(length < 0 || length > mAllowance)
            return {};
        mAllowance -= length;
        return mSrc.read(offset, length);
    }

private:
    bool header(qint64 pos, qint64 end, Box &box) const {
        if(pos < 0 || end > mSrc.size() || end - pos < 8)
            return false;
        const QByteArray head = mSrc.read(pos, qMin<qint64>(16, end - pos));
        if(head.size() < 8)
            return false;
        quint64 size = be32(head, 0);
        qint64 headerSize = 8;
        if(size == 1) {
            // A 64-bit size, as an mdat over 4 GiB needs.
            if(head.size() < 16)
                return false;
            size = be64(head, 8);
            headerSize = 16;
        } else if(size == 0) {
            // Extends to the end of its parent; at the top level, to the end of the file.
            size = quint64(end - pos);
        }
        if(size < quint64(headerSize) || size > quint64(end - pos))
            return false;
        box.type = head.mid(4, 4);
        box.body = pos + headerSize;
        box.end = pos + qint64(size);
        return true;
    }

    ByteSource const &mSrc;
    int mBudget = kMaxStructures;
    qint64 mAllowance = kMaxBlockBytes;
};

// ilst item names. Well-known fields map onto them; a few others get a readable name, and the rest are
// listed under their four-character code.
struct ItemName {
    QByteArrayView type;
    Field field;
    char const *name;
};

// \251 is 0xA9, the (c) sign that begins the QuickTime-era names.
const ItemName kItemNames[] = {
    {"\251nam", Field::Title, nullptr},
    {"\251ART", Field::Artist, nullptr},
    {"\251alb", Field::Album, nullptr},
    {"aART", Field::AlbumArtist, nullptr},
    {"\251day", Field::Date, nullptr},
    {"\251gen", Field::Genre, nullptr},
    {"gnre", Field::Genre, nullptr},
    {"trkn", Field::Track, nullptr},
    {"disk", Field::Disc, nullptr},
    {"\251wrt", Field::Composer, nullptr},
    {"\251cmt", Field::Comment, nullptr},
    {"\251too", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Encoder")},
    {"\251enc", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Encoded by")},
    {"\251grp", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Grouping")},
    {"\251lyr", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Lyrics")},
    {"\251wrk", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Work")},
    {"\251mvn", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Movement")},
    {"cprt", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Copyright")},
    {"\251cpy", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Copyright")},
    {"desc", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Description")},
    {"\251des", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Description")},
    {"ldes", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Description")},
    {"\251inf", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Information")},
    {"tmpo", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "BPM")},
};

ItemName const *itemName(QByteArrayView type) {
    for(ItemName const &item : kItemNames) {
        if(item.type == type)
            return &item;
    }
    return nullptr;
}

// Adds an item's text under the field or readable name its code has, or else under the code itself.
void addItem(AudioMetadata &meta, QByteArrayView type, QString const &value) {
    ItemName const *known = itemName(type);
    const Field field = known ? known->field : Field::None;
    const QString key =
        known && known->name ? QCoreApplication::translate("AudioMetadata", known->name) : QString::fromLatin1(type);
    addField(meta, field, key, value);
}

bool isQuickTimeName(QByteArrayView type) {
    return type.size() == 4 && quint8(type.at(0)) == 0xA9;
}

// Four-character codes are printable, bar the leading (c) of the QuickTime-era names. Anything else is
// an item keyed some other way (an 'mdta' key index), which these names do not describe.
bool isPrintableName(QByteArrayView type) {
    if(type.size() != 4)
        return false;
    for(qsizetype i = 0; i < type.size(); i++) {
        const quint8 c = quint8(type.at(i));
        if(!(c >= 0x20 && c < 0x7F) && !(i == 0 && c == 0xA9))
            return false;
    }
    return true;
}

// Text from a writer of the QuickTime era is in a classic Mac encoding, Mac Roman for the Western
// languages that nearly all such files use. Plenty of later writers put UTF-8 there regardless, and
// non-ASCII Mac Roman text is almost never valid UTF-8, so valid UTF-8 is taken as UTF-8.
QString legacyText(QByteArray const &bytes) {
    if(bytes.isValidUtf8())
        return decodeText(bytes, TextEncoding::Utf8);
    // Mac Roman 0x80-0xFF; below that it is ASCII.
    static constexpr char16_t kHigh[128] = {
        0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
        0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
        0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
        0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
        0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
        0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
        0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
        0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
        0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
        0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};
    QString text;
    text.reserve(bytes.size());
    for(char c : bytes) {
        const quint8 b = quint8(c);
        text.append(b < 0x80 ? QChar(b) : QChar(kHigh[b - 0x80]));
    }
    while(text.endsWith(QChar(0)))
        text.chop(1);
    return text;
}

// 'data' box type indicators.
constexpr quint32 kImplicit = 0;
constexpr quint32 kUtf8 = 1;
constexpr quint32 kUtf16 = 2;
constexpr quint32 kUtf8Sort = 4;
constexpr quint32 kUtf16Sort = 5;
constexpr quint32 kJpeg = 13;
constexpr quint32 kPng = 14;
constexpr quint32 kBeSignedInt = 21;
constexpr quint32 kBmp = 27;

// The value of one 'data' box of the item `type`, as text; empty if it is not something to show.
QString dataText(QByteArrayView type, quint32 dataType, QByteArray const &value) {
    if(type == "trkn" || type == "disk") {
        // Binary: 16-bit padding, number, total (trkn adds 16 more bits of padding).
        if(value.size() < 6)
            return {};
        const quint16 number = be16(value, 2);
        const quint16 total = be16(value, 4);
        if(number == 0)
            return {};
        return total ? QStringLiteral("%1/%2").arg(number).arg(total) : QString::number(number);
    }
    if(type == "gnre") {
        // The ID3v1 genre index, plus one.
        if(value.size() < 2)
            return {};
        return id3GenreName(int(be16(value, 0)) - 1);
    }
    switch(dataType) {
    case kUtf8:
    case kUtf8Sort:
        return decodeText(value, TextEncoding::Utf8);
    case kUtf16:
    case kUtf16Sort:
        return decodeText(value, TextEncoding::Utf16BE);
    case kBeSignedInt:
        // Only for items we name: most integer items are flags and IDs for iTunes, not text for people.
        if(type == "tmpo" && value.size() == 2)
            return QString::number(be16(value, 0));
        return {};
    case kImplicit:
        // Writers of the QuickTime-style names sometimes leave text untyped.
        return isQuickTimeName(type) ? legacyText(value) : QString();
    }
    return {};
}

// Calls `visit(dataType, value)` for each 'data' box of `item`, with at most `maxValue` bytes of value,
// until it returns false.
template<typename Visit>
void forEachData(Boxes &boxes, Box const &item, qint64 maxValue, Visit &&visit) {
    boxes.children(item.body, item.end, [&](Box const &child) {
        if(child.type != "data" || child.bodySize() < 8)
            return true;
        const qint64 length = child.bodySize() - 8;
        if(length > maxValue)
            return true;
        // The first byte of the indicator is a "type set" that is always 0; the type is the low 24 bits.
        const quint32 dataType = be32(boxes.read(child.body, 4), 0) & 0xFFFFFF;
        return visit(dataType, boxes.read(child.body + 8, length));
    });
}

void parseCovers(Boxes &boxes, Box const &item, AudioMetadata &meta) {
    // Each data box is one picture; the convention is that the first is the front cover. Nothing replaces
    // a front cover, so once there is one the rest are not read: a covr can hold any number of pictures.
    if(meta.cover.type == 3)
        return;
    bool first = true;
    forEachData(boxes, item, kMaxPictureBytes, [&](quint32 dataType, QByteArray const &value) {
        AudioPicture picture;
        picture.data = value;
        picture.type = first ? 3 : -1;
        first = false;
        if(dataType == kJpeg)
            picture.mimeType = QStringLiteral("image/jpeg");
        else if(dataType == kPng)
            picture.mimeType = QStringLiteral("image/png");
        else if(dataType == kBmp)
            picture.mimeType = QStringLiteral("image/bmp");
        offerPicture(meta, std::move(picture));
        return meta.cover.type != 3;
    });
}

// A '----' item: a reverse-DNS 'mean', a 'name' and the value, which is how tools add fields iTunes has no
// atom for (REPLAYGAIN_TRACK_GAIN, MusicBrainz IDs...).
void parseFreeform(Boxes &boxes, Box const &item, AudioMetadata &meta) {
    constexpr qint64 kMaxNameBytes = 1024;
    QString name;
    QStringList values;
    boxes.children(item.body, item.end, [&](Box const &child) {
        if(child.type == "name" && child.bodySize() > 4 && child.bodySize() <= kMaxNameBytes) {
            // A full box: version and flags, then the name.
            name = decodeText(boxes.read(child.body + 4, child.bodySize() - 4), TextEncoding::Utf8);
        } else if(child.type == "data" && child.bodySize() >= 8) {
            const quint32 dataType = be32(boxes.read(child.body, 4), 0) & 0xFFFFFF;
            const QString value = dataText("----", dataType, boxes.read(child.body + 8, child.bodySize() - 8));
            if(!value.trimmed().isEmpty())
                values.append(value);
        }
        return true;
    });
    // Afterwards, by hash: checking each value against the list as it came would take quadratic time.
    values.removeDuplicates();
    // iTunNORM, iTunSMPB, iTunes_CDDB_IDs and the like are Apple's machine data (gain, gapless info, disc
    // IDs) as hex strings, nothing a person reads.
    if(name.isEmpty() || name.startsWith(QLatin1String("iTun")))
        return;
    addField(meta, Field::None, name, values.join(QStringLiteral("; ")));
}

void parseItem(Boxes &boxes, Box const &item, AudioMetadata &meta, int parts) {
    if(item.type == "covr") {
        if(parts & AudioMetadataReader::Cover)
            parseCovers(boxes, item, meta);
        return;
    }
    if(!(parts & AudioMetadataReader::Tags))
        return;
    if(item.type == "----") {
        parseFreeform(boxes, item, meta);
        return;
    }
    if(!isPrintableName(item.type))
        return;
    QStringList values;
    forEachData(boxes, item, kMaxBlockBytes, [&](quint32 dataType, QByteArray const &value) {
        const QString text = dataText(item.type, dataType, value);
        if(!text.trimmed().isEmpty())
            values.append(text);
        return true;
    });
    values.removeDuplicates();
    addItem(meta, item.type, values.join(QStringLiteral("; ")));
}

// A QuickTime user data text atom: one or more [16-bit length, 16-bit language, text] entries, one per
// language. The first that has any text is the one shown.
QString quickTimeText(QByteArray const &payload) {
    qint64 pos = 0;
    for(int i = 0; i < kMaxStructures && payload.size() - pos >= 4; i++) {
        const qint64 length = be16(payload, pos);
        const quint16 language = be16(payload, pos + 2);
        pos += 4;
        if(length > payload.size() - pos)
            break;
        const QByteArray text = payload.mid(pos, length);
        pos += length;
        // A packed ISO 639 code (0x400 and up) means Unicode: UTF-16 with a byte order mark, else UTF-8.
        // Below that it is a classic Mac language code.
        QString decoded;
        if(language >= 0x400 && text.startsWith("\xFE\xFF"))
            decoded = decodeText(text, TextEncoding::Utf16Bom);
        else
            decoded = legacyText(text);
        if(!decoded.trimmed().isEmpty())
            return decoded;
    }
    return {};
}

// ISO 'meta' is a full box, its children after 4 bytes of version and flags; QuickTime's is a plain
// box. Both turn up in .m4a files. The handler box, which comes first, shows where the children start;
// without one, the version and flags are zero where a QuickTime child's size is not.
bool findIlst(Boxes &boxes, Box const &metaBox, Box &ilst) {
    const QByteArray peek = boxes.read(metaBox.body, qMin<qint64>(12, metaBox.bodySize()));
    const bool fullBox = peek.mid(8, 4) == "hdlr" || be32(peek, 0) == 0;
    return boxes.find(fullBox ? metaBox.body + 4 : metaBox.body, metaBox.end, "ilst", ilst);
}

} // namespace

bool parseMp4(ByteSource const &src, AudioMetadata &meta, int parts) {
    if(!(parts & AudioMetadataReader::All))
        return false;
    Boxes boxes(src);
    // moov can come after mdat, which may be over 4 GiB: skipped by its size, never read.
    Box moov;
    if(!boxes.find(0, src.size(), "moov", moov))
        return false;

    Box udta, moovMeta;
    bool haveUdta = false, haveMoovMeta = false;
    boxes.children(moov.body, moov.end, [&](Box const &box) {
        if(box.type == "udta" && !haveUdta) {
            udta = box;
            haveUdta = true;
        } else if(box.type == "meta" && !haveMoovMeta) {
            moovMeta = box;
            haveMoovMeta = true;
        }
        return true;
    });

    // iTunes puts its list in moov.udta.meta; a few writers put the meta straight in moov.
    Box ilst;
    bool haveIlst = false;
    QList<Box> textAtoms;
    if(haveUdta) {
        boxes.children(udta.body, udta.end, [&](Box const &box) {
            if(box.type == "meta" && !haveIlst)
                haveIlst = findIlst(boxes, box, ilst);
            else if(isQuickTimeName(box.type))
                textAtoms.append(box);
            return true;
        });
    }
    if(!haveIlst && haveMoovMeta)
        haveIlst = findIlst(boxes, moovMeta, ilst);

    if(haveIlst) {
        addTagType(meta, QStringLiteral("MP4"));
        boxes.children(ilst.body, ilst.end, [&](Box const &item) {
            parseItem(boxes, item, meta, parts);
            return true;
        });
    } else if(!textAtoms.isEmpty() && (parts & AudioMetadataReader::Tags)) {
        // A QuickTime file without an iTunes list keeps its text in user data atoms.
        addTagType(meta, QStringLiteral("QuickTime"));
        for(Box const &atom : std::as_const(textAtoms))
            addItem(meta, atom.type, quickTimeText(boxes.read(atom.body, atom.bodySize())));
    }
    return true;
}

} // namespace AudioMeta
