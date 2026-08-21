# Building qimgv

**Requirements:** Qt 6.5+, CMake 3.21+, Ninja, and a C++20 compiler (GCC 11+, Clang 14+, MSVC 19.30+).

Once dependencies are installed, every platform is the same two commands:

```bash
cmake --preset dev
cmake --build --preset dev
```

The binary lands in `build/dev/bin/qimgv`. Presets carry all the flags — you should never need to
pass `-D` by hand for a normal build.

---

## Dependencies

| Dependency | Required? | Purpose |
|---|---|---|
| Qt 6.5+ (Core, Gui, Widgets, Svg, PrintSupport, OpenGLWidgets) | **yes** | everything |
| Qt 6 LinguistTools | no | compiles translations; skipped with a warning if absent |
| exiv2 ≥ 0.27 | optional (`QIMGV_EXIV2`) | EXIF metadata |
| OpenCV (core, imgproc) | optional (`QIMGV_OPENCV`) | high-quality scaling filters |
| libmpv ≥ 2.0 | optional (`QIMGV_VIDEO_SUPPORT`) | video playback |
| KF6WindowSystem | optional (`QIMGV_KDE_BLUR`) | KWin background blur |

Optional dependencies default to **on**. Turn one off if you don't have it:

```bash
cmake --preset dev -DQIMGV_VIDEO_SUPPORT=OFF
```

### Image format plugins

qimgv reads whatever Qt can read. To get AVIF, JPEG XL, HEIF, WebP and friends, install the plugin
packages below — no rebuild of qimgv is needed, they're discovered at runtime.

| Platform | Command |
|---|---|
| Arch | `pacman -S qt6-imageformats kimageformats` |
| Fedora | `dnf install qt6-qtimageformats kf6-kimageformats` |
| RHEL/Rocky 10 | `dnf install qt6-qtimageformats && dnf --enablerepo=epel install kf6-kimageformats` |
| Debian/Ubuntu | `apt install qt6-image-formats-plugins kimageformats6-plugins` |
| MSYS2 | `pacman -S mingw-w64-ucrt-x86_64-{qt6-imageformats,kimageformats}` |
| macOS | `brew install qt` (imageformats included); kimageformats via MacPorts |

Verify what's actually available at runtime with:

```bash
qimgv --build-options
```

---

## Linux

### Fedora / RHEL / Rocky

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf \
    qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel \
    exiv2-devel opencv-devel mpv-libs-devel
```

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake ninja-build pkgconf \
    qt6-base-dev qt6-svg-dev qt6-tools-dev libgl1-mesa-dev \
    libexiv2-dev libopencv-core-dev libopencv-imgproc-dev libmpv-dev
```

