# Changelog

Notable changes per release. Newest first.

The heading for each version is what qimgv shows in its "what's new" window
after an update, so keep them as `## <version>` and keep the prose readable by
someone who is not following the commit log.

## 2.1.0

**Audio**

- qimgv now plays audio files. Open one as you would an image and it plays in
  a new audio view: the album art, the title, artist and album, the format
  (for example "FLAC · 44.1 kHz · 16-bit · Stereo"), a seek bar, and buttons
  for previous, play/pause, next, mute and volume.
- Most audio formats play, through the same mpv and ffmpeg that play video:
  MP3, AAC and M4A (including Apple Lossless), FLAC, Ogg Vorbis, Opus, WAV,
  AIFF, WMA, Monkey's Audio (APE), WavPack, Musepack, TTA, DSD (DSF and DFF),
  AC-3, DTS, Matroska and WebM audio, tracker modules, and more.
- Album art comes from the file's own tags, or from a picture next to it such
  as `cover.jpg` or `folder.png`. It is also the file's thumbnail in the folder
  view and the thumbnail panel, and that thumbnail follows the picture if you
  replace it. A file without art gets a music note instead.
- By default an audio file behaves like any other file: next and previous go
  to the adjacent file, whatever its type, and the track stops when it ends.
  Press `A` on an audio file or in the folder view, or click the mode button
  in the audio view, to switch to another mode. qimgv remembers the one you
  pick.
  - **Single file** — the default, as above.
  - **Repeat track** — plays the track over and over.
  - **Play folder** — plays the folder's audio files in order, starting again
    after the last one. Next and previous skip images and videos.
  - **Shuffle** — plays the folder's audio files in random order, each one
    once before any repeats. Previous goes back through what has played.
- In Play folder and Shuffle, a file that cannot be played is skipped, and if
  none of them can, playback stops and says so. Pausing near the end of a
  track holds it there instead of moving on.
- Music keeps playing while you browse its folder in the folder view, until
  you open another file or go to another folder. Selecting the track that is
  playing brings the audio view back without restarting it.
- `Space` pauses and resumes, `Ctrl+Left` / `Ctrl+Right` skip back and forward
  10 seconds as for video, and `,` / `.` skip 5 seconds. The volume is shared
  with videos.
- In a slideshow, a track plays to its end before the slideshow moves on, as
  videos do.
- A file that cannot be played says why instead of showing an empty view.
- On Windows, the keyboard's play/pause, next and previous keys control a
  video or audio file while one is open in qimgv; the rest of the time they go
  on to the system and other players, as before. (On Linux and macOS this
  depends on whether the desktop passes them on.) A media key you have already
  bound to something else keeps your binding. The volume keys are left to the
  system, and any media key can now be assigned under Settings → Controls.
- The file info panel (`I`) shows an audio file's tags and describes its
  embedded album art.
- Settings → General has a new Audio playback section, where you can turn off
  audio playback or the blurred album art behind the audio view. "Play sounds"
  is now called "Play sounds in videos", because it does not affect audio
  files.

**Format detection**

- Audio-only `.webm`, `.mp4`, `.mkv` and `.ogg` files open in the audio view
  instead of as a video with no picture. qimgv looks inside a file to tell
  audio from video rather than going by its extension.
- Opening an `.ogg` file directly, from Explorer or the command line, no
  longer fails.
- The format shown for a video in the file info panel and under its thumbnail
  is now its own extension: MP4 rather than M4V or QT, OGV rather than OGG.

**Fixes**

- Pressing next or previous straight after opening a file from another folder
  used to step through the folder you were in before. It now waits for the
  new folder, saying "Loading folder..." until it is ready.

**Installing**

- The Windows installer also offers qimgv for audio files in Settings →
  Default apps. As before, it does not make itself the default for anything.
  The Linux desktop entry and the macOS bundle list the audio types too.
- The minimal Windows package has no audio playback, just as it has no video.

## 2.0.3

**Video**

- Videos can now be zoomed and panned. Zoom with `+` and `-`, with Ctrl+wheel
  (at the pointer), or by dragging up and down with the right mouse button;
  the wheel with the right button held works too. Once a video is larger than
  the window, drag it around with the left mouse button, or scroll it with the
  Up and Down keys.
