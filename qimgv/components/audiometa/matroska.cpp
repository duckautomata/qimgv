#include "audiometa_p.h"

#include <QSet>
#include <algorithm>

// Matroska and WebM (RFC 9559; tag levels from matroska.org's "Tagging" page). What the reader wants sits at
// the top level of the Segment: Info for the title, Tags, and Attachments for the cover. Muxers often write
// Tags and Attachments after the media, so they are reached through the SeekHead instead of walking every
// Cluster to get there.

namespace AudioMeta {

namespace {

using namespace Qt::StringLiterals;

// Element IDs, with their length-marker bits, as the specification writes them.
constexpr quint32 kEbml = 0x1A45DFA3;
constexpr quint32 kDocType = 0x4282;
constexpr quint32 kSegment = 0x18538067;
constexpr quint32 kSeekHead = 0x114D9B74;
constexpr quint32 kSeek = 0x4DBB;
constexpr quint32 kSeekId = 0x53AB;
constexpr quint32 kSeekPosition = 0x53AC;
constexpr quint32 kInfo = 0x1549A966;
constexpr quint32 kTitle = 0x7BA9;
constexpr quint32 kCluster = 0x1F43B675;
constexpr quint32 kTags = 0x1254C367;
constexpr quint32 kTag = 0x7373;
constexpr quint32 kTargets = 0x63C0;
constexpr quint32 kTargetTypeValue = 0x68CA;
constexpr quint32 kTagEditionUid = 0x63C9;
constexpr quint32 kTagChapterUid = 0x63C4;
constexpr quint32 kTagAttachmentUid = 0x63C6;
constexpr quint32 kSimpleTag = 0x67C8;
constexpr quint32 kTagName = 0x45A3;
constexpr quint32 kTagString = 0x4487;
constexpr quint32 kAttachments = 0x1941A469;
constexpr quint32 kAttachedFile = 0x61A7;
constexpr quint32 kFileDescription = 0x467E;
constexpr quint32 kFileName = 0x466E;
constexpr quint32 kFileMimeType = 0x4660;
constexpr quint32 kFileData = 0x465C;

// A 4-byte ID and an 8-byte size.
constexpr qint64 kMaxHeaderBytes = 12;
constexpr qint64 kMaxEbmlHeaderBytes = 4096;
// SeekHead, Info and Tags are read whole; together they stay far below this in any real file.
constexpr qint64 kMaxMetadataBytes = 16 * 1024 * 1024;
// An attachment's name, MIME type or description.
constexpr qint64 kMaxNameBytes = 4096;
// Tag names are short identifiers. A nested tag's name is its whole path, so a long one would be copied into
// every tag below it.
constexpr qint64 kMaxTagNameBytes = 256;
// Real files have one or two SeekHeads and one each of Info, Tags and Attachments.
constexpr int kMaxSeekTargets = 64;
constexpr int kMaxSegmentSearch = 16;
constexpr int kMaxTagDepth = 8;

constexpr int kTrackLevel = 30;
constexpr int kAlbumLevel = 50;
constexpr int kEditionLevel = 60;
constexpr int kAnyLevel = -1;

struct Element {
    quint32 id = 0;
    qint64 offset = 0; // of the header
    qint64 data = 0;
    qint64 size = 0; // -1: unknown, runs to the end of its parent
};

// An element header in `buf` at `pos`: an ID of 1-4 bytes whose leading zeros give its length, then a size of
// 1-8 bytes coded the same way. All value bits set means the size is unknown.
bool parseHeader(QByteArray const &buf, qsizetype pos, Element &e) {
    if(pos < 0 || pos >= buf.size())
        return false;
    const quint8 first = static_cast<quint8>(buf.at(pos));
    const int idLength = first >= 0x80 ? 1 : first >= 0x40 ? 2 : first >= 0x20 ? 3 : first >= 0x10 ? 4 : 0;
    if(idLength == 0 || buf.size() - pos <= idLength)
        return false;
    quint32 id = 0;
    for(int i = 0; i < idLength; i++)
        id = (id << 8) | static_cast<quint8>(buf.at(pos + i));
    const qsizetype at = pos + idLength;
    const quint8 lead = static_cast<quint8>(buf.at(at));
    if(lead == 0)
        return false;
    int sizeLength = 1;
    quint8 marker = 0x80;
    while(!(lead & marker)) {
        sizeLength++;
        marker >>= 1;
    }
    if(buf.size() - at < sizeLength)
        return false;
    quint64 size = lead & (marker - 1);
    bool allOnes = size == quint64(marker - 1);
    for(int i = 1; i < sizeLength; i++) {
        const quint8 b = static_cast<quint8>(buf.at(at + i));
        size = (size << 8) | b;
        allOnes = allOnes && b == 0xFF;
    }
    e.id = id;
    e.offset = pos;
    e.data = at + sizeLength;
    // At most 56 value bits, so it fits.
    e.size = allOnes ? -1 : qint64(size);
    return true;
}

quint64 readUint(QByteArray const &buf, qsizetype pos, qsizetype size) {
    if(size < 0 || size > 8 || pos < 0 || size > buf.size() - pos)
        return 0;
    quint64 value = 0;
    for(qsizetype i = 0; i < size; i++)
        value = (value << 8) | static_cast<quint8>(buf.at(pos + i));
    return value;
}

QString readString(QByteArray const &buf, qsizetype pos, qsizetype size) {
    return decodeText(buf.mid(pos, size), TextEncoding::Utf8).trimmed();
}

bool isImageName(QString const &lowerName) {
    static const char *const suffixes[] = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif"};
    for(const char *suffix : suffixes) {
        if(lowerName.endsWith(QLatin1StringView(suffix)))
            return true;
    }
    return false;
}

struct SimpleTag {
    int level;
    QString name;  // as written, for display
    QString upper; // for matching
    QString value;
};

struct CoverCandidate {
    int rank = 4; // lower is better; 4: none
    qint64 offset = 0;
    qint64 size = 0;
    QString mimeType;
    QString description;
};

class MatroskaReader {
public:
    MatroskaReader(ByteSource const &src, int parts) : mSrc(src), mParts(parts) {}