> Ubuntu 24.04 ships Qt 6.4, which is **below** the 6.5 floor. Use 24.10+, the
> [Qt online installer](https://www.qt.io/download-qt-installer), or `aqtinstall`, then point CMake
> at it with `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.1/gcc_64`.

### Arch

```bash
sudo pacman -S base-devel cmake ninja qt6-base qt6-svg qt6-tools exiv2 opencv mpv
```

---

## Windows (MSYS2 UCRT64) — the recommended dev environment

Windows is qimgv's primary target and the only platform where you can see the UI, video
playback and transparency behave for real. MSYS2 is the supported toolchain: every dependency,
including libmpv, is a prebuilt package.

1. Install [MSYS2](https://www.msys2.org/) and run the installer's update prompt.
2. Open **MSYS2 UCRT64** from the Start menu — *not* "MSYS2 MSYS" and *not* "MSYS2 MINGW64".
   You can confirm with `echo $MSYSTEM`, which must print `UCRT64`.
3. Clone and install dependencies:

```bash
git clone https://github.com/duckautomata/qimgv.git
cd qimgv
./scripts/setup-msys2.sh
```

   The script refuses to run outside UCRT64 and prints the resolved versions when it finishes.

4. Build and run:

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/bin/qimgv.exe
```

5. Check that the codecs came through:

```bash
./build/dev/bin/qimgv.exe --build-options
```

   `avif`, `heic`, `jxl` and `webp` should all show `[x]`.

### Editor setup

`compile_commands.json` lands in `build/dev/`, so any clangd-based editor works with no
configuration. Two good options:

- **VS Code** — install the *CMake Tools* and *clangd* extensions, open the folder, pick the
  `dev` preset from the status bar. Launch VS Code **from the UCRT64 shell** (`code .`) so it
  inherits the right `PATH`; started from the Start menu it will not find the toolchain.
- **Qt Creator** — opens `CMakeLists.txt` directly and reads `CMakePresets.json` natively. Best
  option if you plan to touch the `.ui` files, since Designer is built in.

### Packaging a portable build

```bash
cmake --preset windows-msys2
cmake --build --preset windows-msys2
./scripts/package-windows.sh     # -> build/dist/
```

`package-windows.sh` runs `windeployqt`, then walks the binaries with `ldd` to copy every MSYS2 DLL
they actually need. There is no hardcoded DLL list to go stale.

### MSVC

The CMake config is MSVC-clean and the app will build, but **libmpv has no usable MSVC build**, so
you must disable video:

```bash
cmake -B build -DQIMGV_VIDEO_SUPPORT=OFF
```

MSYS2 is recommended for anything you intend to ship.

---

## macOS

```bash
brew install cmake ninja qt exiv2 opencv mpv pkgconf
cmake --preset macos
cmake --build --preset macos
```

Produces `build/macos/bin/qimgv.app`. macOS support is best-effort — it builds and runs, but gets
less testing than Windows and Linux.

---

## Development

### Editor / IDE setup

`compile_commands.json` is generated automatically into the build directory, so
[clangd](https://clangd.llvm.org/) works with no extra configuration. Point your editor at it, or
symlink it to the project root:

```bash
ln -sf build/dev/compile_commands.json .
```

VS Code, CLion, Qt Creator, and Visual Studio all read `CMakePresets.json` natively — just open the
folder and pick a preset.

### Tests

```bash
ctest --preset dev
```

Tests run headless via `QT_QPA_PLATFORM=offscreen`. Format-detection tests skip themselves when the
relevant image plugin isn't installed, so a bare system won't produce false failures.

### Useful options

| Option | Default | Effect |
|---|---|---|
| `QIMGV_VIDEO_SUPPORT` | ON | libmpv video playback |
| `QIMGV_EXIV2` | ON | EXIF metadata |
| `QIMGV_OPENCV` | ON | HQ scaling filters |
| `QIMGV_KDE_BLUR` | OFF | KWin background blur |
| `QIMGV_BUILD_TESTS` | OFF | build the ctest suite |
| `QIMGV_LTO` | ON | link-time optimization in Release |
| `QIMGV_WERROR` | OFF | warnings become errors (CI uses this) |
| `QIMGV_SANITIZE` | OFF | ASan + UBSan |

The pre-fork option names (`VIDEO_SUPPORT`, `EXIV2`, `OPENCV_SUPPORT`, `KDE_SUPPORT`) still work but
emit a deprecation warning.

### Build speed

`ccache`/`sccache` and `mold`/`lld` are picked up automatically when present. Installing them is the
single biggest improvement to iteration time:

```bash
sudo dnf install ccache mold     # or apt / pacman
```

### Translations

`.ts` files live in `qimgv/res/translations/`. To add a language, create `<locale>.ts`, add it to
`QIMGV_TS_FILES` in `qimgv/CMakeLists.txt`, then:

```bash
cmake --build --preset dev --target qimgv_lupdate   # refresh source strings
```

and translate with Qt Linguist. Compiled `.qm` files are written next to the binary, so a build-tree
run picks them up automatically.

---

## Troubleshooting

**`Could NOT find Qt6`** — Qt isn't on CMake's search path. Pass
`-DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.1/gcc_64`.

**`libmpv not found`** — install the dev package (`mpv-libs-devel`, `libmpv-dev`,
`mingw-w64-ucrt-x86_64-mpv`), or build without video: `-DQIMGV_VIDEO_SUPPORT=OFF`.

**AVIF/JXL/HEIF files won't open** — the Qt image plugin isn't installed. See
[Image format plugins](#image-format-plugins) above, then check `qimgv --build-options`,
which lists every readable format and names the package supplying each notable one.

**APNG shows only the first frame** — expected. There is no maintained Qt 6 APNG plugin in
mainstream distro repos; qimgv deliberately treats APNG as a static image unless an `apng`
reader is present, rather than pretending to animate it.

**No output from `--build-options` or `--gen-thumbs`** — fixed in this fork. Fedora, RHEL and
Rocky ship `/usr/share/qt6/qtlogging.ini` with `*.debug=false`, which silenced these commands
because they printed through `qDebug()`. They now write to stdout.

**Video plays but transparency renders black** — needs libmpv ≥ 0.38 (`background=none`). Older
libmpv falls back to the pre-0.38 `alpha=yes` option automatically.

**Stale build after switching branches** — `rm -rf build/dev && cmake --preset dev`.
