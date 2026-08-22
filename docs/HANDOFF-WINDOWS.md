# Handoff: continuing qimgv from Windows

Written 2026-08-20 at the end of the `modernize` branch work. Delete this file
once the open items below are closed — it is a snapshot, not documentation.

---

## Situation

`duckautomata/qimgv` is a fork of `easymodo/qimgv` (unmaintained). The
`modernize` branch (9 commits) did the build-system, Qt6, codec and CI work.

The build, test and packaging work was done on a headless Linux server. As of
2026-08-20 the Windows toolchain has been brought up: `dev`, `ci` and
`windows-msys2` all configure, build warning-clean and pass 3/3 tests, the
portable package runs standalone, and the GUI has been looked at for the first
time. macOS and CI have still never run.

Read `git log master..modernize` — the commit messages carry the reasoning and
are more detailed than this file.

---

## Setup

MSYS2 **UCRT64** shell (not MSYS, not MINGW64 — `echo $MSYSTEM` must say
`UCRT64`):

```bash
git clone https://github.com/duckautomata/qimgv.git
cd qimgv && git checkout modernize
./scripts/setup-msys2.sh          # installs the whole toolchain
cmake --preset dev
cmake --build --preset dev
./build/dev/bin/qimgv.exe
```

`./build/dev/bin/qimgv.exe --build-options` prints Qt versions and every
readable image format. `avif`, `heic`, `jxl`, `webp` should show `[x]`.

Full detail in `docs/BUILDING.md`.

---

## What is actually verified

Linux results are Qt 6.10.1; Windows results are MSYS2 UCRT64, GCC 16.1.0,
Qt 6.11.1.

| Area | Status |
|---|---|
| `dev` + `ci` presets, clean build (Linux) | verified, 0 warnings under `-Wall -Wextra -Wpedantic -Werror` |
| `dev` + `ci` + `windows-msys2`, clean build (Windows) | verified — GCC 16.1.0 / Qt 6.11.1, 0 warnings under `-Wall -Wextra -Wpedantic` |
| Test suite (3 targets) | verified on Linux and Windows, 3/3 pass |
| Animated AVIF | **verified end to end** on Linux *and* Windows — real ffmpeg/libaom file, `QImageReader` reports 10 frames, `QMovie` decodes all 10, and it animates in the running app |
| Windows portable package | verified — `package-windows.sh` produces a `build/dist/` that runs with MSYS2 off `PATH`; avif/heic/jxl/webp/tiff all `[x]` |
| Windows GUI | verified by the maintainer — every keyboard shortcut does its job; rendering, directory scan and title metadata all correct |
| ProRes 4444 alpha *decode* | **verified** — libmpv reports `pixelformat=yuva444p12`, `alpha=straight`, and accepts `background=none` |
| ProRes 4444 alpha *compositing* | **verified** — transparency is visible and composites against the app background. Two bugs found doing it, both fixed; see "Windows bring-up" below. |
| macOS build | **NEVER RUN** |
| GitHub Actions | **NEVER RUN** |

---

## Priority tasks

### 1. ~~Does ProRes 4444 alpha actually composite?~~ Done

Yes. Confirmed on Windows with a `prores_ks -profile:v 4444` alpha ramp: the
transparent side shows the app background and the opaque side shows the video.

If you re-check this, capture the screen (`Graphics.CopyFromScreen`), not the
window (`PrintWindow`). `PrintWindow` renders the window in isolation against
its own background, so a window that is punching a hole through to the desktop
still looks perfectly correct in the capture. Putting a garish window behind
qimgv and confirming none of it bleeds into the video is the reliable check.

Two bugs surfaced while checking it, both fixed:

- The video had **no background at all** underneath it, so the transparent part
  went through the window and showed the desktop.
- Landing on a video **closed and reopened the window**.

Both are described under "Windows bring-up" below.

One thing deliberately left alone: mpv reports `alpha=straight`
(non-premultiplied) while Qt's compositor expects premultiplied, so the blend
is arithmetically slightly off. Every shipped theme uses a near-black viewer
background (`#1a1a1a`, `#18191a`, `#000000` — even `COLORS_LIGHT` keeps the
viewer dark), and against near-black the two formulas are indistinguishable.
It would only show as washed-out edges on a custom light background.

**Do not trust `alpha=yes`.** mpv renamed that option in 0.38; it returns
`-5 option not found` on current libmpv. The code uses `background=none` with
a fallback. All mpv options now go through a checked wrapper — keep it that
way, the original bug was a discarded return value.

### 2. Get CI green

Push the branch and watch. macOS is still completely unexercised.

The Windows build itself is now clean, so the job should mostly work. `-Werror`
is still **off** for Windows and macOS: the local build is warning-free with
GCC 16.1.0, but the runner's toolchain is a different version and a new warning
must not block the artifact on the first ever run. Flip it on once CI has been
green once.

All three unverified assumptions in `scripts/package-windows.sh` turned out to
be correct — `windeployqt6` exists, the kimageformats plugins are at
`/ucrt64/share/qt6/plugins/imageformats`, and `ldd` resolves MinGW PE
dependencies inside the UCRT64 shell. The resulting `build/dist/` is ~344 MB
and runs with MSYS2 off `PATH`; trimming it is worth a look.

