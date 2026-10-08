#include "mediaprobe.h"

#include <QFile>
#include <QIODevice>
#include <QtEndian>
#include <bit>
#include <cstring>

// Every size and count below comes from the file, so each is checked against what is actually left before it
// is used, and every walk is capped. A corrupt or hostile file costs at most kMaxLoads small reads and
// kMaxStructures parsed headers, then gets Kind::Unknown.

namespace {

using MediaProbe::Kind;
using MediaProbe::Result;

// Reads at scattered offsets through one small window. Neighbouring headers -- consecutive Ogg pages, the
// elements at the start of a Matroska segment, the boxes at the head of an MP4 track -- come out of one load,
// and the loads are capped however the file is laid out.
class Source {
public:
    explicit Source(QIODevice &device) : mDevice(device), mSize(device.size()) {}

    qint64 size() const { return mSize; }

    // Copies up to `len` bytes from `pos` and returns how many there were: fewer near the end of the file,
    // none past it or once the read budget is spent.
    int readSome(qint64 pos, void *out, int len) {
        if(pos < 0 || pos >= mSize || len <= 0 || len > kWindow)
            return 0;
        len = static_cast<int>(qMin<qint64>(len, mSize - pos));
        if(pos < mBase || pos + len > mBase + mFilled) {
            if(mLoads >= kMaxLoads || !mDevice.seek(pos))
                return 0;
            mLoads++;
            mBase = pos;
            mFilled = static_cast<int>(qMax<qint64>(0, mDevice.read(mWindow, kWindow)));
            len = qMin(len, mFilled);
        }
        memcpy(out, mWindow + (pos - mBase), static_cast<size_t>(len));
        return len;
    }

    bool read(qint64 pos, void *out, int len) { return readSome(pos, out, len) == len; }

    // Called once per header parsed; false once the walk has gone on long enough to be pathological.
    bool visit() { return ++mStructures <= kMaxStructures; }

private:
    static constexpr int kWindow = 4096;
    static constexpr int kMaxLoads = 64;
    static constexpr int kMaxStructures = 2048;