- Every video opens fitted to the window, and the zoom resets when you move to
  the next one. The fit shortcuts and Lock zoom only apply to images.
- Clicking a video zoomed larger than the window still pauses it, but on
  release rather than on press, so that dragging it does not.
- Moving from one video to the next used to show the previous video's last
  frame while the new one loaded. It now shows the background until the new
  video is ready, and a video that fails to open no longer leaves the previous
  one on screen.

## 2.0.2

**File info**

- The I key now opens a file info panel instead of a short list of EXIF tags.
  It shows the file's name, size and location; the format worked out from the
  file's contents rather than its extension, with its media type, resolution
  and codec; and all of the Exif, IPTC and XMP metadata it carries. Previously
  only nine EXIF tags were shown, and IPTC and XMP were never read at all.
- Values are readable rather than raw -- "Flash: Yes, auto, red-eye reduction"
  instead of "Flash: 89".
- The panel scrolls, so a file with a lot of metadata is no longer cut off.
- Reading metadata no longer holds up the interface, and no longer happens at
  all unless the panel is open. Every image you opened used to have its
  metadata read whether or not you were looking at it.

**Image fit**

- New **Lock zoom** fit mode: keeps the zoom level you are on rather than
  re-fitting each image as you move through a folder. Choose it in Settings, in
  the right-click menu, or with `0`.
- "Fit in window (stretch)" is now **Stretch to height**, which is what it
  actually does, and it sits next to "Stretch to width".
- Choosing "Stretch to height" in Settings had no effect. Fixed.
- The fit shortcuts are now `0` lock zoom, `1` fit window, `2` stretch to
  width, `3` stretch to height, `4` 1:1. Note that `3` and `4` have swapped
  meaning. Shortcuts you set yourself are kept as they are.

## 2.0.1

Mostly about large folders and slow storage. Reading a folder no longer blocks
the window, which is what made qimgv unusable on a slow network share.

**Large folders and slow storage**

- Reading a folder now happens on a background thread. The window stays
  responsive while it works instead of locking up until the folder has been
  read. On a slow network share that was the difference between a window you
  could not move and one you can use.
- While a folder is still being read, next and previous image now say
  "Loading folder..." rather than appearing to do nothing.
- Opening a large folder is faster: a folder of 20,000 images shows a window in
  about 0.7 seconds, down from 3.5.
- Scrolling a large folder is roughly three times cheaper per scroll, and
  generating thumbnails is about twice as fast.

**Fixes**

- The transparency grid now shows through video that has an alpha channel, such
  as ProRes 4444. The setting previously affected images only.
- Thumbnails no longer keep a rendering cached for the wrong display scale after
  the window moves to a monitor at 125% or 150%.
- The folder watcher no longer closes an uninitialised handle when it first
  starts.
- Thumbnail images are no longer converted on worker threads, which Qt does not
  support and which could misbehave in ways that were hard to reproduce.
- Asking to scale an image to the size it already is returns the image instead of
  a blank one.

## 2.0.0

First release of the `duckautomata` fork. The major version separates it from
`easymodo/qimgv`, which stopped at an unreleased 1.0.3, rather than implying a
continuation of it.

**Codecs**

- Animated AVIF, HEIF and JPEG XL support.
- ProRes 4444 and other alpha video now composites against the application
  background instead of showing black or, briefly, your desktop.
- AVIF and HEIF image sequences are detected by their container brands, so an
  animated AVIF written by ffmpeg is no longer mistaken for a video file.

**Windows**

- A per-user installer, alongside the portable zip. It needs no administrator
  rights, appears in Apps & Features, and registers qimgv as an app you can
  assign file types to in Settings > Default apps. It sets no defaults itself.
- Settings, cache and thumbnails move to the usual per-user locations when
  qimgv is installed. A copy with a `conf` folder next to the executable — which
  is what the zip ships — stays fully portable, as before.
- Fixed the window closing and reopening the first time you opened a video.
- A `minimal` package without video support, for anyone who wants a smaller
  download.

**General**

- Qt 6 only; Qt 5 support has been dropped.
- Rebuilt CMake build with presets, and CI covering Windows, Linux and macOS.
- Optional update check, off by default, on the About page.
- Fixed a hang when shuffling in a folder containing a single image.
- Fixed saved scripts not persisting across restarts.
