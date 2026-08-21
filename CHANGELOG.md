# Changelog

Notable changes per release. Newest first.

The heading for each version is what qimgv shows in its "what's new" window
after an update, so keep them as `## <version>` and keep the prose readable by
someone who is not following the commit log.

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
