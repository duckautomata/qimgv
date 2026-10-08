#!/usr/bin/env bash
# Regenerates the audio fixtures in this directory. They are committed, so this only needs running when a
# fixture is added or changed; the tests never call it.
#
# Every fixture is a quarter of a second of silence carrying the same tags, so tests can check one set of
# expected values everywhere:
#   title "Fixture", artist "qimgv", album "Test Album", album artist "Various", track "3/12",
#   date "2026", genre "Ambient"
# and, where the container can hold one, a 16x16 cover: solid red PNG, or solid blue JPEG for the ID3v2.3 MP3.
#
# Needs an ffmpeg built with libmp3lame, libvorbis, libopus, libspeex, libtheora and libvpx (MSYS2's is), and
# python3 for the two fixtures assembled by hand.
# Formats ffmpeg cannot write (APE, Musepack, DSF, an ASF WM/Picture, APEv2 binary items) are built by hand
# inside the tests instead.
set -euo pipefail
cd "$(dirname "$0")"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

ff -f lavfi -i "anullsrc=r=22050:cl=mono" -t 0.25 -c:a pcm_s16le "$work/src.wav"
ff -f lavfi -i "color=c=0xff0000:s=16x16" -frames:v 1 "$work/red.png"
ff -f lavfi -i "color=c=0x0000ff:s=16x16" -frames:v 1 -q:v 2 "$work/blue.jpg"
cp "$work/red.png" cover-red.png

tags=(-metadata "title=Fixture" -metadata "artist=qimgv" -metadata "album=Test Album"
      -metadata "album_artist=Various" -metadata "track=3/12" -metadata "date=2026" -metadata "genre=Ambient")
src=(-i "$work/src.wav")
art() { echo -i "$1" -map 0 -map 1 -c:v copy -disposition:v attached_pic; }

# A FLAC PICTURE block for the red PNG, base64-encoded, as Vorbis comments carry it in
# METADATA_BLOCK_PICTURE. Layout: type, mime length, mime, description length, description, width,
# height, depth, colours, data length, data -- all integers 32-bit big-endian.
be32() { printf '%08x' "$1" | sed 's/../\\x&/g'; }
picture_block() {
    local png="$1" mime="image/png" size
    size=$(stat -c %s "$png")
    {
        printf "$(be32 3)"
        printf "$(be32 ${#mime})"; printf '%s' "$mime"
        printf "$(be32 0)"
        printf "$(be32 16)"; printf "$(be32 16)"; printf "$(be32 24)"; printf "$(be32 0)"
        printf "$(be32 "$size")"; cat "$png"
    } | base64 -w0
}
pic=$(picture_block "$work/red.png")

# --- MPEG audio ---------------------------------------------------------------------------------------------
ff "${src[@]}" $(art "$work/blue.jpg") -c:a libmp3lame -b:a 32k -id3v2_version 3 "${tags[@]}" mp3-id3v23.mp3
ff "${src[@]}" $(art "$work/red.png") -c:a libmp3lame -b:a 32k -id3v2_version 4 "${tags[@]}" mp3-id3v24.mp3
# ffmpeg will not write ID3v1 without ID3v2 (-write_id3v1 is ignored with -id3v2_version 0), so take the
# 128-byte ID3v1 tag from the end of a second encode and put it on a tagless one.
ff "${src[@]}" -c:a libmp3lame -b:a 32k -id3v2_version 0 mp3-id3v1.mp3
ff "${src[@]}" -c:a libmp3lame -b:a 32k -write_id3v1 1 "${tags[@]}" "$work/v1source.mp3"
tail -c 128 "$work/v1source.mp3" >> mp3-id3v1.mp3
# No tag at all, and no Xing header: only frame sync identifies it.
ff "${src[@]}" -c:a libmp3lame -b:a 32k -id3v2_version 0 -write_xing 0 mp3-bare.mp3

