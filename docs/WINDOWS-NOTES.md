# Windows notes

Why parts of qimgv are built the way they are on Windows, and what has been
measured. This is reference material, not a task list.

- Building and toolchain setup: [BUILDING.md](BUILDING.md)
- Contribution rules, code style, the measurement expectation: [CONTRIBUTING.md](../CONTRIBUTING.md)
- Installing a release: [INSTALL.md](INSTALL.md)

Windows is the supported target. Linux and macOS are built by CI as a check that
the code stays portable, but no release artifacts are produced for them.

---

## Platform quirks

Each of these was found by building and running on Windows, and each is fixed.
They are written down because none of them reproduces on Linux, so the reasoning
behind the workaround is not recoverable from the code alone.

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

## Performance: what was measured and what was decided

An audit listed eight structural problems; all eight were worked through. What
follows is what each turned out to be, because two were not what the audit
assumed and one was deliberately left alone. Everything here was measured on a
generated 20,000-file corpus, not estimated -- the numbers are the reason the
code is shaped the way it is, so change them only with new measurements.

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
   Since confirmed against the slow NAS this was written for, side by side with
   2.0.0: that release freezes the window until the listing lands, the dev build
   stays responsive throughout. This was the hang. The synchronous decode in
   `Core::loadPath()` is still there and is *not* what the maintainer was hitting
   — worth revisiting only if an uncached file over a slow share turns out to
   freeze the window on its own, which has not been observed.
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

## Verified by hand

Things a person sat down and checked, which CI does not cover and which are not
re-derivable from the source.

| Area | Status |
|---|---|
| Animated AVIF | **verified end to end** on Linux *and* Windows — real ffmpeg/libaom file, `QImageReader` reports 10 frames, `QMovie` decodes all 10, and it animates in the running app |
| ProRes 4444 alpha *decode* | **verified** — libmpv reports `pixelformat=yuva444p12`, `alpha=straight`, and accepts `background=none` |
| ProRes 4444 alpha *compositing* | **verified** — transparency is visible and composites against the app background. Two bugs found doing it, both fixed; see "Platform quirks" above. |
| Directory scan on a slow network share | **verified, A/B against 2.0.0** — 2.0.0 locks the window until the listing finishes, then snaps back. 2.0.1 stays responsive throughout, zoom and pan included. Only next/previous image is inactive, for the few seconds the listing takes, and it now says so. |
| Windows GUI | verified — every keyboard shortcut does its job; rendering, directory scan and title metadata all correct |
| Windows portable package | verified — `package-windows.sh` produces a `build/dist/` that runs with MSYS2 off `PATH`; avif/heic/jxl/webp/tiff all `[x]` |

Still unexercised: HiDPI at 125%/150% used by hand on a real monitor. It has
been compared at `QT_SCALE_FACTOR=1.5` by rendering the folder grid offscreen and
diffing it, which is not the same thing.

---

## Known limitations

- **APNG does not animate**, and this is not a bug in qimgv. No maintained Qt 6
  APNG plugin is packaged by mainstream distros. `detectAPNG()` is correct and
  the gate on a present `apng` reader is deliberate — qimgv shows frame 1 rather
  than claiming an animation it cannot play. Building
  [QtApng](https://github.com/Skycoder42/QtApng) would fix it.
- **`MapOverlay` is dead code.** ~320 lines, a minimap widget, zero
  instantiations anywhere in the tree, still compiled into the binary. It was
  kept and fixed (its `enterEvent` had the wrong Qt6 signature) only because it
  was the sole thing the pre-existing test covered. Delete it, or wire it up.
- **LTO is off on MinGW**, so `release` and `windows-msys2` builds are slightly
  slower than the Linux equivalents. See "Platform quirks" for why.

---

## Driving the app for a test

Synthetic input — `SendKeys`, `SendInput`, with or without real scan codes —
does not reach the window. Anything that needs a keypress or a click has to be
checked by hand.

Non-interactive entry points that do work, and are worth reaching for first:

```bash
qimgv.exe --build-options        # Qt versions and every readable image format
qimgv.exe --gen-thumbs <dir>     # thumbnails for a directory, recursively
qimgv.exe <file>                 # opens a path directly
qimgv.exe --version
```

Qt Test output is invisible under ctest on Windows unless
`QT_FORCE_STDERR_LOGGING` is set; the test targets already set it.
