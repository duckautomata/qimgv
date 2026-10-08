#pragma once

#include <QByteArray>
#include <QString>

class QIODevice;

// Tells audio from video by looking inside the file. Mime types cannot do it: Ogg, Matroska, WebM, MP4 and
// ASF each hold either, and the three mime databases qimgv can end up with (Qt's built-in one, the
// freedesktop.org.xml the Windows package ships, the system's on Linux) disagree about the names anyway.
//
// It runs wherever DocumentInfo runs -- loader, thumbnailer and file-info threads -- so it only does
// bounded file I/O: a handful of small reads, never a scan of the whole file.
namespace MediaProbe {
enum class Kind { Unknown, Audio, Video };

struct Result {
    Kind kind = Kind::Unknown;
    // Short lower-case container name when recognised, even when the kind is not: "ogg", "matroska",
    // "webm", "mp4", "mov", "asf", "wav", "aiff", "flac", "mp3", "aac", "ape", "wavpack", "musepack",
    // "tta", "dsf", "dff", "amr", "au", "caf", ...
    QByteArray container;
};

// Classifies by content alone. Kind::Unknown when the content does not settle it; the caller then
// falls back to the mime type and the extension.
Result probe(QString const &path);
// The same for a device already open for reading. It must allow seeking, and is left at whatever
// position the probe read last.
Result probe(QIODevice &device);
} // namespace MediaProbe