    bool read();
    void apply(AudioMetadata &meta) const;

private:
    template<typename F>
    void forEachChild(QByteArray const &buf, qsizetype begin, qsizetype end, F &&f);
    bool readElement(qint64 pos, Element &e) const;
    QByteArray readWhole(Element const &e);
    bool findSegment();
    void visit(Element const &e, int seekDepth);
    void readSeekHead(QByteArray const &buf, int depth);
    void readTags(QByteArray const &buf);
    void readSimpleTag(QByteArray const &buf, qsizetype begin, qsizetype end, QString const &parent, int depth,
                       QList<SimpleTag> &out);
    void readAttachments(Element const &attachments);
    void readAttachedFile(qint64 begin, qint64 end);

    QString joined(QString const &name, int level) const;
    void applyTags(AudioMetadata &meta) const;

    struct SeekTarget {
        quint32 id;
        qint64 position;
        int depth;
    };

    ByteSource const &mSrc;
    const int mParts;
    qint64 mSegmentData = 0;
    qint64 mSegmentEnd = 0;
    int mElements = 0;
    qint64 mBytesRead = 0;
    QSet<qint64> mVisited;
    QList<SeekTarget> mSeekTargets;
    QString mInfoTitle;
    QList<SimpleTag> mTags;
    CoverCandidate mCover;
};

// Calls f(element) for each child of buf[begin, end). A child that claims more than its parent has is cut to
// fit, which keeps what a truncated file still holds.
template<typename F>
void MatroskaReader::forEachChild(QByteArray const &buf, qsizetype begin, qsizetype end, F &&f) {
    qsizetype pos = begin;
    while(pos < end && mElements < kMaxStructures) {
        mElements++;
        Element e;
        if(!parseHeader(buf, pos, e) || e.data > end)
            return;
        if(e.size < 0 || e.size > end - e.data)
            e.size = end - e.data;
        f(e);
        pos = static_cast<qsizetype>(e.data + e.size);
    }
}

bool MatroskaReader::readElement(qint64 pos, Element &e) const {
    if(pos < 0 || pos >= mSrc.size())
        return false;
    const QByteArray buf = mSrc.read(pos, std::min(kMaxHeaderBytes, mSrc.size() - pos));
    if(!parseHeader(buf, 0, e))
        return false;
    e.offset = pos;
    e.data += pos;
    return true;
}

// The element's data, or as much of it as the file holds. Empty when over the limits.
QByteArray MatroskaReader::readWhole(Element const &e) {
    const qint64 size = std::min(e.size, mSrc.size() - e.data);
    if(e.size < 0 || size <= 0 || size > kMaxMetadataBytes || size > kMaxBlockBytes - mBytesRead)
        return {};
    mBytesRead += size;
    return mSrc.read(e.data, size);
}

bool MatroskaReader::findSegment() {
    Element header;
    if(!readElement(0, header) || header.id != kEbml || header.size < 0 || header.size > kMaxEbmlHeaderBytes)
        return false;
    const QByteArray buf = mSrc.read(header.data, header.size);
    if(buf.size() != header.size)
        return false;
    // DocType defaults to "matroska" when absent.
    QByteArray docType = "matroska";
    forEachChild(buf, 0, buf.size(), [&](Element const &e) {
        if(e.id == kDocType)
            docType = buf.mid(e.data, e.size);
    });
    while(docType.endsWith('\0'))
        docType.chop(1);
    if(docType != "matroska" && docType != "webm")
        return false;

    qint64 pos = header.data + header.size;
    for(int i = 0; i < kMaxSegmentSearch; i++) {
        Element e;
        if(!readElement(pos, e))
            return false;
        if(e.id == kSegment) {
            mSegmentData = e.data;
            // An unknown size (a live recording) or one past the end (a truncated file) both mean "to EOF".
            mSegmentEnd = e.size < 0 || e.size > mSrc.size() - e.data ? mSrc.size() : e.data + e.size;
            return true;
        }
        // Void and other padding between the header and the Segment.
        if(e.size < 0)
            return false;
        pos = e.data + e.size;
    }
    return false;
}

bool MatroskaReader::read() {
    if(!findSegment())
        return false;
    qint64 pos = mSegmentData;
    while(pos < mSegmentEnd && mElements < kMaxStructures) {
        mElements++;
        Element e;
        // Past the first Cluster is media; what follows the Clusters is found through the SeekHead. Only a
        // Cluster may have an unknown size at this level, and it has to end the walk anyway.
        if(!readElement(pos, e) || e.id == kCluster || e.size < 0)
            break;
        visit(e, 0);
        if(e.size > mSegmentEnd - e.data)
            break;
        pos = e.data + e.size;
    }
    // A second SeekHead found through the first adds its targets to the end of the list.
    for(qsizetype i = 0; i < mSeekTargets.size() && i < kMaxSeekTargets; i++) {
        const SeekTarget target = mSeekTargets.at(i);
        // Relative to the start of the Segment's data.
        if(target.position < 0 || target.position >= mSegmentEnd - mSegmentData)
            continue;
        Element e;
        if(readElement(mSegmentData + target.position, e) && e.id == target.id)
            visit(e, target.depth);
    }
    return true;
}

void MatroskaReader::visit(Element const &e, int seekDepth) {
    if(mVisited.contains(e.offset))
        return;
    mVisited.insert(e.offset);
    const bool tags = mParts & AudioMetadataReader::Tags;
    switch(e.id) {
    case kSeekHead:
        // The first SeekHead may list a second one; that is as deep as real files go.
        if(seekDepth < 2)
            readSeekHead(readWhole(e), seekDepth + 1);
        break;
    case kInfo:
        if(tags) {
            const QByteArray buf = readWhole(e);
            forEachChild(buf, 0, buf.size(), [&](Element const &child) {
                if(child.id == kTitle)
                    mInfoTitle = readString(buf, child.data, child.size);
            });
        }
        break;
    case kTags:
        if(tags)
            readTags(readWhole(e));
        break;
    case kAttachments:
        if(mParts & AudioMetadataReader::Cover)
            readAttachments(e);
        break;
    }
}

void MatroskaReader::readSeekHead(QByteArray const &buf, int depth) {
    forEachChild(buf, 0, buf.size(), [&](Element const &seek) {
        if(seek.id != kSeek)
            return;
        quint32 id = 0;
        quint64 position = 0;
        bool hasPosition = false;
        forEachChild(buf, seek.data, seek.data + seek.size, [&](Element const &child) {
            if(child.id == kSeekId && child.size >= 1 && child.size <= 4) {
                id = quint32(readUint(buf, child.data, child.size));
            } else if(child.id == kSeekPosition && child.size >= 1 && child.size <= 8) {
                position = readUint(buf, child.data, child.size);
                hasPosition = true;
            }
        });
        const bool wanted = id == kSeekHead || ((mParts & AudioMetadataReader::Tags) && (id == kInfo || id == kTags)) ||
                            ((mParts & AudioMetadataReader::Cover) && id == kAttachments);
        if(wanted && hasPosition && position <= quint64(mSrc.size()) && mSeekTargets.size() < kMaxSeekTargets)
            mSeekTargets.append({id, qint64(position), depth});
    });
}

void MatroskaReader::readTags(QByteArray const &buf) {
    forEachChild(buf, 0, buf.size(), [&](Element const &tag) {
        if(tag.id != kTag)
            return;
        // Targets may come after the SimpleTags it applies to, so collect first.
        int level = kAlbumLevel;
        bool elsewhere = false;
        QList<SimpleTag> simpleTags;
        forEachChild(buf, tag.data, tag.data + tag.size, [&](Element const &child) {
            if(child.id == kTargets) {
                forEachChild(buf, child.data, child.data + child.size, [&](Element const &target) {
                    const quint64 value = readUint(buf, target.data, target.size);
                    if(target.id == kTargetTypeValue)
                        level = int(std::min<quint64>(value, 100));
                    // Tags about one chapter, edition or attachment say nothing about the file.
                    else if(target.id == kTagChapterUid || target.id == kTagEditionUid ||
                            target.id == kTagAttachmentUid)
                        elsewhere = elsewhere || value != 0;
                });
            } else if(child.id == kSimpleTag) {
                readSimpleTag(buf, child.data, child.data + child.size, {}, 0, simpleTags);
            }
        });
        if(elsewhere)
            return;
        for(SimpleTag &simpleTag : simpleTags) {
            simpleTag.level = level;
            mTags.append(std::move(simpleTag));
        }
    });
}

// A SimpleTag may nest others that qualify it ("ARTIST" > "URL"); those are kept as "ARTIST/URL".
void MatroskaReader::readSimpleTag(QByteArray const &buf, qsizetype begin, qsizetype end, QString const &parent,
                                   int depth, QList<SimpleTag> &out) {
    QString name, value;
    QList<Element> nested;
    forEachChild(buf, begin, end, [&](Element const &child) {
        if(child.id == kTagName && child.size <= kMaxTagNameBytes)
            name = readString(buf, child.data, child.size);
        else if(child.id == kTagString)
            value = readString(buf, child.data, child.size);
        else if(child.id == kSimpleTag && depth + 1 < kMaxTagDepth)
            nested.append(child);
    });
    if(name.isEmpty())
        return;
    const QString path = parent.isEmpty() ? name : parent + u'/' + name;
    if(!value.isEmpty())
        out.append({0, path, path.toUpper(), value});
    for(Element const &child : std::as_const(nested))
        readSimpleTag(buf, child.data, child.data + child.size, path, depth + 1, out);
}

void MatroskaReader::readAttachments(Element const &attachments) {
    const qint64 end = attachments.data + std::min(attachments.size, mSrc.size() - attachments.data);
    qint64 pos = attachments.data;
    while(pos < end && mElements < kMaxStructures) {
        mElements++;
        Element file;
        if(!readElement(pos, file) || file.data > end)
            break;
        if(file.size < 0 || file.size > end - file.data)
            file.size = end - file.data;
        if(file.id == kAttachedFile)
            readAttachedFile(file.data, file.data + file.size);
        pos = file.data + file.size;
    }
}

// Attachments are read by their headers: one may be a large font or video, and only the chosen picture's
// bytes are ever read. The cover-art convention names the pictures cover, cover_land, small_cover and
// small_cover_land; failing those, the first image will do.
void MatroskaReader::readAttachedFile(qint64 begin, qint64 end) {
    QString name, mimeType, description;
    qint64 dataOffset = -1, dataSize = 0;
    qint64 pos = begin;
    while(pos < end && mElements < kMaxStructures) {
        mElements++;
        Element e;
        if(!readElement(pos, e) || e.data > end)
            break;
        const bool cut = e.size < 0 || e.size > end - e.data;
        if(cut)
            e.size = end - e.data;
        // A picture cut short by a truncated file would decode to a half-grey square; better none.
        if(e.id == kFileData && !cut) {
            dataOffset = e.data;
            dataSize = e.size;
        } else if((e.id == kFileName || e.id == kFileMimeType || e.id == kFileDescription) && e.size <= kMaxNameBytes) {
            const QString text = decodeText(mSrc.read(e.data, e.size), TextEncoding::Utf8).trimmed();
            if(e.id == kFileName)
                name = text;
            else if(e.id == kFileMimeType)
                mimeType = text;
            else
                description = text;
        }
        pos = e.data + e.size;
    }
    const QString lowerName = name.toLower();
    if(dataOffset < 0 || dataSize <= 0 || dataSize > kMaxPictureBytes ||
       !(mimeType.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive) || isImageName(lowerName)))
        return;
    int rank = 3;
    if(lowerName.startsWith(QStringLiteral("cover_land")))
        rank = 1;
    else if(lowerName.startsWith(QStringLiteral("cover")))
        rank = 0;
    else if(lowerName.startsWith(QStringLiteral("small_cover")))
        rank = 2;
    if(rank < mCover.rank)
        mCover = {rank, dataOffset, dataSize, mimeType, description.isEmpty() ? name : description};
}

// All values of tag `name` at `level` (or any level), in file order, without repeats.
QString MatroskaReader::joined(QString const &name, int level) const {
    QStringList values;
    for(SimpleTag const &tag : mTags) {
        if(tag.upper == name && (level == kAnyLevel || tag.level == level))
            values.append(tag.value);
    }
    // By hash, afterwards: checking each against the list as it came would take quadratic time.
    values.removeDuplicates();
    return values.join(QStringLiteral("; "));
}

// Levels follow matroska.org's tagging guide: 30 is the track, 50 the album, and a tag at a higher level
// applies to the levels below it unless they say otherwise. ffmpeg writes a file's own metadata at 50 with
// no Targets, so when nothing is at 30 the level-50 tags describe the track itself.
void MatroskaReader::applyTags(AudioMetadata &meta) const {
    struct Source {
        QString name;
        int level;
    };
    QSet<QString> used;
    // The first source that has a value; its name is then not listed again on its own.
    auto pick = [&](QList<Source> const &sources) {
        for(Source const &source : sources) {
            const QString value = joined(source.name, source.level);
            if(!value.isEmpty()) {
                used.insert(source.name);
                return value;
            }
        }
        return QString();
    };
    auto nearest = [](QString const &name) {
        return QList<Source>{{name, kTrackLevel}, {name, kAlbumLevel}, {name, kAnyLevel}};
    };
    auto numbered = [&](QString const &number, int totalLevel) {
        if(number.isEmpty() || number.contains(u'/'))
            return number;
        const QString total = pick({{u"TOTAL_PARTS"_s, totalLevel}});
        return total.isEmpty() ? number : number + u'/' + total;
    };
    bool hasTrackLevel = false;
    for(SimpleTag const &tag : mTags)
        hasTrackLevel = hasTrackLevel || tag.level == kTrackLevel;
    const int partLevel = hasTrackLevel ? kTrackLevel : kAlbumLevel;

    addField(meta, Field::Title, {}, pick({{u"TITLE"_s, kTrackLevel}}));
    addField(meta, Field::Artist, {}, pick({{u"ARTIST"_s, kTrackLevel}, {u"ARTIST"_s, kAlbumLevel}}));
    addField(meta, Field::Album, {}, pick(QList<Source>{{u"TITLE"_s, kAlbumLevel}} + nearest(u"ALBUM"_s)));
    QList<Source> albumArtist = nearest(u"ALBUM_ARTIST"_s) + nearest(u"ALBUMARTIST"_s);
    if(hasTrackLevel)
        albumArtist.append({u"ARTIST"_s, kAlbumLevel});
    addField(meta, Field::AlbumArtist, {}, pick(albumArtist));
    addField(meta, Field::Date, {},
             pick(nearest(u"DATE_RELEASED"_s) + nearest(u"DATE_RECORDED"_s) + nearest(u"DATE"_s)));
    addField(meta, Field::Genre, {}, pick(nearest(u"GENRE"_s)));
    addField(meta, Field::Track, {}, numbered(pick({{u"PART_NUMBER"_s, partLevel}}), kAlbumLevel));
    addField(meta, Field::Track, {}, pick(nearest(u"TRACKNUMBER"_s)));
    if(hasTrackLevel)
        addField(meta, Field::Disc, {}, numbered(pick({{u"PART_NUMBER"_s, kAlbumLevel}}), kEditionLevel));
    addField(meta, Field::Disc, {}, pick(nearest(u"DISCNUMBER"_s)));
    addField(meta, Field::Composer, {}, pick(nearest(u"COMPOSER"_s)));
    addField(meta, Field::Comment, {}, pick(nearest(u"COMMENT"_s) + nearest(u"DESCRIPTION"_s)));

    // mkvmerge's per-track statistics and ffmpeg's per-track DURATION describe the stream, not the music.
    static const QSet<QString> statistics = {u"DURATION"_s,
                                             u"BPS"_s,
                                             u"NUMBER_OF_FRAMES"_s,
                                             u"NUMBER_OF_BYTES"_s,
                                             u"_STATISTICS_WRITING_APP"_s,
                                             u"_STATISTICS_WRITING_DATE_UTC"_s,
                                             u"_STATISTICS_TAGS"_s};
    QSet<QString> listed;
    for(SimpleTag const &tag : mTags) {
        if(used.contains(tag.upper) || statistics.contains(tag.upper) || tag.upper.endsWith(u"/SORT_WITH") ||
           listed.contains(tag.upper))
            continue;
        listed.insert(tag.upper);
        addField(meta, Field::None, tag.name, joined(tag.upper, kAnyLevel));
    }
}

void MatroskaReader::apply(AudioMetadata &meta) const {
    if(mTags.isEmpty() && mInfoTitle.isEmpty() && mCover.rank > 3)
        return;
    addTagType(meta, QStringLiteral("Matroska"));
    if(mParts & AudioMetadataReader::Tags) {
        applyTags(meta);
        // The Segment title is what players show when there are no tags, so a TITLE tag outranks it.
        addField(meta, Field::Title, {}, mInfoTitle);
    }
    if((mParts & AudioMetadataReader::Cover) && mCover.rank <= 3) {
        AudioPicture picture;
        picture.data = mSrc.read(mCover.offset, mCover.size);
        picture.mimeType = mCover.mimeType;
        picture.description = mCover.description;
        // The named covers are all of the front, whatever their size or orientation.
        picture.type = mCover.rank < 3 ? 3 : -1;
        offerPicture(meta, std::move(picture));
    }
}

} // namespace

bool parseMatroska(ByteSource const &src, AudioMetadata &meta, int parts) {
    MatroskaReader reader(src, parts);
    if(!reader.read())
        return false;
    reader.apply(meta);
    return true;
}

} // namespace AudioMeta
