#!/usr/bin/env bash
# One-shot dependency setup for building qimgv on Windows.
#
# Run from an MSYS2 *UCRT64* shell:
#     ./scripts/setup-msys2.sh
#
# Then:
#     cmake --preset dev && cmake --build --preset dev
set -euo pipefail

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "error: this must run from the MSYS2 UCRT64 shell." >&2
    echo "       Current MSYSTEM is '${MSYSTEM:-unset}'." >&2
    echo "       Open 'MSYS2 UCRT64' from the Start menu (not MSYS, not MINGW64)." >&2
    exit 1
fi

P=mingw-w64-ucrt-x86_64

PACKAGES=(
    # toolchain
    "$P-toolchain" "$P-cmake" "$P-ninja" "$P-ccache" "$P-pkgconf"
    # Qt
    "$P-qt6-base" "$P-qt6-svg" "$P-qt6-imageformats" "$P-qt6-tools"
    # image codecs: AVIF (incl. animated), HEIF, JPEG XL, QOI, PSD, RAW, ...
    "$P-kimageformats"
    # features
    "$P-opencv" "$P-exiv2" "$P-mpv"
    # convenience
    git
)

echo "==> Installing ${#PACKAGES[@]} package groups (this takes a few minutes the first time)"
pacman -S --needed --noconfirm "${PACKAGES[@]}"

echo
echo "==> Versions"
printf '    %-10s %s\n' cmake  "$(cmake --version | head -1 | awk '{print $3}')"
printf '    %-10s %s\n' ninja  "$(ninja --version)"
printf '    %-10s %s\n' gcc    "$(gcc -dumpversion)"
printf '    %-10s %s\n' qt6    "$(qmake6 -query QT_VERSION 2>/dev/null || echo '?')"
printf '    %-10s %s\n' mpv    "$(pkg-config --modversion mpv 2>/dev/null || echo '?')"
printf '    %-10s %s\n' exiv2  "$(pkg-config --modversion exiv2 2>/dev/null || echo '?')"
printf '    %-10s %s\n' opencv "$(pkg-config --modversion opencv4 2>/dev/null || echo '?')"

echo
echo "==> Done. Next:"
echo "      cmake --preset dev"
echo "      cmake --build --preset dev"
echo "      ./build/dev/bin/qimgv.exe"
