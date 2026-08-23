<h1 align="center">qimgv</h1>

<p align="center">
  <b>Fast, configurable image viewer with optional video support.</b><br>
  <sub>Qt 6 · Windows · builds on Linux and macOS</sub>
</p>

<p align="center">
  <a href="https://github.com/duckautomata/qimgv/actions/workflows/ci.yml">
    <img alt="CI" src="https://github.com/duckautomata/qimgv/actions/workflows/ci.yml/badge.svg">
  </a>
  <a href="https://github.com/duckautomata/qimgv/releases">
    <img alt="Release" src="https://img.shields.io/github/v/release/duckautomata/qimgv?include_prereleases">
  </a>
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue">
</p>

> **This is a fork.** The original [easymodo/qimgv](https://github.com/easymodo/qimgv) is no longer
> actively maintained. This fork continues it: Qt 6 only, a modernized build system, working CI, and
> support for current image and video codecs. All credit for the original work goes to
> [easymodo](https://github.com/easymodo) and the upstream contributors.

|  Main window & panel  |  Folder view  |  Settings  |
|:---:|:---:|:---:|
| [![img1](qimgv/distrib/screenshots/qimgv0.9_1_thumb.jpg)](qimgv/distrib/screenshots/qimgv0.9_1.jpg?raw=true) | [![img2](qimgv/distrib/screenshots/qimgv0.9_2_thumb.jpg)](qimgv/distrib/screenshots/qimgv0.9_2.jpg?raw=true) | [![img3](qimgv/distrib/screenshots/qimgv_3_thumb.jpg)](qimgv/distrib/screenshots/qimgv_3.jpg?raw=true) |

## Features

- Minimal, uncluttered UI — controls appear only when you need them
- Fast: threaded decoding, prefetching, and an in-memory image cache
- Fully configurable themes and keyboard shortcuts
- High-quality scaling (bicubic / bilinear+sharpen) via OpenCV
- Basic editing: crop, rotate, resize
- Quick copy / move to nine destination folders
- Folder view with thumbnails
- Video playback via libmpv
- Run custom shell scripts on the current image

## Supported formats

**Images (built in):** JPEG · PNG · GIF · BMP · PPM/PGM/PBM · XBM/XPM · SVG · ICO

**Images (via Qt plugins, bundled in the Windows build):**

| Format | Provided by |
|---|---|
| WebP (incl. animated), TIFF, TGA, ICNS, WBMP | `qt6-imageformats` |
| AVIF (incl. animated), HEIF/HEIC, JPEG XL, QOI, PSD, EXR, HDR, DDS | [`kimageformats`](https://invent.kde.org/frameworks/kimageformats) |
| Camera RAW (CR2/CR3, NEF, ARW, DNG, RAF, ORF, RW2, …) | `kimageformats` |

**Not supported:** APNG animation. No maintained Qt 6 APNG plugin is packaged by
mainstream distributions, so `.apng` files display as a single static frame.
[QtApng](https://github.com/Skycoder42/QtApng) can be built from source if you need it.

Run `qimgv --build-options` to see exactly which formats your install can read.

**Video (via libmpv — anything ffmpeg can decode):** H.264 · H.265/HEVC · AV1 · VP8/VP9 ·
ProRes (incl. 4444 with alpha) · MPEG-2/4 · Theora · WMV, in `mp4` `mkv` `webm` `mov` `avi` `ts`
`m2ts` `mxf` `ogv` `3gp` `wmv` `flv` and more.

Video with an alpha channel (ProRes 4444, VP9/AV1 alpha, transparent WebM) is composited against the
application background rather than rendered over black.

## Installation

### Windows

Three downloads on the [releases page](https://github.com/duckautomata/qimgv/releases/latest):

| File | Size | Use it if |
|---|---|---|
| `…-win64-setup.exe` | ~88 MB | You want it installed like a normal app. **Start here.** |
| `…-win64.zip` | ~131 MB | You want it portable, with nothing written outside the folder. |
| `…-win64-minimal.zip` | ~61 MB | Images only — no video playback, smallest download. |

The installer is per-user, so it needs no administrator rights. Full instructions,
including upgrading, where settings live and how to set qimgv as your default image
viewer, are in **[docs/INSTALL.md](docs/INSTALL.md)**.

### Linux

No binaries yet. Build from source (see below), or use the `qimgv/distrib/PKGBUILD` on Arch.

Note that `qimgv` in your distro's repos is the **upstream** package, not this fork.

### macOS

Build from source. See [docs/BUILDING.md](docs/BUILDING.md).

It compiles and its tests pass in CI, but no one has run the application on macOS
yet — treat it as unverified rather than supported.

## Building

Requires **Qt 6.5+**, CMake 3.21+, and a C++20 compiler.

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/bin/qimgv
```

That's it — the presets carry every flag. Full dependency lists and per-platform notes are in
**[docs/BUILDING.md](docs/BUILDING.md)**.

| Preset | Purpose |
|---|---|
| `dev` | Debug, all features, tests on — the day-to-day preset |
| `release` | Optimized with LTO |
| `relwithdebinfo` | Optimized with symbols, for profiling |
| `asan` | Debug + AddressSanitizer/UBSan |
| `minimal` | No video/exiv2/OpenCV — checks the optional-feature paths still compile |
| `windows-msys2` | Windows release package build |
| `macos` | macOS `.app` bundle |

## Default controls

| Action | Shortcut |
| --- | --- |
| Next / previous image | Right / Left arrow, MouseWheel |
| First / last image | Home / End |
| Zoom in / out | Ctrl+MouseWheel, Ctrl+Up / Ctrl+Down |
| Zoom (alternative) | Hold right mouse button and move up / down |
| Fit window / width / 1:1 | 1 / 2 / 3 |
| Cycle fit modes | Space |
| Toggle fullscreen | DoubleClick, F, F11 |
| Exit fullscreen | Esc |
| EXIF panel | I |
| Crop / Resize | X / R |
| Rotate left / right | Ctrl+L / Ctrl+R |
| Open containing directory | Ctrl+D |
| Slideshow / Shuffle | ~ / Ctrl+~ |
| Quick copy / move | C / M |
| Trash / Delete | Delete / Shift+Delete |
| Save / Save As | Ctrl+S / Ctrl+Shift+S |
| Folder view | Enter / Backspace |
| Open | Ctrl+O |
| Print / Export PDF | Ctrl+P |
| Settings | P |
| Quit | Esc, Ctrl+Q, Alt+X, MiddleClick |

Every shortcut is rebindable under **Settings → Controls**.

## Usage notes

### Quick copy / move

Press <kbd>C</kbd> or <kbd>M</kbd> to bring up the panel. Nine destination directories are shown;
click a folder icon to change one. With the panel visible, press <kbd>1</kbd>–<kbd>9</kbd> to send
the current image there. Press <kbd>C</kbd> / <kbd>M</kbd> again to hide it.

### Running scripts

**Settings → Scripts → Add.** A script can be a shell command or a script file.

```bash
# command form — %file% is substituted
convert %file% %file%_.pdf
```

```bash
#!/bin/bash
# script-file form — $1 is the image path
gimp "$1"
```

Script files must be executable and carry a shebang. Bind the script to a key under
**Settings → Controls → Add**.

### HiDPI

If qimgv renders too small or too large, override the scale factor:

```bash
QT_SCALE_FACTOR=1.5 qimgv /path/to/image.png
```

Values below `1.0` are not supported. qimgv otherwise follows the desktop's global scale factor.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bug reports and pull requests are welcome at
[github.com/duckautomata/qimgv/issues](https://github.com/duckautomata/qimgv/issues).

[docs/WINDOWS-NOTES.md](docs/WINDOWS-NOTES.md) covers the Windows-specific
workarounds, the performance measurements behind the current design, and what
has been verified by hand rather than by CI.
[docs/RELEASING.md](docs/RELEASING.md) covers cutting a release.

## License

[GPL-3.0-or-later](LICENSE). Originally written by [easymodo](https://github.com/easymodo);
see the upstream [contributors list](https://github.com/easymodo/qimgv/graphs/contributors) for the
people who built the foundation this fork stands on.
