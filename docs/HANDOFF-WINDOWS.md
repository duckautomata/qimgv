# Handoff: continuing qimgv from Windows

Written 2026-08-20 at the end of the `modernize` branch work. Delete this file
once the open items below are closed — it is a snapshot, not documentation.

---

## Situation

`duckautomata/qimgv` is a fork of `easymodo/qimgv` (unmaintained). The
`modernize` branch (9 commits) did the build-system, Qt6, codec and CI work.

**All of it was done on a headless Linux server.** The maintainer has no
display there, so nothing visual has ever been looked at, and neither CI, the
Windows build, nor macOS has ever run. Windows is now the primary dev
environment because it is the primary target and the only place the UI, video
playback and transparency can actually be evaluated.

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

Everything below was verified on Linux/Qt 6.10.1 unless stated.

| Area | Status |
|---|---|
| `dev` + `ci` presets, clean build | verified, 0 warnings under `-Wall -Wextra -Wpedantic -Werror` |
| Test suite (3 targets) | verified, 3/3 pass |
| Animated AVIF | **verified end to end** — real ffmpeg/libaom file, `QImageReader` reports 10 frames, `QMovie` decodes all 10 |
| ProRes 4444 alpha *decode* | **verified** — libmpv reports `pixelformat=yuva444p12`, `alpha=straight`, and accepts `background=none` |
| ProRes 4444 alpha *compositing* | **NOT VERIFIED** — needs a display. See task 1. |
| Windows build | **NEVER RUN** |
| macOS build | **NEVER RUN** |
| GitHub Actions | **NEVER RUN** |
| Any UI behaviour at all | **NEVER LOOKED AT** |

---

## Priority tasks

### 1. Does ProRes 4444 alpha actually composite?

The highest-value unknown, and an explicit product requirement.

Generate the test file (ffmpeg required):

```bash
ffmpeg -f lavfi -i "testsrc2=size=128x128:rate=10:duration=1,format=yuva444p10le,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*X/W'" \
  -c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le -alpha_bits 16 prores4444.mov
```

It is a left-to-right alpha ramp: fully transparent at the left edge, opaque at
the right. Open it in qimgv.

- **Left side shows the app background through it** → working, close this out.
- **Left side is black** → decode is fine (proven), compositing is not. Most
  likely cause: mpv reports `alpha=straight` (non-premultiplied) while GL
  blending expects premultiplied. Look at `MpvWidget::paintGL` in
  `plugins/player_mpv/src/mpvwidget.cpp` — the FBO is declared `GL_RGBA8` and
  the surface format requests `alphaBufferSize(8)`, but nothing premultiplies.
  Also check whether `WA_AlwaysStackOnTop` is needed on the QOpenGLWidget —
  it was deliberately NOT set, because it would draw the video over the video
  controls overlay.

**Do not trust `alpha=yes`.** mpv renamed that option in 0.38; it returns
`-5 option not found` on current libmpv. The code uses `background=none` with
a fallback. All mpv options now go through a checked wrapper — keep it that
way, the original bug was a discarded return value.

### 2. Get CI green

Push the branch and watch. Expect the Windows job to need iteration.

`-Werror` is intentionally **off** for Windows and macOS — those sources have
never seen `-Wall -Wextra`, and a new warning must not block the artifact. The
job prints a warning count instead. I preemptively fixed two certain failures
in the Windows watcher (`OVERLAPPED ovl = {0}`, an unused `Q_D`), but the
Windows branches inside shared files are unaudited. Once Windows is clean,
turn `-Werror` on for it.

Unverified assumptions in `scripts/package-windows.sh`:
- `windeployqt6` is the right binary name under MSYS2 (could be `windeployqt`)
- kimageformats plugins live at `/ucrt64/share/qt6/plugins/imageformats`
- `ldd` resolves MinGW PE dependencies inside the UCRT64 shell

### 3. Exercise the app

Nobody has looked at this build. Worth an hour: open a large folder, switch fit
modes, crop/resize/rotate, folder view, quick copy/move panels, settings,
fullscreen, HiDPI at 125%/150%, and video playback.

Two specific things to try, both previously broken:
- A directory with **exactly one image**, then toggle shuffle mode. This hung
  the app before (`Randomizer` infinite loop). There is a regression test, but
  confirm in the real app.
- Save a script under Settings → Scripts, restart, confirm it persisted. The
  `Script` QDataStream operators were an ODR violation that worked only by
  link order.

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

## Known structural problems (deliberately not touched)

Found during the audit, verified in the source, all real. Each is a project
rather than a cleanup, and the maintainer's stated top priority is speed.
Roughly in impact order:

1. **No thumbnail-view virtualization.** `ThumbnailView::populate()`
   (`qimgv/gui/customwidgets/thumbnailview.cpp`) eagerly constructs one
   `ThumbnailWidget` QGraphicsItem per file and inserts it into the layout. A
   20k-file folder is 20k items built on the GUI thread before a single
   thumbnail is even requested. Almost certainly the largest directory-open
   cost.
2. **Directory scanning is synchronous on the GUI thread.**
   `Core::setDirectory` → `DirectoryModel::setDirectory` →
   `DirectoryManager::setDirectory`. No worker thread anywhere in that path.
3. **`DirectoryPresenter::onThumbnailReady` is O(n²).** It calls
   `indexOfFile()` per delivered thumbnail, which is a linear
   `std::find_if` over the whole vector. Needs a path→index hash.
4. **21 `qApp->processEvents()` call sites**, several inside `populate()` —
   i.e. nested event loops during folder loading. Risky to remove, easy to
   deadlock on.
5. **`ThumbnailerRunnable::generate` builds a `QPixmap` on a worker thread**
   (`new QPixmap(size)` then immediately overwritten by
   `QPixmap::fromImage`). QPixmap off the GUI thread is unsupported in Qt, and
   the sized ctor allocates a full buffer that is discarded one line later.
6. **`ImageLib::scaled_CV` has a dead branch**: when `destSize == source
   size` it writes nothing and returns an empty QImage. A no-op scale request
   yields a null image.
7. **Thumbnail cache saves PNG at "quality 15"**, which Qt maps *inversely*
   onto zlib compression — that is near-maximum CPU per thumbnail write. A
   one-line change worth measuring.
8. **`DeviceCoordinateCache` is disabled on fractional DPI**
   (`thumbnailwidget.cpp`, guarded by `trunc(dpr) == dpr`) — so the common
   HiDPI case gets no item caching at all.

The scan micro-optimizations that *were* done are worth ~3.4 ms per 20k files.
Measured, and small. Do not expect them to matter next to items 1–3.

---

## Conventions

- `cmake --build --preset ci` (warnings are errors on Linux) and
  `ctest --preset ci` before any PR.
- Do not reformat files wholesale. `.clang-format` matches the existing style;
  CI only checks files a PR touches.
- If you touch loading, scaling, caching, thumbnailing or directory scanning,
  measure it. See `CONTRIBUTING.md`.
- Tests `QSKIP` when an image plugin is missing rather than failing.
