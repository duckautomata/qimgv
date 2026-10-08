#include "audiometa_p.h"

#include <QCoreApplication>
#include <QStringDecoder>

namespace AudioMeta {

ByteSource::ByteSource(QString const &path) : mFile(path) {
    if(mFile.open(QIODevice::ReadOnly)) {
        mSize = mFile.size();
        mValid = true;
    }
}

ByteSource::ByteSource(QByteArray data) : mData(std::move(data)), mInMemory(true), mValid(true) {
    mSize = mData.size();
}

QByteArray ByteSource::read(qint64 offset, qint64 length) const {
    // Written so that no sum can overflow: offset and length are each checked against the size before
    // they are combined, and the comparison is a subtraction from a value known to be >= offset.
    if(!mValid || offset < 0 || length < 0 || length > kMaxBlockBytes || offset > mSize || length > mSize - offset)
        return {};
    if(length == 0)
        return {};
    if(mInMemory)
        return mData.mid(static_cast<qsizetype>(offset), static_cast<qsizetype>(length));
    if(!mFile.seek(offset))
        return {};
    QByteArray result = mFile.read(length);
    if(result.size() != length)
        return {};
    return result;
}

static inline quint8 byteAt(QByteArray const &d, qsizetype at) {
    return static_cast<quint8>(d.at(at));
}

static inline bool fits(QByteArray const &d, qsizetype at, qsizetype n) {
    return at >= 0 && n >= 0 && at <= d.size() && n <= d.size() - at;
}

quint16 be16(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 2))
        return 0;
    return static_cast<quint16>((byteAt(d, at) << 8) | byteAt(d, at + 1));
}

quint32 be24(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 3))
        return 0;
    return (quint32(byteAt(d, at)) << 16) | (quint32(byteAt(d, at + 1)) << 8) | quint32(byteAt(d, at + 2));
}

quint32 be32(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 4))
        return 0;
    return (quint32(byteAt(d, at)) << 24) | (quint32(byteAt(d, at + 1)) << 16) | (quint32(byteAt(d, at + 2)) << 8) |
           quint32(byteAt(d, at + 3));
}

quint64 be64(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 8))
        return 0;
    return (quint64(be32(d, at)) << 32) | quint64(be32(d, at + 4));
}

quint16 le16(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 2))
        return 0;
    return static_cast<quint16>(byteAt(d, at) | (byteAt(d, at + 1) << 8));
}

quint32 le32(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 4))
        return 0;
    return quint32(byteAt(d, at)) | (quint32(byteAt(d, at + 1)) << 8) | (quint32(byteAt(d, at + 2)) << 16) |
           (quint32(byteAt(d, at + 3)) << 24);
}

quint64 le64(QByteArray const &d, qsizetype at) {
    if(!fits(d, at, 8))
        return 0;
    return quint64(le32(d, at)) | (quint64(le32(d, at + 4)) << 32);
}

bool syncsafe32(QByteArray const &d, qsizetype at, quint32 &out) {
    if(!fits(d, at, 4))
        return false;
    quint32 value = 0;
    for(int i = 0; i < 4; i++) {
        const quint8 b = byteAt(d, at + i);
        if(b & 0x80)
            return false;
        value = (value << 7) | b;
    }
    out = value;
    return true;
}

static QString stripTrailingNuls(QString s) {
    qsizetype end = s.size();
    while(end > 0 && s.at(end - 1) == QChar(0))
        --end;
    s.truncate(end);
    return s;
}

QString decodeText(QByteArray const &bytes, TextEncoding encoding) {
    if(bytes.isEmpty())
        return {};
    switch(encoding) {
    case TextEncoding::Latin1:
        return stripTrailingNuls(QString::fromLatin1(bytes));
    case TextEncoding::Utf8:
        return stripTrailingNuls(QString::fromUtf8(bytes));
    case TextEncoding::Utf16BE: {
        QStringDecoder decoder(QStringDecoder::Utf16BE, QStringDecoder::Flag::Stateless);
        return stripTrailingNuls(decoder.decode(bytes));
    }
    case TextEncoding::Utf16LE: {
        QStringDecoder decoder(QStringDecoder::Utf16LE, QStringDecoder::Flag::Stateless);
        return stripTrailingNuls(decoder.decode(bytes));
    }
    case TextEncoding::Utf16Bom: {
        // A BOM picks the byte order and is not part of the text. Without one, little-endian: that is what
        // the Windows tools that write BOM-less UTF-16 produce.
        if(bytes.size() >= 2 && byteAt(bytes, 0) == 0xFE && byteAt(bytes, 1) == 0xFF)
            return decodeText(bytes.mid(2), TextEncoding::Utf16BE);
        if(bytes.size() >= 2 && byteAt(bytes, 0) == 0xFF && byteAt(bytes, 1) == 0xFE)
            return decodeText(bytes.mid(2), TextEncoding::Utf16LE);
        return decodeText(bytes, TextEncoding::Utf16LE);
    }
    }
    return {};
}

