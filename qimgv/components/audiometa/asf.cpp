#include "audiometa_p.h"

#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QSet>
#include <QtEndian>

namespace AudioMeta {

namespace {

// A GUID as ASF stores it: the first three fields little-endian, the last eight bytes in order. Written
// here in the canonical form, so each one can be checked against the specification at a glance.
QByteArray guid(quint32 data1, quint16 data2, quint16 data3, quint64 data4) {
    QByteArray bytes(16, Qt::Uninitialized);
    qToLittleEndian(data1, bytes.data());
    qToLittleEndian(data2, bytes.data() + 4);
    qToLittleEndian(data3, bytes.data() + 6);
    qToBigEndian(data4, bytes.data() + 8);
    return bytes;
}

const QByteArray kHeaderObject = guid(0x75B22630, 0x668E, 0x11CF, 0xA6D900AA0062CE6C);
const QByteArray kContentDescription = guid(0x75B22633, 0x668E, 0x11CF, 0xA6D900AA0062CE6C);
const QByteArray kExtendedContentDescription = guid(0xD2D0A440, 0xE307, 0x11D2, 0x97F000A0C95EA850);
const QByteArray kHeaderExtension = guid(0x5FBF03B5, 0xA92E, 0x11CF, 0x8EE300C00C205365);
const QByteArray kMetadata = guid(0xC5F8CBEA, 0x5BAF, 0x4877, 0x8467AA8C44FA4CCA);
const QByteArray kMetadataLibrary = guid(0x44231C94, 0x9498, 0x49D1, 0xA1411D134E457054);

constexpr qint64 kObjectHeaderSize = 24; // GUID, 64-bit size
constexpr qint64 kHeaderObjectSize = 30; // plus a 32-bit object count and two reserved bytes

// Attribute value types.
constexpr quint16 kString = 0;
constexpr quint16 kBytes = 1;
constexpr quint16 kDword = 3;
constexpr quint16 kQword = 4;
constexpr quint16 kWord = 5;

// Walks the objects in `data` from `pos`, each a GUID and a 64-bit size that includes the 24-byte
// header. A size that does not fit what is left ends the walk.
template<typename Visit>
void forEachObject(QByteArray const &data, qint64 pos, int &budget, Visit &&visit) {
    while(budget > 0 && data.size() - pos >= kObjectHeaderSize) {
        const quint64 size = le64(data, pos + 16);
        if(size < quint64(kObjectHeaderSize) || size > quint64(data.size() - pos))
            return;
        budget--;
        visit(QByteArrayView(data).sliced(pos, 16),
              QByteArray::fromRawData(data.constData() + pos + kObjectHeaderSize, qsizetype(size) - kObjectHeaderSize));
        pos += qint64(size);
    }
}

// Every attribute, by name, before any is mapped: ffmpeg writes the title into both description objects,
// a name may repeat in the Metadata Library, and WM/Track only counts when WM/TrackNumber is missing.
class Attributes {
public:
    void add(QString const &name, QString const &value) {
        const QString trimmed = value.trimmed();
        if(trimmed.isEmpty())
            return;
        const QString id = name.toLower();
        auto it = mIndex.constFind(id);
        if(it == mIndex.constEnd()) {
            it = mIndex.insert(id, mGroups.size());
            mGroups.append({name, {}, {}});
        }
        // A set, not a scan of the list: thousands of long values under one name would take quadratic time.
        Group &group = mGroups[*it];
        if(!group.seen.contains(trimmed)) {
            group.seen.insert(trimmed);
            group.values.append(trimmed);
        }
    }

    // The joined values of the first of `names` present, which is then used up.
    bool take(std::initializer_list<QStringView> names, QString &value) {
        for(QStringView name : names) {
            const auto it = mIndex.constFind(name.toString().toLower());
            if(it == mIndex.constEnd() || mUsed.contains(*it))
                continue;
            mUsed.insert(*it);
            value = mGroups.at(*it).values.join(QStringLiteral("; "));
            return true;
        }
        return false;
    }

    template<typename F>
    void forEachUnused(F &&visit) const {
        for(qsizetype i = 0; i < mGroups.size(); i++) {
            if(!mUsed.contains(i))
                visit(mGroups.at(i).name, mGroups.at(i).values.join(QStringLiteral("; ")));
        }
    }

private:
    struct Group {
        QString name; // as first written
        QStringList values;
        QSet<QString> seen;
    };
    QList<Group> mGroups;
    QHash<QString, qsizetype> mIndex;
    QSet<qsizetype> mUsed;
};

class Reader {
public:
    Reader(AudioMetadata &meta, int parts)
        : mMeta(meta), mWantTags(parts & AudioMetadataReader::Tags), mWantCover(parts & AudioMetadataReader::Cover) {}