### 3. ~~Exercise the app~~ Mostly done

The maintainer has confirmed every keyboard shortcut works, and both previously
broken cases (single-image directory + shuffle, and script persistence) are
covered. Video playback, fit modes, folder view and the panels have all been
driven by hand.

A 20,000-file corpus has since been driven through folder open, scrolling,
thumbnail generation and navigation while the structural problems below were
worked through. HiDPI has been compared at `QT_SCALE_FACTOR=1.5` by rendering
the folder grid offscreen and diffing it, but not yet used by hand at 125%/150%
on a real monitor, which is the remaining gap.

Note for anyone automating this: synthetic input (SendKeys / SendInput, with or
without real scan codes) does not reach the window, so UI checks have to be
done by hand. Non-interactive entry points that do work: `--build-options`,
`--gen-thumbs <dir>`, and passing a file path directly.

---

## Windows bring-up: what had to be fixed

Everything here was found by actually building and running on Windows, and is
fixed on the branch. Listed because most of it is invisible on Linux.

- **`cmake --preset dev` failed to generate.** `mingw-w64-ucrt-x86_64-mujs`
  ships a `.pc` file with hardcoded MSYS paths (`Cflags: -I/ucrt64/include`)
  instead of `${prefix}`-derived ones, so pkgconf has nothing to relocate and a
  native CMake cannot resolve the result. mpv pulls mujs in through
  `Requires.private`, which is why only the mpv plugin broke. Worth reporting
  upstream to MSYS2; `cmake/PkgConfigFixup.cmake` repairs it locally.
- **exiv2 0.28 dropped its wide-path API.** The only `open()` left takes a
  narrow `std::string` that the CRT decodes with the ANSI codepage, so EXIF on
  any path outside CP1252 failed. `main()` now puts `LC_CTYPE` in UTF-8 mode.
  Verified with a Cyrillic + Japanese filename.
- **LTO is off on MinGW.** GCC mis-handles COMDAT sections for
  virtual-destructor thunks; every multiply-inherited widget produced a
  "multiple definition" link error. Not an ODR bug in our code. This makes
  `release`/`windows-msys2` slightly slower than the Linux equivalents.
- **Animated AVIF was silently dead.** `kimageformats` ships `kimg_avif.dll`
  but only *optionally* depends on `libavif`, so the plugin installed and then
  failed to load. Same for HEIF and RAW. Added to both `setup-msys2.sh` and the
  CI job. Always confirm with `--build-options` rather than assuming.
- **Animated AVIF was misdetected as video.** Mime databases sniff ISOBMFF by
  major brand, and ffmpeg writes `avis` for a sequence, which Qt's built-in
  copy of freedesktop.org.xml maps to `video/quicktime` — so the file would
  have gone to mpv instead of `QMovie`. `detectFormat()` now lets the ftyp
  brands override. Two format tests were failing on this.
- **The video had no background under it**, so the transparent parts of a
  ProRes 4444 clip showed the user's desktop. Nothing in the video path painted
  one: `VideoPlayerInitProxy::paintEvent` was empty and the proxy,
  `VideoPlayerMpv` and `MpvWidget` are all `WA_TranslucentBackground`. Harmless
  while mpv composited onto black itself; once `background=none` made the alpha
  real, those pixels went straight through the window.

  The important part: **painting a background on an ancestor widget does not
  work.** With a translucent top-level, Qt hands the QOpenGLWidget's alpha
  directly to the window surface instead of blending it against the backing
  store, so the GL rect punches through regardless of what is painted beneath
  it. Verified in isolation — a parent filled bright red is not visible at all
  through a transparent GL child.

  The composite has to happen inside `MpvWidget::paintGL()`, where we still own
  the alpha: mpv renders (wrapped in `beginNativePainting`), then the app
  background is painted underneath with
  `QPainter::CompositionMode_DestinationOver`. The colour comes from the app
  through `VideoPlayer::setBackgroundColor()` and tracks the theme, fullscreen
  state and `backgroundOpacity` exactly like the image viewer.

  mpv's own letterbox bars are opaque and are not covered by that composite, so
  `background-color` is pushed to mpv as well, otherwise the bars stay black and
  seam against the rest of the window.
- **The plugin ABI had two hand-maintained copies.** `videoplayer.h` and
  `videoplayer.cpp` existed byte-for-byte in both `qimgv/gui/viewers/` and
  `plugins/player_mpv/src/`, defining the same class whose vtable qimgv calls
  across the DLL boundary. Adding one virtual to the app's copy is enough to
  make the two disagree, which is silent memory corruption rather than a build
  error. The plugin now compiles the app's copy; its `src/` duplicates are gone.
- **Landing on a video closed and reopened the window.** `initPlayer()` is
  lazy, so the first video adds the plugin's `QOpenGLWidget` to a window that
  is already on screen; Qt then switches the backing store to a
  texture-composited one, which on Windows destroys and recreates the native
  window. Confirmed in isolation — the `HWND` changes. An empty hidden
  `QOpenGLWidget` parked in the proxy's constructor makes the window
  texture-backed before it is ever shown and the `HWND` stays put. It is never
  laid out or painted, so it creates no GL context. Trade-off: qimgv's window
  is now texture-composited even for people who never open a video.
