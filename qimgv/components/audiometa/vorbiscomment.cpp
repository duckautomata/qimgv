#include "audiometa_p.h"

#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QSet>

namespace AudioMeta {

namespace {

// Vorbis comment keys are case-insensitive and may repeat (ARTIST=a, ARTIST=b is two artists), so the
// entries are grouped by upper-cased key before any is mapped: a field's values must all be known before
// they are joined, and the totals must be known before the track and disc numbers are written.
class Groups {
public:
    void add(QByteArray const &id, QString const &key, QString const &value) {
        const QString trimmed = value.trimmed();
        if(trimmed.isEmpty())
            return;
        auto it = mIndex.constFind(id);
        if(it == mIndex.constEnd()) {
            it = mIndex.insert(id, mGroups.size());
            mGroups.append({key, {}, {}});
        }
        // A set, not a scan of the list: thousands of long values under one key would take quadratic time.
        Group &group = mGroups[*it];
        if(!group.seen.contains(trimmed)) {
            group.seen.insert(trimmed);
            group.values.append(trimmed);
        }
    }

    // The joined values of the first of `ids` that is present; that group is used up. The other aliases
    // stay, so a second spelling with a different value is still listed under its own key.
    bool take(std::initializer_list<QByteArrayView> ids, QString &value) {
        for(QByteArrayView id : ids) {
            const auto it = mIndex.constFind(id.toByteArray());
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
                visit(mGroups.at(i).key, mGroups.at(i).values.join(QStringLiteral("; ")));
        }
    }

private:
    struct Group {
        QString key; // as first written
        QStringList values;
        QSet<QString> seen;
    };
    QList<Group> mGroups;
    QHash<QByteArray, qsizetype> mIndex;
    QSet<qsizetype> mUsed;
};

// "3" and a separate total of "12" make "3/12"; a number that already carries its total is left alone.
QString withTotal(QString number, Groups &groups, std::initializer_list<QByteArrayView> totalIds) {
    QString total;
    if(groups.take(totalIds, total) && !number.contains(QLatin1Char('/')) && !total.trimmed().isEmpty())
        number = number.trimmed() + QLatin1Char('/') + total.trimmed();
    return number;
}

bool isBase64Space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Pictures are base64 in the comment. Line-wrapped base64 turns up, so whitespace is dropped; anything else
// that is not base64 rejects the whole value rather than decoding into garbage.
QByteArray decodeBase64(QByteArrayView text, qint64 maxDecoded) {
    // Checked before decoding (and before copying), with room for the whitespace that is about to go.
    const qint64 maxEncoded = maxDecoded * 4 / 3 + 4;
    if(text.size() > maxEncoded * 2)
        return {};
    QByteArray compact;
    compact.reserve(text.size());
    for(char c : text) {
        if(!isBase64Space(c))
            compact.append(c);
    }
    if(compact.size() > maxEncoded)
        return {};
    auto result = QByteArray::fromBase64Encoding(std::move(compact), QByteArray::AbortOnBase64DecodingErrors);
    if(!result)
        return {};
    return std::move(result.decoded);
}

} // namespace

void parseVorbisComment(QByteArray const &data, AudioMetadata &meta, int parts) {
    const bool wantTags = parts & AudioMetadataReader::Tags;
    const bool wantCover = parts & AudioMetadataReader::Cover;
    if(!wantTags && !wantCover)
        return;
    const qint64 size = data.size();
    if(size < 8)
        return;
    const qint64 vendorLength = le32(data, 0);
    if(vendorLength > size - 8)
        return;
    qint64 pos = 4 + vendorLength;
    const quint32 count = le32(data, pos);
    pos += 4;
    addTagType(meta, QStringLiteral("Vorbis comment"));

    Groups groups;
    QList<QByteArrayView> legacyCovers;
    QByteArrayView legacyCoverMime;
    // Each entry takes at least its 4-byte length, so the walk ends with the data whatever the count says.
    for(quint32 i = 0; i < count && i < quint32(kMaxStructures) && size - pos >= 4; i++) {
        const qint64 length = le32(data, pos);
        pos += 4;
        if(length > size - pos)
            break;
        const QByteArrayView entry = QByteArrayView(data).sliced(pos, length);
        pos += length;
        const qsizetype eq = entry.indexOf('=');
        if(eq <= 0)
            continue;
        const QByteArray id = entry.first(eq).toByteArray().toUpper();
        const QByteArrayView value = entry.sliced(eq + 1);
        if(id == "METADATA_BLOCK_PICTURE") {
            if(wantCover)
                // A whole FLAC picture block: the picture and a few dozen bytes of header around it.
                parseFlacPicture(decodeBase64(value, kMaxPictureBytes + 1024), meta, parts);
        } else if(id == "COVERART") {
            if(wantCover)
                legacyCovers.append(value);
        } else if(id == "COVERARTMIME") {
            if(legacyCoverMime.isEmpty())
                legacyCoverMime = value;
        } else if(wantTags) {
            groups.add(id, QString::fromLatin1(entry.first(eq)), decodeText(value.toByteArray(), TextEncoding::Utf8));
        }
    }

    // The pre-METADATA_BLOCK_PICTURE convention: the bare image in base64, its type in COVERARTMIME.
    for(QByteArrayView encoded : std::as_const(legacyCovers)) {
        AudioPicture picture;
        picture.data = decodeBase64(encoded, kMaxPictureBytes);
        picture.mimeType = QString::fromLatin1(legacyCoverMime);
        offerPicture(meta, std::move(picture));
    }

    if(!wantTags)
        return;
    auto map = [&](Field field, std::initializer_list<QByteArrayView> ids) {
        QString value;
        if(!groups.take(ids, value))
            return;
        if(field == Field::Track)
            value = withTotal(value, groups, {"TRACKTOTAL", "TOTALTRACKS"});
        else if(field == Field::Disc)
            value = withTotal(value, groups, {"DISCTOTAL", "TOTALDISCS"});
        addField(meta, field, {}, value);
    };
    map(Field::Title, {"TITLE"});
    map(Field::Artist, {"ARTIST"});
    map(Field::Album, {"ALBUM"});
    map(Field::AlbumArtist, {"ALBUMARTIST", "ALBUM ARTIST", "ALBUM_ARTIST"});
    map(Field::Date, {"DATE", "YEAR"});
    map(Field::Genre, {"GENRE"});
    map(Field::Track, {"TRACKNUMBER"});
    map(Field::Disc, {"DISCNUMBER"});
    map(Field::Composer, {"COMPOSER"});
    map(Field::Comment, {"COMMENT", "DESCRIPTION"});
    groups.forEachUnused([&](QString const &key, QString const &value) { addField(meta, Field::None, key, value); });
}

} // namespace AudioMeta
