#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>

// Tags and cover art read straight out of an audio file, without libmpv. It runs on worker threads -- the
// thumbnailer, the file info panel and the audio view all use it -- so it deals only in QImage and plain
// data, and every parser behind it reads through a size-checked source: a corrupt length makes a parser
// give up, never read out of range or allocate without bound.

struct AudioPicture {
    // Encoded image (JPEG, PNG, WebP, BMP, GIF...) exactly as stored in the tag.
    QByteArray data;
    // As declared by the tag. Often empty or wrong, so decode by content, never by this.
    QString mimeType;
    // ID3v2 / FLAC picture type: 3 is the front cover, -1 unknown.
    int type = -1;
    QString description;

    bool isNull() const { return data.isEmpty(); }
};

struct AudioField {
    // Readable name: "Title", "Artist", "Album artist", "Track"... for the fields every format shares, and
    // the format's own key (e.g. "REPLAYGAIN_TRACK_GAIN") for the rest.
    QString key;
    QString value;
    // One of the fields every format shares, which AudioMetadata also has a member for. A display that
    // cannot show every field keeps these.
    bool common = false;
};

struct AudioMetadata {
    QString title, artist, album, albumArtist, date, genre, track, disc, composer, comment;
    // Every text field found, in the order read. A well-known field appears once, from the tag that won it
    // (see AudioMetadataReader::read()).
    QList<AudioField> fields;
    // "ID3v2.3", "ID3v1", "APEv2", "Vorbis comment", "MP4", "ASF", "RIFF INFO", "AIFF", "Matroska"...
    QStringList tagTypes;
    // The best embedded picture: the front cover if there is one, else the first picture.
    AudioPicture cover;
    // The parsers' bookkeeping while `fields` is being filled -- what is already in it, so that a file with
    // thousands of fields is not checked for duplicates by scanning them all every time. Empty in what
    // AudioMetadataReader returns.
    QSet<std::pair<QString, QString>> fieldIndex;

    bool isEmpty() const { return fields.isEmpty() && cover.isNull(); }
};

// Where AudioMetadataReader::loadCover() found its picture. A cover file next to the track can change, appear
// or go while the track itself stays the same, so whatever keeps a picture made from one has to check that
// file as well.
struct AudioCoverSource {
    // The image file next to the track; empty when the picture was embedded in it, or there was none.
    QString sidecar;
    // The sidecar's, taken before it was read: a change made while it was being read then still shows.
    QDateTime lastModified;
    qint64 size = -1;
};

namespace AudioMetadataReader {
enum Part { Tags = 1, Cover = 2, All = Tags | Cover };

// Reads the tags embedded in `path`, detecting the container by content, not by extension. When a file
// carries more than one tag, the first non-empty value of each well-known field wins in this order:
// the container's own tag (FLAC/Vorbis comment, MP4, ASF, Matroska, RIFF INFO), then ID3v2, APEv2,
// ID3v1. Pass `parts` to skip what you do not need: the thumbnailer asks for Cover only.
// Never throws. Thread-safe.
AudioMetadata read(QString const &path, int parts = All);

// The picture to show for `path`: its embedded cover, or else a sidecar image next to it. Decoded by
// content and, when `maxSize` is valid, scaled down to fit it (aspect kept, never enlarged).
// `originalSize`, if given, receives the size of the image before scaling. A null image if there is
// none, and for a picture over 16384 px on a side or 100 megapixels, or a sidecar file over 32 MiB --
// decompression bombs, as far as a thumbnail is concerned. `source`, if given, receives where the
// picture came from. Thread-safe: QImage only.
QImage loadCover(QString const &path, QSize maxSize = QSize(), QSize *originalSize = nullptr,
                 AudioCoverSource *source = nullptr);

// The cover image stored next to `audioPath`, or an empty string. Looks, case-insensitively, for
// cover, folder, front, album, albumart, albumartsmall, a file named like the track itself, and Windows
// Media Player's AlbumArt_{GUID}_Large/_Small, with a jpg/jpeg/png/webp/bmp/gif extension -- in that
// order of preference. Hidden files count: Windows Media Player writes its covers hidden.
// A folder is listed again only once its modification date has changed, or a few seconds have passed;
// see the comments in the implementation. Thread-safe.
QString findSidecarCover(QString const &audioPath);
} // namespace AudioMetadataReader
