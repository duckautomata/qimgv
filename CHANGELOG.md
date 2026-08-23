# Changelog

Notable changes per release. Newest first.

The heading for each version is what qimgv shows in its "what's new" window
after an update, so keep them as `## <version>` and keep the prose readable by
someone who is not following the commit log.

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
