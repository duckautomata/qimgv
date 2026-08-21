# Installing qimgv

Downloads are on the [releases page](https://github.com/duckautomata/qimgv/releases/latest).
Windows builds are published; Linux and macOS are built from source for now.

## Which download?

| File | Size | Use it if |
|---|---|---|
| `qimgv-<version>-win64-setup.exe` | ~88 MB | You want it installed like a normal app. **Start here.** |
| `qimgv-<version>-win64.zip` | ~131 MB | You want it portable — on a USB stick, or with no traces on the machine. |
| `qimgv-<version>-win64-minimal.zip` | ~61 MB | You only view images and want the smallest download. |

All three read the same image formats, including AVIF, HEIF and JPEG XL.

The **minimal** build drops video playback and the OpenCV high-quality scaling
filters. That is where the size difference comes from: a bundled video stack and
OpenCV's numerical library are together larger than the rest of the application.
If you open videos in qimgv, or you use the bicubic/sharpen scaling filters, take
the full build.

64-bit Windows 10 or later. There is no 32-bit or ARM build.

## Installing with the installer

Run `qimgv-<version>-win64-setup.exe`.

It installs for the current user under `%LOCALAPPDATA%\Programs\qimgv`, so there
is no administrator prompt. It appears in **Apps & Features** like any other
program, and the Select Additional Tasks page offers:

- a Start menu shortcut (on by default)
- a desktop shortcut (off)
- an "Open with" entry, so qimgv shows up in Explorer's Open with menu (off)

Windows may warn that the publisher is unknown, because the installer is not code
signed. Certificates cost money and are tied to a verified identity; for now the
[SHA256SUMS.txt](#verifying-your-download) published with each release is how you
confirm you got the real file.

### File associations

The installer does **not** make qimgv the default for any file type. Since
Windows 8, applications cannot set that themselves — the choice belongs to you,
and programs that force it get reset by Windows anyway.

What it does do is register qimgv as an app you can *choose*. To make it the
default:

**Settings → Apps → Default apps → qimgv**, then assign the types you want. Or
right-click any image → **Open with → Choose another app**.

## Installing the portable build

Extract the zip anywhere and run `qimgv.exe`. Nothing is written outside that
folder: settings, cache and thumbnails all live in `conf`, `cache` and
`thumbnails` next to the executable.

The `conf` folder is what makes it portable. If you delete it, qimgv switches to
the per-user locations below.

## Upgrading

qimgv shows what changed the first time you run a new version, and can check for
updates itself — **Settings → About → Check for updates**. That check is off by
default and never downloads anything; it tells you a new version exists and links
to the releases page.

**Installed:** download the new installer and run it. It replaces the existing
install in place and keeps your settings — no need to uninstall first.

**Portable:** extract the new zip to a *new* folder, then copy your old `conf`
folder into it. Extracting over the top also works and keeps your settings, but
leaves files from the old version behind.

## Where your settings live

| | Installed | Portable |
|---|---|---|
| Settings | `%LOCALAPPDATA%\qimgv` | `conf` next to `qimgv.exe` |
| Thumbnails and cache | `%LOCALAPPDATA%\cache\qimgv` | `cache` and `thumbnails` next to `qimgv.exe` |

## Uninstalling

**Apps & Features → qimgv → Uninstall**, or run `unins000.exe` from the install
folder.

This removes the program, its shortcuts, its registry entries and the thumbnail
cache it generated. Your settings in `%LOCALAPPDATA%\qimgv` are left alone, so
reinstalling keeps your configuration. Delete that folder yourself if you want a
clean slate.

For the portable build, delete the folder.

## Verifying your download

Every release publishes `SHA256SUMS.txt`. To check a file in PowerShell:

```powershell
Get-FileHash .\qimgv-2.0.0-win64-setup.exe -Algorithm SHA256
```

Compare the result against the matching line in `SHA256SUMS.txt`.

## Linux and macOS

No binaries yet. Build from source — see [BUILDING.md](BUILDING.md); the presets
mean it is usually two commands.

`qimgv` in your distribution's repositories is the **upstream** package
(`easymodo/qimgv`), not this fork.
