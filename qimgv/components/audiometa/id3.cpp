#include "audiometa_p.h"

#include <QCoreApplication>
#include <QStringDecoder>
#include <algorithm>

// ID3v2.2, 2.3 and 2.4 (id3.org: id3v2-00, id3v2.3.0, id3v2.4.0-structure and -frames), and ID3v1 with its
// v1.1 track byte and the TAG+ extension in front of it.

namespace AudioMeta {

namespace {

constexpr qint64 kHeaderSize = 10;
constexpr qint64 kId3v1Size = 128;
constexpr qint64 kId3v1PlusSize = 227;

// Tag header flags.
constexpr quint8 kTagUnsync = 0x80;
constexpr quint8 kTagExtendedHeader = 0x40; // v2.2: compression, a scheme the spec never defined
constexpr quint8 kTagFooter = 0x10;

// The second byte of a frame's flags, which describes how its data is stored.
constexpr quint8 kV23Compressed = 0x80;
constexpr quint8 kV23Encrypted = 0x40;
constexpr quint8 kV23Grouped = 0x20;
constexpr quint8 kV24Grouped = 0x40;
constexpr quint8 kV24Compressed = 0x08;
constexpr quint8 kV24Encrypted = 0x04;
constexpr quint8 kV24Unsync = 0x02;
constexpr quint8 kV24DataLength = 0x01;

// Unsynchronisation put a zero after every 0xFF that a player could mistake for an MPEG sync word.
QByteArray undoUnsync(QByteArray const &data) {
    static constexpr char kPair[] = {'\xFF', '\0'};
    QByteArray out;
    out.reserve(data.size());
    qsizetype from = 0;
    for(;;) {
        const qsizetype at = data.indexOf(QByteArrayView(kPair, 2), from);
        if(at < 0)
            break;
        out.append(data.constData() + from, at + 1 - from);
        from = at + 2;
    }
    out.append(data.constData() + from, data.size() - from);
    return out;
}

bool textEncoding(char byte, TextEncoding &out) {
    switch(byte) {
    case 0:
        out = TextEncoding::Latin1;
        return true;
    case 1:
        out = TextEncoding::Utf16Bom;
        return true;
    case 2:
        out = TextEncoding::Utf16BE;
        return true;
    case 3:
        out = TextEncoding::Utf8;
        return true;
    }
    return false;
}

bool isWide(TextEncoding encoding) {
    return encoding == TextEncoding::Utf16Bom || encoding == TextEncoding::Utf16BE || encoding == TextEncoding::Utf16LE;
}

// The values of a text field from `pos` on. v2.4 separates several with the encoding's NUL, and writers use
// the same convention in v2.3. Each UTF-16 value should carry its own BOM, but some writers give one only to
// the first, so a value without one keeps the byte order of the one before.
QStringList textValues(QByteArray const &data, qsizetype pos, TextEncoding encoding) {
    QStringList values;
    const bool wide = isWide(encoding);
    TextEncoding order = encoding == TextEncoding::Utf16Bom ? TextEncoding::Utf16LE : encoding;
    for(int count = 0; pos < data.size() && count < kMaxStructures; count++) {
        qsizetype end = -1;
        if(wide) {
            for(qsizetype i = pos; i + 1 < data.size(); i += 2) {
                if(data.at(i) == 0 && data.at(i + 1) == 0) {
                    end = i;
                    break;
                }
            }
        } else {
            end = data.indexOf('\0', pos);
        }
        const qsizetype next = end < 0 ? data.size() : end + (wide ? 2 : 1);
        if(end < 0)
            end = data.size();
        QByteArray piece = data.mid(pos, end - pos);
        if(encoding == TextEncoding::Utf16Bom && piece.size() >= 2) {
            const quint8 b0 = static_cast<quint8>(piece.at(0)), b1 = static_cast<quint8>(piece.at(1));
            if(b0 == 0xFE && b1 == 0xFF) {
                order = TextEncoding::Utf16BE;
                piece.remove(0, 2);
            } else if(b0 == 0xFF && b1 == 0xFE) {
                order = TextEncoding::Utf16LE;
                piece.remove(0, 2);
            }
        }
        const QString value = decodeText(piece, order).trimmed();
        if(!value.isEmpty())
            values.append(value);
        pos = next;
    }
    values.removeDuplicates();
    return values;
}

// Text that is nominally Latin-1 but that current tools, ffmpeg among them, write as UTF-8. Latin-1 text
// practically never forms valid multi-byte UTF-8, so valid UTF-8 is taken to be UTF-8.
QString latin1OrUtf8(QByteArray const &bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    return decoder.hasError() ? QString::fromLatin1(bytes) : text;
}

bool isFrameId(QByteArray const &d, qsizetype at, int length) {
    if(at < 0 || length > d.size() - at)
        return false;
    for(int i = 0; i < length; i++) {
        const char c = d.at(at + i);
        if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

// Whether a frame may end at `at`: the end of the tag, the start of the padding, or the next frame's ID.
bool isFrameEnd(QByteArray const &body, qint64 at, int idLength) {
    if(at == body.size())
        return true;
    if(at < 0 || at > body.size())
        return false;
    return body.at(at) == 0 || isFrameId(body, at, idLength);
}

// iTunes, and a few others, wrote v2.4 frame sizes as plain integers, the v2.3 way. Like TagLib, believe the
// syncsafe reading unless it misses the next frame and the plain one lands on it.
qint64 v24FrameSize(QByteArray const &body, qsizetype pos) {
    const qint64 plain = be32(body, pos + 4);
    quint32 safe = 0;
    const bool isSafe = syncsafe32(body, pos + 4, safe);
    const qint64 next = pos + kHeaderSize;
    auto landsOnFrame = [&](qint64 size) { return size <= body.size() - next && isFrameEnd(body, next + size, 4); };
    if(isSafe && (safe == plain || landsOnFrame(safe)))
        return safe;
    if(landsOnFrame(plain))
        return plain;
    return isSafe ? qint64(safe) : plain;
}

// v2.3 sizes it without its own size field, v2.4 (syncsafe) with it.
bool skipExtendedHeader(QByteArray const &body, int version, qsizetype &pos) {
    if(body.size() < 4)
        return false;
    qint64 length = 0;
    if(version == 3) {
        length = qint64(be32(body, 0)) + 4;
    } else {
        quint32 size = 0;
        if(!syncsafe32(body, 0, size))
            return false;
        length = size;
    }
    if(length < 6 || length > body.size())
        return false;
    pos = static_cast<qsizetype>(length);
    return true;
}

// ID3v2.2's three-character frame IDs as their v2.3 equivalents, so one mapping serves both. The iTunes-only
// ones (sort orders, compilation, grouping, movement) are included.
struct IdAlias {
    char v22[4];
    char v23[5];
};
constexpr IdAlias kV22Ids[] = {{"TT1", "TIT1"}, {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TP1", "TPE1"}, {"TP2", "TPE2"},
                               {"TP3", "TPE3"}, {"TP4", "TPE4"}, {"TCM", "TCOM"}, {"TXT", "TEXT"}, {"TLA", "TLAN"},
                               {"TCO", "TCON"}, {"TAL", "TALB"}, {"TPA", "TPOS"}, {"TRK", "TRCK"}, {"TRC", "TSRC"},
                               {"TYE", "TYER"}, {"TDA", "TDAT"}, {"TIM", "TIME"}, {"TRD", "TRDA"}, {"TMT", "TMED"},
                               {"TBP", "TBPM"}, {"TCR", "TCOP"}, {"TPB", "TPUB"}, {"TEN", "TENC"}, {"TSS", "TSSE"},
                               {"TOF", "TOFN"}, {"TLE", "TLEN"}, {"TSI", "TSIZ"}, {"TDY", "TDLY"}, {"TKE", "TKEY"},
                               {"TOT", "TOAL"}, {"TOA", "TOPE"}, {"TOL", "TOLY"}, {"TOR", "TORY"}, {"TXX", "TXXX"},
                               {"COM", "COMM"}, {"TST", "TSOT"}, {"TSP", "TSOP"}, {"TSA", "TSOA"}, {"TS2", "TSO2"},
                               {"TSC", "TSOC"}, {"TCP", "TCMP"}, {"GP1", "GRP1"}, {"MVN", "MVNM"}, {"MVI", "MVIN"}};

QByteArray v23Id(QByteArray const &id) {
    for(IdAlias const &alias : kV22Ids) {
        if(id == alias.v22)
            return QByteArray(alias.v23);
    }
    return id;
}

// Text frames with a well-known field or an obvious readable name. Anything else is shown under its ID.
struct TextFrame {
    char id[5];
    Field field;
    const char *name;
};
constexpr TextFrame kTextFrames[] = {
    {"TIT2", Field::Title, nullptr},
    {"TPE1", Field::Artist, nullptr},
    {"TALB", Field::Album, nullptr},
    {"TPE2", Field::AlbumArtist, nullptr},
    {"TDRC", Field::Date, nullptr},
    {"TYER", Field::Date, nullptr},
    {"TCON", Field::Genre, nullptr},
    {"TRCK", Field::Track, nullptr},
    {"TPOS", Field::Disc, nullptr},
    {"TCOM", Field::Composer, nullptr},
    {"TBPM", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "BPM")},
    {"TKEY", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Key")},
    {"TPUB", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Publisher")},
    {"TCOP", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Copyright")},
    {"TENC", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Encoded by")},
    {"TSSE", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Encoder")},
    {"TLAN", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Language")},
    {"TIT1", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Grouping")},
    {"GRP1", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Grouping")},
    {"TIT3", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Subtitle")},
    {"TOPE", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Original artist")},
    {"TOAL", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Original album")},
    {"TDOR", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Original date")},
    {"TORY", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Original date")},
    {"TPE3", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Conductor")},
    {"TPE4", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Remixer")},
    {"TEXT", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Lyricist")},
    {"TMOO", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Mood")},
    {"TMED", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Media")},
    {"TSRC", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "ISRC")},
    {"TCMP", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Compilation")},
    {"MVNM", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Movement")},
    {"MVIN", Field::None, QT_TRANSLATE_NOOP("AudioMetadata", "Movement number")},
};

// Sort orders, the length, and v2.3's day/time fragments of the year: nothing a reader wants listed.
constexpr char kSkippedFrames[][5] = {"TSOA", "TSOP", "TSOT", "TSO2", "TSOC", "TLEN", "TDAT", "TIME", "TSIZ"};

// "17", "(17)", "(17)Rock", "(17)(26)", "RX", "(CR)": ID3v1 genre numbers, bare as v2.4 writes them or in
// v2.3's parentheses, and the two named references. Text after the references refines them and replaces
// them; "((" escapes a literal parenthesis.
QString genreReference(QStringView token) {
    if(token == u"RX")
        return QStringLiteral("Remix");
    if(token == u"CR")
        return QStringLiteral("Cover");
    if(token.isEmpty() || token.size() > 3)
        return {};
    for(QChar c : token) {
        if(c.unicode() < u'0' || c.unicode() > u'9')
            return {};
    }
    return id3GenreName(token.toInt());
}

// `budget` is shared by all of a frame's values: a frame of nothing but "(1)" would otherwise cost a
// lookup and a string for each of its millions of references.
QStringList resolveGenre(QString const &value, int &budget) {
    QStringList references;
    qsizetype pos = 0;
    while(pos + 1 < value.size() && value.at(pos) == u'(' && value.at(pos + 1) != u'(') {
        // Out of budget, what is left is more of the same rather than a refinement.
        if(budget == 0)
            return references;
        budget--;
        const qsizetype close = value.indexOf(u')', pos + 1);
        if(close < 0)
            break;
        const QString name = genreReference(QStringView(value).sliced(pos + 1, close - pos - 1));
        if(name.isEmpty())
            break;
        if(!references.contains(name))
            references.append(name);
        pos = close + 1;
    }
    QString refinement = value.sliced(pos).trimmed();
    if(refinement.startsWith(QStringLiteral("((")))
        refinement.remove(0, 1);
    if(refinement.isEmpty())
        return references;
    if(references.isEmpty()) {
        const QString name = genreReference(refinement);
        if(!name.isEmpty())
            return {name};
    }
    return {refinement};
}

struct Comment {
    QString description;
    QString text;
};

struct Id3Reader {
    AudioMetadata &meta;
    int parts;
    int version;
    QList<Comment> comments;

    void frame(QByteArray id, QByteArray const &body, qsizetype pos, qsizetype size, quint8 format, quint8 tagFlags);
    void text(QByteArray const &id, QByteArray const &data);
    void comment(QByteArray const &data);
    void picture(QByteArray const &data, bool v22);
    void finish();
};

void Id3Reader::frame(QByteArray id, QByteArray const &body, qsizetype pos, qsizetype size, quint8 format,
                      quint8 tagFlags) {
    const bool v22Picture = version == 2 && id == "PIC";
    if(version == 2 && !v22Picture)
        id = v23Id(id);
    const bool isPicture = v22Picture || id == "APIC";
    const bool isText = id.startsWith('T') || id == "COMM" || id == "GRP1" || id == "MVNM" || id == "MVIN";
    // Decide before copying: a Tags-only read should not copy every picture.
    if(!(isPicture && (parts & AudioMetadataReader::Cover)) && !(isText && (parts & AudioMetadataReader::Tags)))
        return;

    QByteArray data = body.mid(pos, size);
    if(version == 3) {
        if(format & (kV23Compressed | kV23Encrypted))
            return;
        if(format & kV23Grouped)
            data.remove(0, 1);
    } else if(version == 4) {
        if(format & (kV24Compressed | kV24Encrypted))
            return;
        const qsizetype extra = ((format & kV24Grouped) ? 1 : 0) + ((format & kV24DataLength) ? 4 : 0);
        if(extra > data.size())
            return;
        data.remove(0, extra);
        // The tag-wide flag means every frame is unsynchronised, whether or not the frame says so.
        if((format & kV24Unsync) || (tagFlags & kTagUnsync))
            data = undoUnsync(data);
    }
    if(isPicture)
        picture(data, v22Picture);
    else if(id == "COMM")
        comment(data);
    else
        text(id, data);
}

void Id3Reader::text(QByteArray const &id, QByteArray const &data) {
    TextEncoding encoding;
    if(data.isEmpty() || !textEncoding(data.at(0), encoding))
        return;
    if(id == "TXXX") {
        qsizetype pos = 1;
        const QString description = readTerminated(data, pos, encoding).trimmed();
        const QString value = textValues(data, pos, encoding).join(QStringLiteral("; "));
        addField(meta, Field::None, description.isEmpty() ? QStringLiteral("TXXX") : description, value);
        return;
    }
    for(char const *skipped : kSkippedFrames) {
        if(id == skipped)
            return;
    }
    QStringList values = textValues(data, 1, encoding);
    Field field = Field::None;
    QString key = QString::fromLatin1(id);
    for(TextFrame const &known : kTextFrames) {
        if(id == known.id) {
            field = known.field;
            if(known.name)
                key = QCoreApplication::translate("AudioMetadata", known.name);
            break;
        }
    }
    if(field == Field::Genre) {
        QStringList genres;
        int budget = kMaxStructures;
        for(QString const &value : std::as_const(values))
            genres.append(resolveGenre(value, budget));
        genres.removeDuplicates();
        values = genres;
    }
    addField(meta, field, key, values.join(QStringLiteral("; ")));
}

void Id3Reader::comment(QByteArray const &data) {
    TextEncoding encoding;
    if(data.size() < 4 || !textEncoding(data.at(0), encoding))
        return;
    qsizetype pos = 4; // after the encoding and the 3-letter language
    Comment c;
    c.description = readTerminated(data, pos, encoding).trimmed();
    c.text = readTerminated(data, pos, encoding).trimmed();
    // iTunNORM, iTunSMPB, iTunPGAP, iTunes_CDDB_IDs...: iTunes' private data, hex numbers in a comment.
    if(!c.text.isEmpty() && !c.description.startsWith(QStringLiteral("iTun")))
        comments.append(c);
}

void Id3Reader::picture(QByteArray const &data, bool v22) {
    TextEncoding encoding;
    if(data.size() < 2 || !textEncoding(data.at(0), encoding))
        return;
    AudioPicture result;
    qsizetype pos = 1;
    if(v22) {
        if(data.size() < 5)
            return;
        const QByteArray format = data.mid(1, 3).toUpper();
        if(format == "JPG")
            result.mimeType = QStringLiteral("image/jpeg");
        else if(format == "PNG" || format == "GIF" || format == "BMP")
            result.mimeType = QStringLiteral("image/") + QString::fromLatin1(format).toLower();
        else if(format == "-->")
            return; // a link to a file elsewhere
        pos = 4;
    } else {
        result.mimeType = readTerminated(data, pos, TextEncoding::Latin1).trimmed();
        if(result.mimeType == QStringLiteral("-->"))
            return;
    }
    if(pos >= data.size())
        return;
    result.type = static_cast<quint8>(data.at(pos++));
    result.description = readTerminated(data, pos, encoding).trimmed();
    if(pos >= data.size())
        return;
    result.data = data.sliced(pos);
    offerPicture(meta, std::move(result));
}

// The comment without a description is the one people mean; others are listed under their description.
void Id3Reader::finish() {
    qsizetype main = -1;
    for(qsizetype i = 0; i < comments.size() && main < 0; i++) {
        if(comments.at(i).description.isEmpty())
            main = i;
    }
    if(main < 0 && !comments.isEmpty())
        main = 0;
    for(qsizetype i = 0; i < comments.size(); i++) {
        Comment const &c = comments.at(i);
        if(i == main)
            addField(meta, Field::Comment, {}, c.text);
        else
            addField(meta, Field::None, c.description.isEmpty() ? fieldName(Field::Comment) : c.description, c.text);
    }
}

// A fixed-width ID3v1 field: NUL-padded by the spec, space-padded by some writers, and cut at the first NUL
// because what follows one is often left over from an earlier, longer value.
QString v1Text(QByteArray const &field) {
    const qsizetype nul = field.indexOf('\0');
    return latin1OrUtf8(nul < 0 ? field : field.left(nul)).trimmed();
}

} // namespace

qint64 id3v2TagSize(ByteSource const &src, qint64 offset) {
    const QByteArray header = src.read(offset, kHeaderSize);
    if(header.size() != kHeaderSize || !header.startsWith("ID3"))
        return 0;
    const quint8 version = static_cast<quint8>(header.at(3));
    const quint8 revision = static_cast<quint8>(header.at(4));
    const quint8 flags = static_cast<quint8>(header.at(5));
    quint32 size = 0;
    if(version < 2 || version > 4 || revision == 0xFF || !syncsafe32(header, 6, size))
        return 0;
    qint64 total = kHeaderSize + size;
    if(version == 4 && (flags & kTagFooter))
        total += kHeaderSize;
    return total;
}

qint64 parseId3v2(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts) {
    const qint64 total = id3v2TagSize(src, offset);
    if(total == 0)
        return 0;
    // Read again, and the file may have changed in between: a short read must not be indexed.
    const QByteArray header = src.read(offset, kHeaderSize);
    if(header.size() != kHeaderSize)
        return 0;
    const int version = static_cast<quint8>(header.at(3));
    const quint8 flags = static_cast<quint8>(header.at(5));
    addTagType(meta, QStringLiteral("ID3v2.%1").arg(version));
    if(version == 2 && (flags & kTagExtendedHeader))
        return total;

    quint32 declared = 0;
    syncsafe32(header, 6, declared);
    // A truncated file still has its first frames, and nothing in a frame points outside the tag, so read what
    // there is.
    const qint64 bodySize = std::min({qint64(declared), src.size() - offset - kHeaderSize, kMaxBlockBytes});
    QByteArray body = src.read(offset + kHeaderSize, bodySize);
    if(body.isEmpty())
        return total;
    // Before v2.4 the whole tag, extended header included, is unsynchronised as one; v2.4 does it per frame.
    if(version < 4 && (flags & kTagUnsync))
        body = undoUnsync(body);

    qsizetype pos = 0;
    if(version >= 3 && (flags & kTagExtendedHeader) && !skipExtendedHeader(body, version, pos))
        return total;

    Id3Reader reader{meta, parts, version, {}};
    const int idLength = version == 2 ? 3 : 4;
    const qsizetype frameHeaderSize = version == 2 ? 6 : kHeaderSize;
    for(int count = 0; count < kMaxStructures && body.size() - pos >= frameHeaderSize; count++) {
        // Padding, or a frame ID no writer would produce: either way there are no more frames.
        if(body.at(pos) == 0 || !isFrameId(body, pos, idLength))
            break;
        qint64 size = 0;
        quint8 format = 0;
        if(version == 2) {
            size = be24(body, pos + 3);
        } else if(version == 3) {
            size = be32(body, pos + 4);
            format = static_cast<quint8>(body.at(pos + 9));
        } else {
            size = v24FrameSize(body, pos);
            format = static_cast<quint8>(body.at(pos + 9));
        }
        if(size > body.size() - pos - frameHeaderSize)
            break;
        reader.frame(body.mid(pos, idLength), body, pos + frameHeaderSize, static_cast<qsizetype>(size), format, flags);
        pos += frameHeaderSize + static_cast<qsizetype>(size);
    }
    reader.finish();
    return total;
}

bool parseId3v1(ByteSource const &src, AudioMetadata &meta, int parts) {
    if(src.size() < kId3v1Size)
        return false;
    const QByteArray tag = src.read(src.size() - kId3v1Size, kId3v1Size);
    if(tag.size() != kId3v1Size || !tag.startsWith("TAG"))
        return false;
    addTagType(meta, QStringLiteral("ID3v1"));
    if(!(parts & AudioMetadataReader::Tags))
        return true;

    // TAG+ continues the title, artist and album past ID3v1's 30 bytes, and has a free-text genre.
    QByteArray plus;
    if(src.size() >= kId3v1Size + kId3v1PlusSize) {
        plus = src.read(src.size() - kId3v1Size - kId3v1PlusSize, kId3v1PlusSize);
        if(!plus.startsWith("TAG+"))
            plus.clear();
    }
    QByteArray title = tag.mid(3, 30), artist = tag.mid(33, 30), album = tag.mid(63, 30);
    if(!plus.isEmpty()) {
        title += plus.mid(4, 60);
        artist += plus.mid(64, 60);
        album += plus.mid(124, 60);
    }
    addField(meta, Field::Title, {}, v1Text(title));
    addField(meta, Field::Artist, {}, v1Text(artist));
    addField(meta, Field::Album, {}, v1Text(album));
    addField(meta, Field::Date, {}, v1Text(tag.mid(93, 4)));

    QByteArray comment = tag.mid(97, 30);
    // ID3v1.1: a zero in the comment's 29th byte makes the 30th the track number.
    if(comment.at(28) == 0 && comment.at(29) != 0) {
        addField(meta, Field::Track, {}, QString::number(static_cast<quint8>(comment.at(29))));
        comment.truncate(28);
    }
    addField(meta, Field::Comment, {}, v1Text(comment));

    const QString plusGenre = plus.isEmpty() ? QString() : v1Text(plus.mid(185, 30));
    addField(meta, Field::Genre, {}, plusGenre.isEmpty() ? id3GenreName(static_cast<quint8>(tag.at(127))) : plusGenre);
    return true;
}

} // namespace AudioMeta