- **Qt Test output is invisible under ctest on Windows.** Qt routes it to
  `OutputDebugString` whenever stderr is not a tty, so a failing test printed
  nothing but an exit code. The test targets now set `QT_FORCE_STDERR_LOGGING`.

---

## Open decisions for the maintainer

- **`MapOverlay` is dead code.** ~320 lines, a minimap widget, zero
  instantiations anywhere in the tree. It is compiled into the binary. It was
  kept and fixed (its `enterEvent` had the wrong Qt6 signature) only because it
  was the sole thing the pre-existing test covered. Delete it, or wire it up.
- **Version is 1.1.0.** The fork's first release. Upstream was at an unreleased
  1.0.3 and the number had drifted across three files.
- **APNG does not animate** and this is not a bug in qimgv. No maintained Qt 6
  APNG plugin is packaged by mainstream distros. `detectAPNG()` is correct and
  the gate on a present `apng` reader is deliberate — qimgv shows frame 1
  rather than claiming an animation it cannot play. Building
  [QtApng](https://github.com/Skycoder42/QtApng) would fix it.

---

## Known structural problems — all now addressed

Found during the audit, verified in the source, all real. Each was worked
through on the `bug-fixes` branch; what follows is what each one turned out to
be, since two of them were not what the audit assumed. Everything below was
measured on a generated 20,000-file corpus, not estimated.

1. **No thumbnail-view virtualization.** *Skipped, deliberately.* The audit
   called this "almost certainly the largest directory-open cost". Measured, it
   is not: `populate()` for 20k items takes 82 ms in `FolderGridView` and 48 ms
   in `ThumbnailStrip`. The real cost was the directory scan (item 2). Building
   every widget up front does cost memory — 130 MB to 258 MB across 20k — and
   the maintainer's call was that memory is not the constraint for realistic
   folders. Virtualizing a `QGraphicsView` is a large, regression-prone change
   for a cost that was already paid down elsewhere.
2. **Directory scanning was synchronous on the GUI thread.** Enumeration now
   runs on a single-threaded pool and the result is delivered by queued signal;
   a stale scan is discarded by generation counter. Opening a 20k folder went
   from 3.54 s to 0.69 s before the window appears. Sorting stayed on the GUI
   thread on purpose: `QCollator` is not thread-safe, and re-sorting 20k names
   is 45 ms against ~1.8 s to enumerate them. `setDirectoryRecursive()` is
   still blocking — `--gen-thumbs` reads the list on the next line.
3. **`DirectoryPresenter::onThumbnailReady` was O(n²).** `indexOfFile` and
   `indexOfDir` are now backed by lazily-rebuilt path→index hashes, invalidated
   at the 15 sites that mutate the lists. Sweeping `indexOfFile` across 20k
   files: 1885 ms to 1 ms. Navigating a whole folder: 1870 ms to 1 ms.
4. **21 `qApp->processEvents()` call sites.** Not removed — the risk the audit
   flagged is real. Instead every one now passes `ExcludeUserInputEvents`, so a
   nested loop can no longer deliver a click or a keypress into a half-built
   widget tree, and `populate()` refuses re-entry and coalesces the pending
   count.
5. **`ThumbnailerRunnable::generate` built a `QPixmap` on a worker thread.**
   `Thumbnail` now carries a `QImage` and creates the `QPixmap` on first use,
   which is always on the GUI thread.
6. **`ImageLib::scaled_CV` had a dead branch.** The equal-size case now returns
   a copy of the source instead of a null image.
7. **Thumbnail cache saved PNG at "quality 15".** Now 50. Per 200x200
   thumbnail: 2.99 ms to 1.12 ms, for 9149 to 9937 bytes. Past 80 compression
   effectively switches off and the same file becomes 160 KB.
8. **`DeviceCoordinateCache` disabled on fractional DPI.** *The workaround was
   kept, and a bug next to it fixed.* Rendering the grid at
   `QT_SCALE_FACTOR=1.5` with the cache on and off, 13.5% of pixels differ by
   up to 231/255 — the cache still lands the item on a different pixel grid on
   Qt 6.11, so enabling it would have been wrong. The actual defect was that
   the check only ever switched the cache *on*: a widget built while the
   primary screen was at 100% kept its cache after moving to a 150% monitor.

Two other bugs surfaced while working through the above and are fixed:
`--gen-thumbs` silently generated nothing after the scan went async, and the
transparency grid never reached video with an alpha channel.

---

## Conventions

- `cmake --build --preset ci` (warnings are errors on Linux) and
  `ctest --preset ci` before any PR.
- Do not reformat files wholesale. `.clang-format` matches the existing style;
  CI only checks files a PR touches.
- If you touch loading, scaling, caching, thumbnailing or directory scanning,
  measure it. See `CONTRIBUTING.md`.
- Tests `QSKIP` when an image plugin is missing rather than failing.
