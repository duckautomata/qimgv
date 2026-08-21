#!/usr/bin/env bash
# Assemble a portable Windows package from an already-configured build.
#
# Run from an MSYS2 UCRT64 shell:
#     cmake --preset windows-msys2
#     cmake --build --preset windows-msys2
#     ./scripts/package-windows.sh
#
# Result: build/dist/  (self-contained; just zip it)
#
# Unlike the old build-qimgv.sh this does NOT download a custom Qt, a custom
# OpenCV, or build five image plugins from source -- every dependency is an
# MSYS2 package. See docs/BUILDING.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build/windows-msys2}"
DIST_DIR="${DIST_DIR:-$SRC_DIR/build/dist}"
MSYS_BIN="${MSYS_BIN:-/ucrt64/bin}"

if [[ ! -x "$BUILD_DIR/bin/qimgv.exe" ]]; then
    echo "error: $BUILD_DIR/bin/qimgv.exe not found." >&2
    echo "       Build first:  cmake --build --preset windows-msys2" >&2
    exit 1
fi

echo "==> Preparing $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

echo "==> Copying qimgv"
cp "$BUILD_DIR/bin/qimgv.exe" "$DIST_DIR/"
if [[ -f "$BUILD_DIR/bin/plugins/player_mpv.dll" ]]; then
    mkdir -p "$DIST_DIR/plugins"
    cp "$BUILD_DIR/bin/plugins/player_mpv.dll" "$DIST_DIR/plugins/"
fi
if [[ -d "$BUILD_DIR/bin/translations" ]]; then
    cp -r "$BUILD_DIR/bin/translations" "$DIST_DIR/"
fi

echo "==> Running windeployqt (Qt libs, platform + imageformat plugins)"
windeployqt6 --release --no-compiler-runtime --no-translations \
             --no-system-d3d-compiler --no-opengl-sw \
             "$DIST_DIR/qimgv.exe" 2>&1 | sed 's/^/    /'

echo "==> Adding kimageformats plugins (AVIF / JPEG XL / HEIF / ...)"
KIF_DIR="/ucrt64/share/qt6/plugins/imageformats"
mkdir -p "$DIST_DIR/imageformats"
shopt -s nullglob
for p in "$KIF_DIR"/kimg_*.dll "$KIF_DIR"/qavif*.dll "$KIF_DIR"/qjpegxl*.dll "$KIF_DIR"/qheif*.dll; do
    cp -v "$p" "$DIST_DIR/imageformats/" | sed 's/^/    /'
done
shopt -u nullglob

# --------------------------------------------------------------------------
# Resolve every MSYS2-provided DLL the binaries need, transitively. ldd
# understands MinGW PE binaries inside MSYS2, so we do not need a hardcoded
# DLL list the way the old script did (that list went stale constantly).
# --------------------------------------------------------------------------
echo "==> Resolving runtime DLLs"
collect_deps() {
    local target="$1"
    ldd "$target" 2>/dev/null \
        | awk '{print $3}' \
        | grep -iE "^(/ucrt64|$MSYS_BIN)" || true
}

declare -A SEEN=()
QUEUE=("$DIST_DIR/qimgv.exe")
[[ -f "$DIST_DIR/plugins/player_mpv.dll" ]] && QUEUE+=("$DIST_DIR/plugins/player_mpv.dll")
while IFS= read -r -d '' f; do QUEUE+=("$f"); done \
    < <(find "$DIST_DIR" -name '*.dll' -print0)

while [[ ${#QUEUE[@]} -gt 0 ]]; do
    current="${QUEUE[0]}"; QUEUE=("${QUEUE[@]:1}")
    while read -r dep; do
        [[ -z "$dep" ]] && continue
        base="$(basename "$dep")"
        [[ -n "${SEEN[$base]:-}" ]] && continue
        SEEN[$base]=1
        if [[ ! -f "$DIST_DIR/$base" ]]; then
            cp "$dep" "$DIST_DIR/"
            QUEUE+=("$DIST_DIR/$base")
        fi
    done < <(collect_deps "$current")
done
echo "    ${#SEEN[@]} DLLs copied"

echo "==> Adding mpv.exe (used for thumbnailing)"
[[ -f "$MSYS_BIN/mpv.exe" ]] && cp "$MSYS_BIN/mpv.exe" "$DIST_DIR/" || true

echo "==> Portable-mode directories"
mkdir -p "$DIST_DIR/conf" "$DIST_DIR/cache" "$DIST_DIR/thumbnails"
if [[ -d "$SRC_DIR/qimgv/distrib/mimedata/data" ]]; then
    cp -r "$SRC_DIR/qimgv/distrib/mimedata/data" "$DIST_DIR/"
fi

echo
echo "==> Done: $DIST_DIR ($(du -sh "$DIST_DIR" | cut -f1))"
