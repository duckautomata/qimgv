#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

// Finds and loads the player plugin (player_mpv) for both of its users: the video player and the audio
// player. It is loaded on first use and never unloaded -- objects created from it may live until exit.
namespace PlayerPlugin {
// The plugin's file name; empty in a build without video support.
QString fileName();
// Where it is looked for, in order.
QStringList searchDirs();
// Resolves `symbol`, loading the plugin on first use. nullptr when there is no plugin (a build without
// video support, a missing file, one that will not load) or it does not export `symbol` (a plugin from
// another version of qimgv). GUI thread only.
QFunctionPointer resolve(const char *symbol);
// Why resolve() returned nullptr, for showing in place of a player: the directories searched, the reason
// the plugin would not load, or that it is from another version of qimgv.
QString loadError();
} // namespace PlayerPlugin
