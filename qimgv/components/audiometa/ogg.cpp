#include "audiometa_p.h"

#include <optional>

namespace AudioMeta {

namespace {

enum class Codec { None, Vorbis, Opus, Flac, Speex };

// From the first packet of a logical stream, which a BOS page carries alone.
Codec identify(QByteArray const &packet) {
    if(packet.startsWith("\x01vorbis"))
        return Codec::Vorbis;
    if(packet.startsWith("OpusHead"))
        return Codec::Opus;
    // The Ogg FLAC mapping header: 0x7F "FLAC", version, header packet count, then a native "fLaC".
    if(packet.startsWith("\177FLAC") && packet.mid(9, 4) == "fLaC")
        return Codec::Flac;
    if(packet.startsWith("Speex   "))
        return Codec::Speex;
    return Codec::None;
}

constexpr quint8 kContinued = 0x01;
constexpr quint8 kFirstPage = 0x02;
constexpr quint8 kLastPage = 0x04;
// Header, then up to 255 lacing values.
constexpr qint64 kPageHeaderSize = 27;
constexpr qint64 kMaxPageHeaderSize = kPageHeaderSize + 255;
// Enough of a first packet to tell the codec.
constexpr qint64 kIdentifyBytes = 64;
// Past kMaxStructures pages, each page must have brought at least this much of the chosen stream on
// average. Muxers fill header pages to 4 KiB and more, so only a file built to waste our time runs out.
constexpr qint64 kMinBytesPerPage = 512;
// A stream's headers, pictures included, are a few packets of at most kMaxBlockBytes. Ogg FLAC need not say
// how many there are, so without this a stream of header packets that never ends would be read to EOF.
constexpr qint64 kMaxDeliveredBytes = 2 * kMaxBlockBytes;

struct Page {
    quint8 flags = 0;
    quint32 serial = 0;
    QByteArray lacing;
    qint64 body = 0;
    qint64 bodySize = 0;
    qint64 end = 0;
};

// The page at `pos`, if there is a complete one. The CRC is not checked: a tag reader has no use for
// rejecting a page whose audio is damaged.
bool readPage(ByteSource const &src, qint64 pos, Page &page) {
    if(pos < 0 || pos >= src.size())
        return false;
    const QByteArray head = src.read(pos, qMin(kMaxPageHeaderSize, src.size() - pos));
    if(head.size() < kPageHeaderSize || !head.startsWith("OggS") || head.at(4) != 0)
        return false;
    const int segments = quint8(head.at(26));
    if(head.size() < kPageHeaderSize + segments)
        return false;
    page.flags = quint8(head.at(5));
    page.serial = le32(head, 14);
    page.lacing = head.mid(kPageHeaderSize, segments);
    page.bodySize = 0;
    for(char value : std::as_const(page.lacing))
        page.bodySize += quint8(value);
    page.body = pos + kPageHeaderSize + segments;
    if(page.bodySize > src.size() - page.body)
        return false;
    page.end = page.body + page.bodySize;
    return true;
}

// Takes the chosen stream's header packets in order and stops at the one carrying the tags.
class HeaderReader {
public:
    HeaderReader(Codec codec, AudioMetadata &meta, int parts) : mCodec(codec), mMeta(meta), mParts(parts) {}

