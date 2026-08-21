# Contributing to qimgv

Thanks for taking an interest. This is a fork of [easymodo/qimgv](https://github.com/easymodo/qimgv),
maintained because the original is no longer active.

## Getting set up

```bash
git clone https://github.com/duckautomata/qimgv.git
cd qimgv
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Dependencies and per-platform notes: **[docs/BUILDING.md](docs/BUILDING.md)**.

`compile_commands.json` is generated automatically, so clangd works out of the box. Install `ccache`
and `mold` if you can — the build picks them up and iteration gets noticeably faster.

## Before you open a PR

```bash
cmake --build --preset ci     # warnings are errors here
ctest --preset ci
```

CI runs the same thing on Linux (Qt 6.5 and current), Windows, and macOS. Getting `ci` green locally
means CI will almost certainly agree.

## Code style

`.clang-format` matches the existing codebase — 4 spaces, attached braces, `if(cond)` with no space
before the paren, 120 column limit. Format only the lines you touch:

```bash
git clang-format
```

**Please don't reformat files wholesale.** The tree isn't fully formatted yet, and a blanket reformat
buries real changes in noise. CI only checks files your PR modifies.

Other conventions:

- Match the surrounding code. This codebase has its own idioms; consistency beats personal preference.
- C++20 is available. Use it where it makes the code clearer, not to show it off.
- Comment *why*, not *what*. A comment explaining a non-obvious workaround is worth a lot; one
  restating the code is worth less than nothing.

## Performance

Speed is the point of this project. If a change touches image loading, scaling, caching, thumbnailing,
or directory scanning, say what you measured in the PR. A rough before/after on a large directory is
fine — an unmeasured "should be faster" is not.

Things that have bitten us before:

- Calling `QImageReader::supportedImageFormats()` per file (it walks the plugin loader every time)
- Rebuilding a `QRegularExpression` inside a loop
- Full-size `QImage`/`QPixmap` allocations that are immediately overwritten
- `qApp->processEvents()` in a hot path

## Tests

There's a small ctest suite under `qimgv/tests/`. It's not comprehensive, and growing it is welcome —
particularly for anything with tricky edge cases (format sniffing, index math, file operations).

Tests run headless (`QT_QPA_PLATFORM=offscreen`). If a test needs an image plugin that may not be
installed, `QSKIP` rather than fail — see `test_formatdetect.cpp` for the pattern.

If you're fixing a bug, a test that fails before your fix and passes after is the most useful thing
you can include.

## Reporting bugs

Include:

- OS and version
- qimgv version (`qimgv --version`) and how you installed it
- Output of `qimgv --build-options` — this shows which optional features and image plugins are active
- For format problems: the file, or at least `file <name>` output and the exact format

Most "won't open this image" reports turn out to be a missing Qt image plugin. Check
`qimgv --build-options` first.

## Scope

This fork aims to keep qimgv fast and current, not to redesign it. Bug fixes, codec support, build and
packaging improvements, and performance work are all welcome. Large UI redesigns are a harder sell —
open an issue to discuss before writing the code.

## License

Contributions are accepted under [GPL-3.0-or-later](LICENSE), the project's existing license.