QString readTerminated(QByteArray const &data, qsizetype &pos, TextEncoding encoding) {
    if(pos < 0 || pos >= data.size()) {
        pos = data.size();
        return {};
    }
    const bool wide =
        encoding == TextEncoding::Utf16Bom || encoding == TextEncoding::Utf16BE || encoding == TextEncoding::Utf16LE;
    qsizetype end = -1;
    qsizetype next = data.size();
    if(wide) {
        for(qsizetype i = pos; i + 1 < data.size(); i += 2) {
            if(data.at(i) == 0 && data.at(i + 1) == 0) {
                end = i;
                next = i + 2;
                break;
            }
        }
    } else {
        end = data.indexOf('\0', pos);
        if(end != -1)
            next = end + 1;
    }
    if(end == -1)
        end = data.size();
    const QString text = decodeText(data.mid(pos, end - pos), encoding);
    pos = next;
    return text;
}

QString id3GenreName(int index) {
    static const char *const genres[] = {"Blues",
                                         "Classic Rock",
                                         "Country",
                                         "Dance",
                                         "Disco",
                                         "Funk",
                                         "Grunge",
                                         "Hip-Hop",
                                         "Jazz",
                                         "Metal",
                                         "New Age",
                                         "Oldies",
                                         "Other",
                                         "Pop",
                                         "R&B",
                                         "Rap",
                                         "Reggae",
                                         "Rock",
                                         "Techno",
                                         "Industrial",
                                         "Alternative",
                                         "Ska",
                                         "Death Metal",
                                         "Pranks",
                                         "Soundtrack",
                                         "Euro-Techno",
                                         "Ambient",
                                         "Trip-Hop",
                                         "Vocal",
                                         "Jazz+Funk",
                                         "Fusion",
                                         "Trance",
                                         "Classical",
                                         "Instrumental",
                                         "Acid",
                                         "House",
                                         "Game",
                                         "Sound Clip",
                                         "Gospel",
                                         "Noise",
                                         "Alternative Rock",
                                         "Bass",
                                         "Soul",
                                         "Punk",
                                         "Space",
                                         "Meditative",
                                         "Instrumental Pop",
                                         "Instrumental Rock",
                                         "Ethnic",
                                         "Gothic",
                                         "Darkwave",
                                         "Techno-Industrial",
                                         "Electronic",
                                         "Pop-Folk",
                                         "Eurodance",
                                         "Dream",
                                         "Southern Rock",
                                         "Comedy",
                                         "Cult",
                                         "Gangsta",
                                         "Top 40",
                                         "Christian Rap",
                                         "Pop/Funk",
                                         "Jungle",
                                         "Native American",
                                         "Cabaret",
                                         "New Wave",
                                         "Psychedelic",
                                         "Rave",
                                         "Showtunes",
                                         "Trailer",
                                         "Lo-Fi",
                                         "Tribal",
                                         "Acid Punk",
                                         "Acid Jazz",
                                         "Polka",
                                         "Retro",
                                         "Musical",
                                         "Rock & Roll",
                                         "Hard Rock",
                                         "Folk",
                                         "Folk-Rock",
                                         "National Folk",
                                         "Swing",
                                         "Fast Fusion",
                                         "Bebop",
                                         "Latin",
                                         "Revival",
                                         "Celtic",
                                         "Bluegrass",
                                         "Avantgarde",
                                         "Gothic Rock",
                                         "Progressive Rock",
                                         "Psychedelic Rock",
                                         "Symphonic Rock",
                                         "Slow Rock",
                                         "Big Band",
                                         "Chorus",
                                         "Easy Listening",
                                         "Acoustic",
                                         "Humour",
                                         "Speech",
                                         "Chanson",
                                         "Opera",
                                         "Chamber Music",
                                         "Sonata",
                                         "Symphony",
                                         "Booty Bass",
                                         "Primus",
                                         "Porn Groove",
                                         "Satire",
                                         "Slow Jam",
                                         "Club",
                                         "Tango",
                                         "Samba",
                                         "Folklore",
                                         "Ballad",
                                         "Power Ballad",
                                         "Rhythmic Soul",
                                         "Freestyle",
                                         "Duet",
                                         "Punk Rock",
                                         "Drum Solo",
                                         "A Cappella",
                                         "Euro-House",
                                         "Dance Hall",
                                         "Goa",
                                         "Drum & Bass",
                                         "Club-House",
                                         "Hardcore Techno",
                                         "Terror",
                                         "Indie",
                                         "BritPop",
                                         "Worldbeat",
                                         "Polsk Punk",
                                         "Beat",
                                         "Christian Gangsta Rap",
                                         "Heavy Metal",
                                         "Black Metal",
                                         "Crossover",
                                         "Contemporary Christian",
                                         "Christian Rock",
                                         "Merengue",
                                         "Salsa",
                                         "Thrash Metal",
                                         "Anime",
                                         "Jpop",
                                         "Synthpop",
                                         "Abstract",
                                         "Art Rock",
                                         "Baroque",
                                         "Bhangra",
                                         "Big Beat",
                                         "Breakbeat",
                                         "Chillout",
                                         "Downtempo",
                                         "Dub",
                                         "EBM",
                                         "Eclectic",
                                         "Electro",
                                         "Electroclash",
                                         "Emo",
                                         "Experimental",
                                         "Garage",
                                         "Global",
                                         "IDM",
                                         "Illbient",
                                         "Industro-Goth",
                                         "Jam Band",
                                         "Krautrock",
                                         "Leftfield",
                                         "Lounge",
                                         "Math Rock",
                                         "New Romantic",
                                         "Nu-Breakz",
                                         "Post-Punk",
                                         "Post-Rock",
                                         "Psytrance",
                                         "Shoegaze",
                                         "Space Rock",
                                         "Trop Rock",
                                         "World Music",
                                         "Neoclassical",
                                         "Audiobook",
                                         "Audio Theatre",
                                         "Neue Deutsche Welle",
                                         "Podcast",
                                         "Indie Rock",
                                         "G-Funk",
                                         "Dubstep",
                                         "Garage Rock",
                                         "Psybient"};
    constexpr int count = int(sizeof(genres) / sizeof(genres[0]));
    static_assert(count == 192, "the ID3v1 genre list has 192 entries");
    if(index < 0 || index >= count)
        return {};
    return QString::fromLatin1(genres[index]);
}