# --- FLAC and Ogg -------------------------------------------------------------------------------------------
ff "${src[@]}" $(art "$work/red.png") -c:a flac "${tags[@]}" flac-picture.flac
ff "${src[@]}" -c:a libvorbis "${tags[@]}" -metadata "METADATA_BLOCK_PICTURE=$pic" vorbis-picture.ogg
ff "${src[@]}" -c:a libopus -b:a 16k "${tags[@]}" -metadata "METADATA_BLOCK_PICTURE=$pic" opus-picture.opus
ff "${src[@]}" -c:a flac "${tags[@]}" flac-in-ogg.oga
ff "${src[@]}" -c:a libspeex "${tags[@]}" speex.spx

# --- MP4 ----------------------------------------------------------------------------------------------------
ff "${src[@]}" $(art "$work/red.png") -c:a aac -b:a 32k "${tags[@]}" aac-cover.m4a
ff "${src[@]}" -c:a alac "${tags[@]}" alac.m4a
# Audio-only MP4 with a generic major brand: content sniffing calls this video/mp4 or video/quicktime.
ff "${src[@]}" -c:a aac -b:a 32k -f mp4 -brand isom "${tags[@]}" aac-isom.mp4

# --- Everything else ----------------------------------------------------------------------------------------
ff "${src[@]}" -c:a wmav2 -b:a 32k "${tags[@]}" wma.wma
ff "${src[@]}" -c:a libvorbis "${tags[@]}" -attach "$work/red.png" -metadata:s:t mimetype=image/png \
    -metadata:s:t filename=cover.png matroska-cover.mka
ff "${src[@]}" -c:a libopus -b:a 16k -f webm "${tags[@]}" opus-audio-only.webm
ff "${src[@]}" -c:a pcm_s16le "${tags[@]}" wav-info.wav
# ffmpeg's WAV muxer cannot write an ID3 chunk either: append one holding the ID3v2.4 tag (with its red
# cover) from the front of mp3-id3v24.mp3, then fix up the RIFF size. INFO stays first, so it wins.
cp wav-info.wav wav-id3.wav
python3 - mp3-id3v24.mp3 wav-id3.wav <<'PY'
import struct, sys
mp3 = open(sys.argv[1], 'rb').read()
assert mp3[:3] == b'ID3'
size = 10 + ((mp3[6] << 21) | (mp3[7] << 14) | (mp3[8] << 7) | mp3[9]) + (10 if mp3[5] & 0x10 else 0)
chunk = b'id3 ' + struct.pack('<I', size) + mp3[:size] + (b'\x00' if size % 2 else b'')
wav = bytearray(open(sys.argv[2], 'rb').read()) + chunk
wav[4:8] = struct.pack('<I', len(wav) - 8)
open(sys.argv[2], 'wb').write(wav)
PY
ff "${src[@]}" $(art "$work/red.png") -c:a pcm_s16be -write_id3v2 1 "${tags[@]}" aiff-id3.aiff
ff "${src[@]}" -c:a wavpack "${tags[@]}" wavpack-ape.wv
ff "${src[@]}" -c:a tta "${tags[@]}" tta.tta
# Longer than the rest: mpv rejects raw AC-3 shorter than about 0.4 s (too few frames for its probe).
ff -f lavfi -i "anullsrc=r=32000:cl=mono" -t 0.6 -c:a ac3 -b:a 32k ac3.ac3
ff "${src[@]}" -c:a pcm_mulaw -f au sun.au
ff "${src[@]}" -c:a pcm_s16be -f caf apple.caf

# --- Video, which must stay video ---------------------------------------------------------------------------
ff -f lavfi -i "testsrc=s=16x16:d=0.25:r=8" -c:v libtheora theora-video.ogg
ff -f lavfi -i "testsrc=s=16x16:d=0.25:r=8" "${src[@]}" -c:v libvpx -c:a libopus -b:a 16k -shortest vp8-video.webm
# Matroska with a video track but an audio extension: the content wins.
ff -f lavfi -i "testsrc=s=16x16:d=0.25:r=8" "${src[@]}" -c:v libvpx -c:a libvorbis -shortest -f matroska video-misnamed.mka
ff -f lavfi -i "testsrc=s=16x16:d=0.25:r=8" "${src[@]}" -c:v mpeg4 -c:a aac -b:a 32k -shortest mpeg4-video.mp4

ls -l