    // Five 16-bit lengths, then that many bytes of UTF-16 each.
    void contentDescription(QByteArray const &body) {
        if(body.size() < 10)
            return;
        mFound = true;
        if(!mWantTags)
            return;
        static char const *const names[] = {"Title", "Author", "Copyright", "Description", "Rating"};
        qint64 pos = 10;
        for(int i = 0; i < 5; i++) {
            const qint64 length = le16(body, i * 2);
            if(length > body.size() - pos)
                return;
            add(QString::fromLatin1(names[i]), kString, QByteArrayView(body).sliced(pos, length));
            pos += length;
        }
    }

    // A count, then: 16-bit name length, UTF-16 name, 16-bit type, 16-bit value length, value. Each
    // attribute comes out of `budget`, like the objects: a file may hold any number of these objects, each
    // counting up to 65535 attributes, and every attribute can become a field.
    void extendedContentDescription(QByteArray const &body, int &budget) {
        if(body.size() < 2)
            return;
        mFound = true;
        const int count = le16(body, 0);
        qint64 pos = 2;
        for(int i = 0; i < count && budget > 0 && body.size() - pos >= 2; i++) {
            budget--;
            const qint64 nameLength = le16(body, pos);
            pos += 2;
            if(nameLength > body.size() - pos)
                return;
            const QString name = decodeText(body.mid(pos, nameLength), TextEncoding::Utf16LE);
            pos += nameLength;
            if(body.size() - pos < 4)
                return;
            const quint16 type = le16(body, pos);
            const qint64 valueLength = le16(body, pos + 2);
            pos += 4;
            if(valueLength > body.size() - pos)
                return;
            add(name, type, QByteArrayView(body).sliced(pos, valueLength));
            pos += valueLength;
        }
    }

    // A reserved GUID and word, then the size of the objects that follow: Metadata and Metadata Library
    // among them.
    void headerExtension(QByteArray const &body, int &budget) {
        constexpr qint64 kFixedSize = 16 + 2 + 4; // reserved GUID, reserved word, data size
        if(body.size() < kFixedSize)
            return;
        const qint64 dataSize = le32(body, 18);
        if(dataSize > body.size() - kFixedSize)
            return;
        const QByteArray objects = QByteArray::fromRawData(body.constData() + kFixedSize, dataSize);
        forEachObject(objects, 0, budget, [&](QByteArrayView id, QByteArray const &object) {
            if(id == kMetadata || id == kMetadataLibrary)
                metadataRecords(object, budget);
        });
    }

    // Metadata and Metadata Library records share a layout: language index, stream number, name length,
    // type, 32-bit data length, UTF-16 name, data. Out of `budget`, as the attributes above.
    void metadataRecords(QByteArray const &body, int &budget) {
        if(body.size() < 2)
            return;
        mFound = true;
        const int count = le16(body, 0);
        qint64 pos = 2;
        for(int i = 0; i < count && budget > 0 && body.size() - pos >= 12; i++) {
            budget--;
            const qint64 nameLength = le16(body, pos + 4);
            const quint16 type = le16(body, pos + 6);
            const qint64 dataLength = le32(body, pos + 8);
            pos += 12;
            if(nameLength > body.size() - pos)
                return;
            const QString name = decodeText(body.mid(pos, nameLength), TextEncoding::Utf16LE);
            pos += nameLength;
            if(dataLength > body.size() - pos)
                return;
            add(name, type, QByteArrayView(body).sliced(pos, dataLength));
            pos += dataLength;
        }
    }

    // Maps what was collected onto the fields, then lists the rest under their own names.
    void finish() {
        if(!mFound)
            return;
        addTagType(mMeta, QStringLiteral("ASF"));
        QString value;
        auto map = [&](Field field, std::initializer_list<QStringView> names) {
            if(mAttributes.take(names, value))
                addField(mMeta, field, {}, value);
        };
        map(Field::Title, {u"Title"});
        map(Field::Artist, {u"Author"});
        map(Field::Album, {u"WM/AlbumTitle"});
        map(Field::AlbumArtist, {u"WM/AlbumArtist"});
        // "date" is what ffmpeg writes, having no mapping of its own for WM/Year.
        map(Field::Date, {u"WM/Year", u"date", u"Year"});
        map(Field::Genre, {u"WM/Genre"});
        if(mAttributes.take({u"WM/GenreID"}, value))
            addField(mMeta, Field::Genre, {}, genreFromId(value));
        // WM/TrackNumber counts from 1; the older WM/Track from 0.
        map(Field::Track, {u"WM/TrackNumber"});
        if(mAttributes.take({u"WM/Track"}, value)) {
            bool ok = false;
            const qint64 track = value.toLongLong(&ok);
            addField(mMeta, Field::Track, {}, ok && track >= 0 ? QString::number(track + 1) : value);
        }
        map(Field::Disc, {u"WM/PartOfSet"});
        map(Field::Composer, {u"WM/Composer"});
        map(Field::Comment, {u"Description"});
        mAttributes.forEachUnused(
            [&](QString const &name, QString const &text) { addField(mMeta, Field::None, name, text); });
    }

private:
    void add(QString const &name, quint16 type, QByteArrayView value) {
        if(name.compare(QLatin1String("WM/Picture"), Qt::CaseInsensitive) == 0) {
            if(mWantCover && type == kBytes)
                picture(value.toByteArray());
            return;
        }
        if(mWantTags)
            mAttributes.add(name, valueText(type, value));
    }