    // Returns false once there is nothing more to read.
    bool feed(QByteArray const &packet) {
        const int index = mIndex++;
        switch(mCodec) {
        case Codec::Vorbis:
            // Packet 0 identifies, 1 is the comment, 2 sets up the decoder.
            if(packet.startsWith("\x03vorbis")) {
                parseVorbisComment(packet.mid(7), mMeta, mParts);
                return false;
            }
            return index < 1;
        case Codec::Opus:
            if(packet.startsWith("OpusTags")) {
                parseVorbisComment(packet.mid(8), mMeta, mParts);
                return false;
            }
            return index < 1;
        case Codec::Speex:
            // The comment is the second packet, with no signature of its own.
            if(index == 1) {
                parseVorbisComment(packet, mMeta, mParts);
                return false;
            }
            return index < 1;
        case Codec::Flac:
            return flacPacket(packet, index);
        case Codec::None:
            break;
        }
        return false;
    }

private:
    // After the mapping header, each header packet is one native metadata block, 4-byte block header
    // included, the comment first. Pictures may follow it.
    bool flacPacket(QByteArray const &packet, int index) {
        if(index == 0) {
            // 0 means the encoder did not know how many header packets would follow.
            mFlacHeaderPackets = be16(packet, 7);
            return true;
        }
        if(packet.size() < 4)
            return false;
        const quint8 first = quint8(packet.at(0));
        const quint8 type = first & 0x7F;
        // A frame sync code: the audio has started.
        if(first == 0xFF)
            return false;
        const qint64 length = qMin<qint64>(be24(packet, 1), packet.size() - 4);
        // One comment, as in native FLAC: each further one could add thousands of fields.
        if(type == 4 && !mFlacCommentRead) {
            parseVorbisComment(packet.mid(4, length), mMeta, mParts);
            mFlacCommentRead = true;
        } else if(type == 6 && (mParts & AudioMetadataReader::Cover)) {
            parseFlacPicture(packet.mid(4, length), mMeta, mParts);
        }
        if((first & 0x80) || type == 127)
            return false;
        if(mFlacHeaderPackets != 0 && index >= mFlacHeaderPackets)
            return false;
        // Without a picture to look for, the comment is all there is to read.
        return !(mFlacCommentRead && !(mParts & AudioMetadataReader::Cover));
    }

    Codec mCodec;
    AudioMetadata &mMeta;
    int mParts;
    int mIndex = 0;
    int mFlacHeaderPackets = 0;
    bool mFlacCommentRead = false;
};

} // namespace

bool parseOgg(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts) {
    if(!(parts & AudioMetadataReader::All))
        return false;
    Codec codec = Codec::None;
    quint32 serial = 0;
    std::optional<HeaderReader> headers;

    // A comment packet carrying a large picture in base64 legitimately spans thousands of pages, so the
    // page budget grows with the bytes the chosen stream delivers; pages that deliver nothing (other
    // streams, empty pages) are held to kMaxStructures. The packet itself is held to kMaxBlockBytes.
    qint64 delivered = 0;
    qint64 pages = 0;
    // A packet is built across pages: a lacing value of 255 means "continues", and so does the next
    // page's continued flag.
    QByteArray packet;
    bool inPacket = false;
    bool skipFragment = false;
    qint64 pos = offset;
    Page page;
    while(readPage(src, pos, page)) {
        if(++pages > kMaxStructures && pages > delivered / kMinBytesPerPage)
            break;
        pos = page.end;
        if(codec == Codec::None) {
            // The BOS pages of all streams come first; once they are over without an audio stream we
            // know, there is nothing here for us.
            if(!(page.flags & kFirstPage))
                break;
            codec = identify(src.read(page.body, qMin(page.bodySize, kIdentifyBytes)));
            if(codec == Codec::None)
                continue;
            serial = page.serial;
            headers.emplace(codec, meta, parts);
        } else if(page.serial != serial) {
            // Another logical stream multiplexed with ours.
            continue;
        }

        delivered += page.bodySize;
        if(delivered > kMaxDeliveredBytes)
            break;
        const QByteArray body = src.read(page.body, page.bodySize);
        if(body.size() != page.bodySize)
            break;
        if(page.flags & kContinued) {
            // Continues a packet we never saw the start of: drop the fragment, keep the rest of the page.
            skipFragment = !inPacket;
        } else {
            // A packet still in progress never finished.
            packet.clear();
            inPacket = false;
            skipFragment = false;
        }
        bool more = true;
        qint64 at = 0;
        for(char lacing : std::as_const(page.lacing)) {
            const int value = quint8(lacing);
            if(!skipFragment)
                packet.append(body.constData() + at, value);
            at += value;
            if(packet.size() > kMaxBlockBytes) {
                more = false;
                break;
            }
            if(value == 255) {
                inPacket = !skipFragment;
                continue;
            }
            inPacket = false;
            if(skipFragment) {
                skipFragment = false;
                continue;
            }
            more = headers->feed(packet);
            packet.clear();
            if(!more)
                break;
        }
        if(!more || (page.flags & kLastPage))
            break;
    }
    return codec != Codec::None;
}

} // namespace AudioMeta