    QIODevice &mDevice;
    const qint64 mSize;
    qint64 mBase = 0;
    int mFilled = 0;
    int mLoads = 0;
    int mStructures = 0;
    char mWindow[kWindow];
};

bool startsWith(const uchar *data, int len, const char *magic, int magicLen) {
    return len >= magicLen && memcmp(data, magic, static_cast<size_t>(magicLen)) == 0;
}

template<int N>
bool startsWith(const uchar *data, int len, const char (&magic)[N]) {
    return startsWith(data, len, magic, N - 1);
}

constexpr quint32 fourcc(const char (&s)[5]) {
    return quint32(uchar(s[0])) << 24 | quint32(uchar(s[1])) << 16 | quint32(uchar(s[2])) << 8 | uchar(s[3]);
}

// Formats that open with a signature of their own and only ever hold audio. MPEG audio, ADTS AAC, AC-3 and
// DTS are absent on purpose: they start with a bare sync word, which plenty of other files contain at offset
// 0 by chance, so the extension has to back them up (DocumentInfo does that).
const char *audioSignature(const uchar *p, int n) {
    if(startsWith(p, n, "fLaC"))
        return "flac";
    if(startsWith(p, n, "MAC "))
        return "ape";
    if(startsWith(p, n, "wvpk"))
        return "wavpack";
    // SV8, then SV7, whose low nibble after "MP+" is the stream version.
    if(startsWith(p, n, "MPCK") || (startsWith(p, n, "MP+") && n >= 4 && (p[3] & 0x0F) == 7))
        return "musepack";
    if(startsWith(p, n, "TTA1"))
        return "tta";
    if(startsWith(p, n, "tBaK"))
        return "tak";
    if(startsWith(p, n, "#!AMR"))
        return "amr";
    if(startsWith(p, n, "caff") && n >= 6 && qFromBigEndian<quint16>(p + 4) == 1)
        return "caf";
    // The second field is the offset of the audio data, after a header of at least 24 bytes.
    if(startsWith(p, n, ".snd") && n >= 8 && qFromBigEndian<quint32>(p + 4) >= 24)
        return "au";
    // DSF's first chunk is always 28 bytes long.
    if(startsWith(p, n, "DSD ") && n >= 12 && qFromLittleEndian<quint64>(p + 4) == 28)
        return "dsf";
    return nullptr;
}

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------

// Only ever found in front of audio. The checks keep three letters of text from passing for a tag: a known
// major version, and a size whose bytes are all syncsafe.
bool isId3v2(const uchar *p, int n) {
    return startsWith(p, n, "ID3") && n >= 10 && p[3] >= 2 && p[3] <= 4 && p[4] != 0xFF &&
           (p[6] | p[7] | p[8] | p[9]) < 0x80;
}

// Names the audio behind the tag, for the formats that put one in front of their own signature.
Result probeId3(Source &src, const uchar *header) {
    const qint64 size = qint64(header[6]) << 21 | qint64(header[7]) << 14 | qint64(header[8]) << 7 | header[9];
    const bool footer = header[3] == 4 && (header[5] & 0x10);
    // Zeroed although only the first n bytes are read: GCC 11-13 warn otherwise once readSome() is inlined.
    uchar next[12] = {};
    const int n = src.readSome(10 + size + (footer ? 10 : 0), next, sizeof next);
    if(const char *container = audioSignature(next, n))
        return {Kind::Audio, container};
    // ADTS: a 12-bit sync and layer 0, where MPEG audio has an 11-bit sync and a non-zero layer.
    if(n >= 2 && next[0] == 0xFF && (next[1] & 0xF6) == 0xF0)
        return {Kind::Audio, "aac"};
    return {Kind::Audio, "mp3"};
}

// ---------------------------------------------------------------------------
// Ogg
// ---------------------------------------------------------------------------

// The first packet of each logical stream identifies its codec. Skeleton, Kate and anything unknown carry no
// sound or picture of their own and do not count. (Octal escapes: a hex escape would swallow the letters.)
Kind oggStreamKind(const uchar *p, int n) {
    static const struct {
        const char *magic;
        int len;
        Kind kind;
    } kCodecs[] = {
        {"\200theora", 7, Kind::Video}, {"\200daala", 6, Kind::Video},
        {"BBCD\0", 5, Kind::Video},                                          // Dirac
        {"OVP80", 5, Kind::Video},      {"\001video\0\0\0", 9, Kind::Video}, // OGM
        {"\001vorbis", 7, Kind::Audio}, {"OpusHead", 8, Kind::Audio},
        {"\177FLAC", 5, Kind::Audio},   {"fLaC", 4, Kind::Audio}, // FLAC's pre-1.1.1 Ogg mapping
        {"Speex   ", 8, Kind::Audio},   {"PCM     ", 8, Kind::Audio},
        {"CELT    ", 8, Kind::Audio},   {"\001audio\0\0\0", 9, Kind::Audio}, // OGM
    };
    for(const auto &codec : kCodecs) {
        if(startsWith(p, n, codec.magic, codec.len))
            return codec.kind;
    }
    return Kind::Unknown;
}

// Each logical stream opens with a beginning-of-stream page holding that first packet alone, and all of them
// precede every other page, so the walk ends at the first page without the BOS flag.
Result probeOgg(Source &src) {
    constexpr int kMaxStreams = 32;
    bool audio = false;
    qint64 pos = 0;
    for(int i = 0; i < kMaxStreams && src.visit(); i++) {
        uchar header[27];
        if(!src.read(pos, header, sizeof header) || memcmp(header, "OggS", 4) != 0 || header[4] != 0 ||
           !(header[5] & 0x02))
            break;
        const int segments = header[26];
        uchar lacing[255];
        if(segments == 0 || !src.read(pos + 27, lacing, segments))
            break;
        qint64 body = 0;
        for(int s = 0; s < segments; s++)
            body += lacing[s];
        uchar packet[9];
        const int n = src.readSome(pos + 27 + segments, packet, static_cast<int>(qMin<qint64>(body, sizeof packet)));
        const Kind kind = oggStreamKind(packet, n);
        if(kind == Kind::Video)
            return {Kind::Video, "ogg"};
        audio = audio || kind == Kind::Audio;
        pos += 27 + segments + body;
    }
    return {audio ? Kind::Audio : Kind::Unknown, "ogg"};
}

// ---------------------------------------------------------------------------
// Matroska / WebM
// ---------------------------------------------------------------------------

constexpr quint32 kEbmlHeader = 0x1A45DFA3;
constexpr quint32 kDocType = 0x4282;
constexpr quint32 kSegment = 0x18538067;
constexpr quint32 kSeekHead = 0x114D9B74;
constexpr quint32 kSeek = 0x4DBB;
constexpr quint32 kSeekId = 0x53AB;
constexpr quint32 kSeekPosition = 0x53AC;
constexpr quint32 kTracks = 0x1654AE6B;
constexpr quint32 kTrackEntry = 0xAE;
constexpr quint32 kTrackType = 0x83;
constexpr quint32 kCluster = 0x1F43B675;

struct Element {
    quint32 id = 0;
    qint64 pos = 0, data = 0, end = 0;
    // Live recordings leave the size open; such an element runs to the end of its parent.
    bool unknownSize = false;
};

// An EBML element header: a variable-length ID that keeps its length marker (the spec writes IDs that way)
// and a variable-length size that does not. A size reaching past the parent is cut back to it, which is all
// a truncated file needs.
bool readElement(Source &src, qint64 pos, qint64 limit, Element &e) {
    uchar b[12];
    const int n = src.readSome(pos, b, sizeof b);
    if(n < 2 || b[0] == 0)
        return false;
    const int idLen = std::countl_zero(b[0]) + 1;
    if(idLen > 4 || idLen >= n || b[idLen] == 0)
        return false;
    const int sizeLen = std::countl_zero(b[idLen]) + 1;
    if(idLen + sizeLen > n)
        return false;
    quint32 id = 0;
    for(int i = 0; i < idLen; i++)
        id = id << 8 | b[i];
    const uchar valueMask = 0xFF >> sizeLen;
    quint64 size = b[idLen] & valueMask;
    bool allOnes = size == valueMask;
    for(int i = 1; i < sizeLen; i++) {
        size = size << 8 | b[idLen + i];
        allOnes = allOnes && b[idLen + i] == 0xFF;
    }
    e.id = id;
    e.pos = pos;
    e.data = pos + idLen + sizeLen;
    if(e.data > limit)
        return false;
    e.unknownSize = allOnes;
    e.end = (allOnes || size > quint64(limit - e.data)) ? limit : e.data + qint64(size);
    return true;
}

// Calls visit(element) for each element in [begin, end) until it returns false. An element of unknown size
// cannot be stepped over, so the walk ends after it.
template<typename Visit>
void forEachElement(Source &src, qint64 begin, qint64 end, Visit visit) {
    Element e;
    for(qint64 pos = begin; pos < end && src.visit(); pos = e.end) {
        if(!readElement(src, pos, end, e) || !visit(e) || e.unknownSize)
            return;
    }
}

bool readUnsigned(Source &src, const Element &e, quint64 &value) {
    const qint64 len = e.end - e.data;
    uchar b[8];
    if(len < 1 || len > 8 || !src.read(e.data, b, static_cast<int>(len)))
        return false;
    value = 0;
    for(int i = 0; i < len; i++)
        value = value << 8 | b[i];
    return true;
}

// Where `seekHead` says the level-1 element `target` starts, or -1. Positions count from the start of the
// segment's data.
qint64 seekTarget(Source &src, const Element &seekHead, qint64 segmentData, qint64 segmentEnd, quint32 target) {
    qint64 found = -1;
    forEachElement(src, seekHead.data, seekHead.end, [&](const Element &seek) {
        if(seek.id != kSeek)
            return true;
        quint64 id = 0, position = 0;
        bool hasId = false, hasPosition = false;
        forEachElement(src, seek.data, seek.end, [&](const Element &e) {
            if(e.id == kSeekId)
                hasId = readUnsigned(src, e, id);
            else if(e.id == kSeekPosition)
                hasPosition = readUnsigned(src, e, position);
            return true;
        });
        if(hasId && hasPosition && id == target && position < quint64(segmentEnd - segmentData)) {
            found = segmentData + qint64(position);
            return false;
        }
        return true;
    });
    return found;
}

// TrackType 1 is video and 2 audio; 3 is "complex", a combined stream, which may hold pictures. Cover art
// lives in Attachments, not here, so an audio file with a cover has no video track.
Kind tracksKind(Source &src, const Element &tracks) {
    bool audio = false, video = false;
    forEachElement(src, tracks.data, tracks.end, [&](const Element &entry) {
        if(entry.id == kTrackEntry) {
            forEachElement(src, entry.data, entry.end, [&](const Element &e) {
                if(e.id != kTrackType)
                    return true;
                quint64 type = 0;
                if(readUnsigned(src, e, type)) {
                    video = video || type == 1 || type == 3;
                    audio = audio || type == 2;
                }
                return false;
            });
        }
        return !video;
    });
    return video ? Kind::Video : audio ? Kind::Audio : Kind::Unknown;
}

// EBML header, then the Segment, whose Tracks element lists every track. Muxers put Tracks ahead of the
// media, but nothing requires it: when the first Cluster comes sooner, only a SeekHead can say where Tracks
// is -- possibly a SeekHead that a first one points to, at the end of the file.
Result probeMatroska(Source &src) {
    Result result{Kind::Unknown, "matroska"};
    Element ebml;
    if(!readElement(src, 0, src.size(), ebml) || ebml.id != kEbmlHeader || ebml.unknownSize)
        return result;
    forEachElement(src, ebml.data, ebml.end, [&](const Element &e) {
        if(e.id != kDocType)
            return true;
        char docType[4];
        if(e.end - e.data >= 4 && src.read(e.data, docType, 4) && memcmp(docType, "webm", 4) == 0)
            result.container = "webm";
        return false;
    });

    Element segment;
    if(!readElement(src, ebml.end, src.size(), segment) || segment.id != kSegment)
        return result;

    qint64 tracksAt = -1, nextSeekHeadAt = -1;
    bool tracksFound = false;
    forEachElement(src, segment.data, segment.end, [&](const Element &e) {
        if(e.id == kTracks) {
            result.kind = tracksKind(src, e);
            tracksFound = true;
            return false;
        }
        if(e.id == kSeekHead && tracksAt < 0) {
            tracksAt = seekTarget(src, e, segment.data, segment.end, kTracks);
            const qint64 next = seekTarget(src, e, segment.data, segment.end, kSeekHead);
            if(next != e.pos && nextSeekHeadAt < 0)
                nextSeekHeadAt = next;
        }
        return e.id != kCluster;
    });
    if(tracksFound)
        return result;

    Element e;
    if(tracksAt < 0 && nextSeekHeadAt >= 0 && readElement(src, nextSeekHeadAt, segment.end, e) && e.id == kSeekHead)
        tracksAt = seekTarget(src, e, segment.data, segment.end, kTracks);
    if(tracksAt >= 0 && readElement(src, tracksAt, segment.end, e) && e.id == kTracks)
        result.kind = tracksKind(src, e);
    return result;
}

// ---------------------------------------------------------------------------
// ISO base media file format: MP4, M4A, MOV, 3GP
// ---------------------------------------------------------------------------

struct Box {
    quint32 type = 0;
    qint64 data = 0, end = 0;
};

// A box header: size 1 means a 64-bit size follows, size 0 that the box runs to the end of its parent (at the
// top level, the file). A size reaching past the parent is cut back to it, as for Matroska: a file cut short
// still has its tracks at the front.
bool readBox(Source &src, qint64 pos, qint64 limit, Box &box) {
    uchar h[16];
    const int n = src.readSome(pos, h, sizeof h);
    if(n < 8)
        return false;
    quint64 size = qFromBigEndian<quint32>(h);
    qint64 header = 8;
    if(size == 1) {
        if(n < 16)
            return false;
        size = qFromBigEndian<quint64>(h + 8);
        header = 16;
    } else if(size == 0) {
        size = quint64(limit - pos);
    }
    if(size < quint64(header) || limit - pos < header)
        return false;
    size = qMin(size, quint64(limit - pos));
    box.type = qFromBigEndian<quint32>(h + 4);
    box.data = pos + header;
    box.end = pos + qint64(size);
    return true;
}

template<typename Visit>
void forEachBox(Source &src, qint64 begin, qint64 end, Visit visit) {
    Box box;
    for(qint64 pos = begin; pos < end && src.visit(); pos = box.end) {
        if(!readBox(src, pos, end, box) || !visit(box))
            return;
    }
}

struct Track {
    quint32 handler = 0;
    bool enabled = true;
};

// trak/tkhd carries the enabled flag, trak/mdia/hdlr the handler type: 'vide', 'soun', 'text', ...
Track readTrack(Source &src, const Box &trak) {
    Track track;
    forEachBox(src, trak.data, trak.end, [&](const Box &box) {
        if(box.type == fourcc("tkhd")) {
            uchar versionAndFlags[4];
            if(box.end - box.data >= 4 && src.read(box.data, versionAndFlags, 4))
                track.enabled = versionAndFlags[3] & 0x01;
        } else if(box.type == fourcc("mdia")) {
            forEachBox(src, box.data, box.end, [&](const Box &child) {
                if(child.type != fourcc("hdlr"))
                    return true;
                // Version and flags, then pre_defined -- QuickTime's component type -- then the handler.
                uchar h[12];
                if(child.end - child.data >= 12 && src.read(child.data, h, sizeof h))
                    track.handler = qFromBigEndian<quint32>(h + 8);
                return false;
            });
        }
        return true;
    });
    return track;
}

// Any enabled video track makes it a video. A disabled one does not: that is how chapter images in audiobooks
// and podcasts are stored, and players do not show them as video.
Kind moovKind(Source &src, const Box &moov) {
    bool audio = false, video = false;
    forEachBox(src, moov.data, moov.end, [&](const Box &box) {
        if(box.type == fourcc("trak")) {
            const Track track = readTrack(src, box);
            video = video || (track.handler == fourcc("vide") && track.enabled);
            audio = audio || track.handler == fourcc("soun");
        }
        return !video;
    });
    return video ? Kind::Video : audio ? Kind::Audio : Kind::Unknown;
}

// Audio-only brands settle it from the ftyp box. Otherwise the tracks do, wherever moov is: writers that do not
// move it to the front leave it after the media data, which is a seek away, not a read.
Result probeIsoBmff(Source &src) {
    Result result{Kind::Unknown, "mp4"};
    Box first;
    if(!readBox(src, 0, src.size(), first))
        return result;
    if(first.type == fourcc("ftyp")) {
        constexpr int kMaxBrands = 64;
        uchar brands[kMaxBrands * 4];
        const qint64 len = qMin<qint64>(first.end - first.data, sizeof brands);
        if(len < 4 || !src.read(first.data, brands, static_cast<int>(len)))
            return result;
        const quint32 major = qFromBigEndian<quint32>(brands);
        if(major == fourcc("qt  "))
            result.container = "mov";
        // Major brand, then the compatible ones after minor_version. Images live in ISOBMFF too, and an image
        // sequence or a Canon CR3 keeps its pictures in tracks that look like video. DocumentInfo handles the
        // images it knows by these brands before asking here; the rest are not ours to call video.
        for(qint64 i = 0; i + 4 <= len; i += 4) {
            if(i == 4)
                continue;
            const quint32 brand = qFromBigEndian<quint32>(brands + i);
            if(brand == fourcc("avif") || brand == fourcc("avis") || brand == fourcc("mif1") ||
               brand == fourcc("msf1") || brand == fourcc("crx "))
                return {};
        }
        if(major == fourcc("M4A ") || major == fourcc("M4B ") || major == fourcc("M4P ") || major == fourcc("F4A ") ||
           major == fourcc("F4B ")) {
            result.kind = Kind::Audio;
            return result;
        }
    } else {
        result.container = "mov";
    }
    forEachBox(src, 0, src.size(), [&](const Box &box) {
        if(box.type != fourcc("moov"))
            return true;
        result.kind = moovKind(src, box);
        return false;
    });
    return result;
}

// Old QuickTime files have no ftyp and open straight on one of these.
bool isQuickTimeAtom(const uchar *type) {
    static const char *const kTypes[] = {"moov", "mdat", "free", "skip", "wide", "pnot"};
    for(const char *t : kTypes) {
        if(memcmp(type, t, 4) == 0)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ASF: WMA, WMV
// ---------------------------------------------------------------------------

// GUIDs as stored: the first three fields little-endian.
constexpr uchar kAsfHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                  0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uchar kAsfStreamProperties[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                            0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uchar kAsfHeaderExtension[16] = {0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11,
                                           0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uchar kAsfExtendedStreamProperties[16] = {0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6, 0x32, 0x43,
                                                    0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A};
constexpr uchar kAsfAudioMedia[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                      0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uchar kAsfVideoMedia[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                      0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

// Every ASF object: a GUID, then a 64-bit size that counts these 24 bytes. Sizes past the parent are cut back
// to it, as for the other containers.
struct AsfObject {
    uchar guid[16];
    qint64 pos = 0, end = 0;
};

bool readAsfObject(Source &src, qint64 pos, qint64 limit, AsfObject &object) {
    uchar h[24];
    if(!src.read(pos, h, sizeof h))
        return false;
    const quint64 size = qFromLittleEndian<quint64>(h + 16);
    if(size < sizeof h || limit - pos < qint64(sizeof h))
        return false;
    memcpy(object.guid, h, 16);
    object.pos = pos;
    object.end = pos + qint64(qMin(size, quint64(limit - pos)));
    return true;
}

template<typename Visit>
void forEachAsfObject(Source &src, qint64 begin, qint64 end, Visit visit) {
    AsfObject object;
    for(qint64 pos = begin; pos < end && src.visit(); pos = object.end) {
        if(!readAsfObject(src, pos, end, object) || !visit(object))
            return;
    }
}

bool isGuid(const AsfObject &object, const uchar (&guid)[16]) {
    return memcmp(object.guid, guid, 16) == 0;
}

// A Stream Properties object's stream type follows its own header. Images and commands are neither.
Kind asfStreamKind(Source &src, const AsfObject &streamProperties) {
    uchar type[16];
    if(streamProperties.end - streamProperties.pos < 40 || !src.read(streamProperties.pos + 24, type, sizeof type))
        return Kind::Unknown;
    if(memcmp(type, kAsfVideoMedia, 16) == 0)
        return Kind::Video;
    if(memcmp(type, kAsfAudioMedia, 16) == 0)
        return Kind::Audio;
    return Kind::Unknown;
}

// An Extended Stream Properties object can end with the stream's Stream Properties object, and for some
// streams -- the extra bitrates of a multi-bitrate WMV -- that is the only place it is declared. It sits after
// 64 bytes of fixed fields, the stream names and the payload extension systems.
Kind asfExtendedStreamKind(Source &src, const AsfObject &esp) {
    qint64 pos = esp.pos + 24 + 60;
    uchar counts[4];
    if(pos + 4 > esp.end || !src.read(pos, counts, sizeof counts))
        return Kind::Unknown;
    pos += 4;
    // Each name: language index, length, then that many bytes.
    for(int i = qFromLittleEndian<quint16>(counts); i > 0; i--) {
        uchar h[4];
        if(!src.visit() || pos + 4 > esp.end || !src.read(pos, h, sizeof h))
            return Kind::Unknown;
        pos += 4 + qFromLittleEndian<quint16>(h + 2);
    }
    // Each system: GUID, data size, then an info block whose length is the last field.
    for(int i = qFromLittleEndian<quint16>(counts + 2); i > 0; i--) {
        uchar h[22];
        if(!src.visit() || pos + 22 > esp.end || !src.read(pos, h, sizeof h))
            return Kind::Unknown;
        pos += 22 + qint64(qFromLittleEndian<quint32>(h + 18));
    }
    AsfObject streamProperties;
    if(pos < esp.end && readAsfObject(src, pos, esp.end, streamProperties) &&
       isGuid(streamProperties, kAsfStreamProperties))
        return asfStreamKind(src, streamProperties);
    return Kind::Unknown;
}

// The Header object leads the file and holds one Stream Properties object per stream, some of them possibly
// inside the Header Extension object. The header also carries tags and cover art, so it is walked object by
// object rather than read whole.
Result probeAsf(Source &src) {
    Result result{Kind::Unknown, "asf"};
    AsfObject header;
    if(!readAsfObject(src, 0, src.size(), header))
        return result;
    bool audio = false, video = false;
    auto note = [&](Kind kind) {
        video = video || kind == Kind::Video;
        audio = audio || kind == Kind::Audio;
    };
    // The Header object's own fields: object count and two reserved bytes.
    forEachAsfObject(src, header.pos + 30, header.end, [&](const AsfObject &object) {
        if(isGuid(object, kAsfStreamProperties)) {
            note(asfStreamKind(src, object));
        } else if(isGuid(object, kAsfHeaderExtension)) {
            // Its objects follow a reserved GUID, a reserved 16-bit field and their total size.
            forEachAsfObject(src, object.pos + 46, object.end, [&](const AsfObject &child) {
                if(isGuid(child, kAsfExtendedStreamProperties))
                    note(asfExtendedStreamKind(src, child));
                return !video;
            });
        }
        return !video;
    });
    result.kind = video ? Kind::Video : audio ? Kind::Audio : Kind::Unknown;
    return result;
}

// Sony Wave64: RIFF and WAVE spelled as GUIDs.
constexpr uchar kWave64Riff[16] = {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
                                   0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
constexpr uchar kWave64Wave[16] = {0x77, 0x61, 0x76, 0x65, 0xF3, 0xAC, 0xD3, 0x11,
                                   0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};

} // namespace

MediaProbe::Result MediaProbe::probe(QString const &path) {
    QFile file(path);
    // Unbuffered: Source keeps its own window, and QFile's would read 16 KiB around every seek.
    if(!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return {};
    return probe(file);
}

MediaProbe::Result MediaProbe::probe(QIODevice &device) {
    if(!device.isReadable() || device.isSequential())
        return {};
    Source src(device);
    uchar head[40];
    const int n = src.readSome(0, head, sizeof head);
    auto at = [&](int offset, const char *magic) { return offset + 4 <= n && memcmp(head + offset, magic, 4) == 0; };

    if(at(0, "OggS"))
        return probeOgg(src);
    if(at(0, "\x1A\x45\xDF\xA3"))
        return probeMatroska(src);
    if(at(4, "ftyp") || (n >= 8 && isQuickTimeAtom(head + 4)))
        return probeIsoBmff(src);
    if(n >= 16 && memcmp(head, kAsfHeader, 16) == 0)
        return probeAsf(src);

    // RF64 and BW64 are WAVE with 64-bit sizes for files past 4 GiB.
    if((at(0, "RIFF") || at(0, "RF64") || at(0, "BW64")) && at(8, "WAVE"))
        return {Kind::Audio, "wav"};
    if(at(0, "RIFF") && at(8, "AVI "))
        return {Kind::Video, "avi"};
    if(n >= 40 && memcmp(head, kWave64Riff, 16) == 0 && memcmp(head + 24, kWave64Wave, 16) == 0)
        return {Kind::Audio, "w64"};
    if(at(0, "FORM") && (at(8, "AIFF") || at(8, "AIFC")))
        return {Kind::Audio, "aiff"};
    if(at(0, "FORM") && (at(8, "8SVX") || at(8, "16SV")))
        return {Kind::Audio, "8svx"};
    if(at(0, "FRM8") && at(12, "DSD "))
        return {Kind::Audio, "dff"};

    if(isId3v2(head, n))
        return probeId3(src, head);
    if(const char *container = audioSignature(head, n))
        return {Kind::Audio, container};
    return {};
}