    // Strings as they are, integers in decimal. Bytes, GUIDs and booleans are nothing to show.
    static QString valueText(quint16 type, QByteArrayView value) {
        const QByteArray bytes = QByteArray::fromRawData(value.data(), value.size());
        switch(type) {
        case kString:
            return decodeText(bytes, TextEncoding::Utf16LE);
        case kDword:
            return bytes.size() == 4 ? QString::number(le32(bytes, 0)) : QString();
        case kQword:
            return bytes.size() == 8 ? QString::number(le64(bytes, 0)) : QString();
        case kWord:
            return bytes.size() == 2 ? QString::number(le16(bytes, 0)) : QString();
        }
        return {};
    }

    // WM/Picture: picture type byte, 32-bit data length, NUL-terminated UTF-16 MIME type and
    // description, then the data.
    void picture(QByteArray const &value) {
        if(value.size() < 5)
            return;
        const quint8 type = quint8(value.at(0));
        const qint64 length = le32(value, 1);
        qsizetype pos = 5;
        const QString mime = readTerminated(value, pos, TextEncoding::Utf16LE);
        const QString description = readTerminated(value, pos, TextEncoding::Utf16LE);
        if(length > value.size() - pos || length > kMaxPictureBytes)
            return;
        AudioPicture picture;
        picture.data = value.mid(pos, length);
        picture.mimeType = mime;
        picture.type = type <= 20 ? type : -1;
        picture.description = description;
        offerPicture(mMeta, std::move(picture));
    }

    // WM/GenreID is "(n)" or a bare number indexing the ID3v1 list, or else the name itself.
    static QString genreFromId(QString const &value) {
        QString number = value.trimmed();
        if(number.startsWith(QLatin1Char('(')) && number.endsWith(QLatin1Char(')')))
            number = number.mid(1, number.size() - 2);
        bool ok = false;
        const int index = number.toInt(&ok);
        const QString name = ok ? id3GenreName(index) : QString();
        return name.isEmpty() ? value : name;
    }

    AudioMetadata &mMeta;
    bool mWantTags;
    bool mWantCover;
    bool mFound = false;
    Attributes mAttributes;
};

} // namespace

bool parseAsf(ByteSource const &src, AudioMetadata &meta, int parts) {
    if(!(parts & AudioMetadataReader::All))
        return false;
    const QByteArray header = src.read(0, kHeaderObjectSize);
    if(header.size() != kHeaderObjectSize || !header.startsWith(kHeaderObject))
        return false;
    const quint64 headerSize = le64(header, 16);
    if(headerSize < quint64(kHeaderObjectSize) || headerSize > quint64(src.size()))
        return false;

    // The header object's children are walked in the file, and only the ones that hold metadata are read
    // whole -- each bounded by kMaxBlockBytes, and all of them together by one allowance, since the header
    // may be as large as the file. The object count is not trusted; the sizes decide.
    Reader reader(meta, parts);
    int budget = kMaxStructures;
    qint64 allowance = kMaxBlockBytes;
    const qint64 end = qint64(headerSize);
    qint64 pos = kHeaderObjectSize;
    while(budget > 0 && end - pos >= kObjectHeaderSize) {
        const QByteArray objectHeader = src.read(pos, kObjectHeaderSize);
        const quint64 size = le64(objectHeader, 16);
        if(objectHeader.size() != kObjectHeaderSize || size < quint64(kObjectHeaderSize) || size > quint64(end - pos))
            break;
        budget--;
        const QByteArrayView id = QByteArrayView(objectHeader).first(16);
        const qint64 bodySize = qint64(size) - kObjectHeaderSize;
        auto body = [&] {
            if(bodySize > allowance)
                return QByteArray();
            allowance -= bodySize;
            return src.read(pos + kObjectHeaderSize, bodySize);
        };
        if(id == kContentDescription)
            reader.contentDescription(body());
        else if(id == kExtendedContentDescription)
            reader.extendedContentDescription(body(), budget);
        else if(id == kHeaderExtension)
            reader.headerExtension(body(), budget);
        pos += qint64(size);
    }
    reader.finish();
    return true;
}

} // namespace AudioMeta
