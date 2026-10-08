#pragma once

#include <QString>
#include <QStringList>
#include <functional>

// Where the Play folder playback mode goes from one audio file to the next. Pure functions over the
// folder's file list, so the edge cases are testable without a model, settings or a player. Shuffle keeps
// its own order (AudioShuffle) but plays from the same pool.
namespace AudioNavigation {
// The lower-case extension of a path's file name, without the dot; empty if it has none.
QString suffix(QStringView path);

// The files among `files` (the whole folder, in folder order) that may be audio, assuming `current` is one:
// `current` itself, and every file whose extension `mayBeAudio` accepts. Decided by name alone; nothing is
// read from disk. A name cannot settle it -- an audio-only .webm has a video's extension, a Theora .ogg an
// audio one's -- so this is only the shortlist, and pick() has the last word.
QStringList pool(QStringList const &files, QString const &current,
                 std::function<bool(QString const &suffix)> const &mayBeAudio);

// The file `offset` places away from `current` in `pool`, wrapping around at either end. When `current`
// is not in the pool, a step forward lands on its first file and a step back on its last. Empty only for
// an empty pool.
QString step(QStringList const &pool, QString const &current, int offset);

// The first track `next` offers from `pool` that `accept` takes. A track turned down leaves the pool before
// `next` is asked again, so none is tested twice and the search ends even when nothing qualifies. What
// `next` offers that is empty or `current` comes back untested: there is nowhere else to go. Empty if
// everything in the pool was turned down.
QString pick(QStringList pool, QString const &current, std::function<QString(QStringList const &pool)> const &next,
             std::function<bool(QString const &path)> const &accept);
} // namespace AudioNavigation