QString fieldName(Field field) {
    switch(field) {
    case Field::Title:
        return QCoreApplication::translate("AudioMetadata", "Title");
    case Field::Artist:
        return QCoreApplication::translate("AudioMetadata", "Artist");
    case Field::Album:
        return QCoreApplication::translate("AudioMetadata", "Album");
    case Field::AlbumArtist:
        return QCoreApplication::translate("AudioMetadata", "Album artist");
    case Field::Date:
        return QCoreApplication::translate("AudioMetadata", "Date");
    case Field::Genre:
        return QCoreApplication::translate("AudioMetadata", "Genre");
    case Field::Track:
        return QCoreApplication::translate("AudioMetadata", "Track");
    case Field::Disc:
        return QCoreApplication::translate("AudioMetadata", "Disc");
    case Field::Composer:
        return QCoreApplication::translate("AudioMetadata", "Composer");
    case Field::Comment:
        return QCoreApplication::translate("AudioMetadata", "Comment");
    case Field::None:
        break;
    }
    return {};
}

static QString *memberFor(AudioMetadata &meta, Field field) {
    switch(field) {
    case Field::Title:
        return &meta.title;
    case Field::Artist:
        return &meta.artist;
    case Field::Album:
        return &meta.album;
    case Field::AlbumArtist:
        return &meta.albumArtist;
    case Field::Date:
        return &meta.date;
    case Field::Genre:
        return &meta.genre;
    case Field::Track:
        return &meta.track;
    case Field::Disc:
        return &meta.disc;
    case Field::Composer:
        return &meta.composer;
    case Field::Comment:
        return &meta.comment;
    case Field::None:
        break;
    }
    return nullptr;
}

void addField(AudioMetadata &meta, Field field, QString const &key, QString const &value) {
    const QString trimmed = value.trimmed();
    if(trimmed.isEmpty())
        return;
    if(QString *member = memberFor(meta, field)) {
        if(!member->isEmpty())
            return;
        *member = trimmed;
        meta.fields.append({fieldName(field), trimmed, true});
        return;
    }
    const QString name = key.trimmed();
    if(name.isEmpty() || meta.fields.size() >= kMaxFields)
        return;
    // Hashed rather than a scan of `fields`: a few stacked tags of thousands of fields each made the scan
    // quadratic, seconds for a sub-megabyte file. The strings are shared with `fields`, not copied.
    if(meta.fieldIndex.contains({name, trimmed}))
        return;
    meta.fieldIndex.insert({name, trimmed});
    meta.fields.append({name, trimmed});
}

void offerPicture(AudioMetadata &meta, AudioPicture picture) {
    if(picture.data.isEmpty() || picture.data.size() > kMaxPictureBytes)
        return;
    if(meta.cover.isNull() || (picture.type == 3 && meta.cover.type != 3))
        meta.cover = std::move(picture);
}

void addTagType(AudioMetadata &meta, QString const &type) {
    if(!type.isEmpty() && !meta.tagTypes.contains(type))
        meta.tagTypes.append(type);
}

} // namespace AudioMeta
