#include "audiometa_p.h"

namespace AudioMeta {

namespace {

constexpr quint8 kBlockVorbisComment = 4;
constexpr quint8 kBlockPicture = 6;
// Reserved by the format as invalid, so a header carrying it means we are no longer reading blocks.
constexpr quint8 kBlockInvalid = 127;

} // namespace

bool parseFlac(ByteSource const &src, qint64 offset, AudioMetadata &meta, int parts) {
    if(src.read(offset, 4) != "fLaC")
        return false;
    qint64 pos = offset + 4;
    // The format allows one comment block. Reading every one a file claims to have would let each add
    // thousands of fields.
    bool commentRead = false;
    for(int i = 0; i < kMaxStructures; i++) {
        const QByteArray header = src.read(pos, 4);
        if(header.size() != 4)
            break;
        const bool last = quint8(header.at(0)) & 0x80;
        const quint8 type = quint8(header.at(0)) & 0x7F;
        if(type == kBlockInvalid)
            break;
        const qint64 body = pos + 4;
        const qint64 length = be24(header, 1);
        const bool complete = length <= src.size() - body;
        if(type == kBlockVorbisComment && !commentRead) {
            commentRead = true;
            // Even a comment cut off by the end of the file is worth reading: a partly downloaded file
            // keeps the entries that made it, and the parser stops at the first one that does not fit.
            parseVorbisComment(src.read(body, complete ? length : src.size() - body), meta, parts);
        } else if(type == kBlockPicture && complete && (parts & AudioMetadataReader::Cover) && meta.cover.type != 3) {
            // Nothing replaces a front cover, so once there is one the other pictures are not even read.
            parseFlacPicture(src.read(body, length), meta, parts);
        }
        if(last || !complete)
            break;
        pos = body + length;
    }
    return true;
}

void parseFlacPicture(QByteArray const &block, AudioMetadata &meta, int parts) {
    if(!(parts & AudioMetadataReader::Cover))
        return;
    // Layout: type, mime length, mime, description length, description, width, height, depth, colours,
    // data length, data -- integers all 32-bit big-endian. Every length is checked against what is left
    // of the block before it is used.
    const qint64 size = block.size();
    qint64 pos = 0;
    if(size < 8)
        return;
    const quint32 type = be32(block, 0);
    const qint64 mimeLength = be32(block, 4);
    pos = 8;
    if(mimeLength > size - pos)
        return;
    const QByteArray mime = block.mid(pos, mimeLength);
    pos += mimeLength;
    if(size - pos < 4)
        return;
    const qint64 descriptionLength = be32(block, pos);
    pos += 4;
    if(descriptionLength > size - pos)
        return;
    const QByteArray description = block.mid(pos, descriptionLength);
    pos += descriptionLength;
    // Width, height, depth and colour count describe the image, which is decoded by content anyway.
    if(size - pos < 20)
        return;
    const qint64 dataLength = be32(block, pos + 16);
    pos += 20;
    if(dataLength > size - pos || dataLength > kMaxPictureBytes)
        return;
    // "-->" says the data is a URL to the picture, not the picture. We do not fetch it.
    if(mime == "-->")
        return;
    AudioPicture picture;
    picture.data = block.mid(pos, dataLength);
    picture.mimeType = QString::fromLatin1(mime);
    picture.type = type <= 20 ? int(type) : -1;
    picture.description = decodeText(description, TextEncoding::Utf8);
    offerPicture(meta, std::move(picture));
}

} // namespace AudioMeta
