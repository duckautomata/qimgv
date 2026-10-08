#include "playerplugin.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

#ifdef _QIMGV_PLAYER_PLUGIN
#define QIMGV_PLAYER_PLUGIN _QIMGV_PLAYER_PLUGIN
#else
#define QIMGV_PLAYER_PLUGIN ""
#endif

namespace PlayerPlugin {

QString fileName() {
    return QStringLiteral(QIMGV_PLAYER_PLUGIN);
}

QStringList searchDirs() {
    QStringList dirs;
#ifdef _WIN32
    dirs << QCoreApplication::applicationDirPath() + "/plugins";
#else
    QDir libPath(QCoreApplication::applicationDirPath() + "/../lib/qimgv");
    dirs << (libPath.makeAbsolute() ? libPath.path() : ".") << "/usr/lib/qimgv" << "/usr/lib64/qimgv";
#endif
    return dirs;
}

#ifdef USE_MPV
// One instance for the whole process: QLibrary reference-counts loads of the same file anyway, and the
// video and audio players must agree on which copy of the plugin they are talking to.
static QLibrary &library() {
    static QLibrary lib;
    return lib;
}

static bool load() {
    QLibrary &lib = library();
    if(lib.isLoaded())
        return true;
    if(fileName().isEmpty())
        return false;
    if(lib.fileName().isEmpty()) {
        for(QString const &dir : searchDirs()) {
            QFileInfo candidate(dir + "/" + fileName());
            if(candidate.isFile() && candidate.isReadable()) {
                lib.setFileName(candidate.absoluteFilePath());
                break;
            }
        }
        if(lib.fileName().isEmpty()) {
            qDebug() << "Could not find" << fileName() << "in the following directories:" << searchDirs();
            return false;
        }
    }
    if(!lib.load()) {
        qDebug() << "Could not load" << lib.fileName() << ":" << lib.errorString();
        return false;
    }
    return true;
}
#endif

QFunctionPointer resolve(const char *symbol) {
#ifndef USE_MPV
    Q_UNUSED(symbol)
    return nullptr;
#else
    if(!load())
        return nullptr;
    QFunctionPointer fn = library().resolve(symbol);
    if(!fn)
        qDebug() << library().fileName() << "does not export" << symbol << "- wrong plugin version?";
    return fn;
#endif
}

QString loadError() {
#ifdef USE_MPV
    QLibrary const &lib = library();
    // Loaded, so it lacks the symbol asked for: an older plugin next to a newer qimgv (only the exe copied
    // over a portable install), or a stale one found first on Linux. Pointing at the search path instead
    // would send the user looking for a file that is right there.
    if(lib.isLoaded()) {
        return QCoreApplication::translate("PlayerPlugin", "%1 is from another version of qimgv:\n%2\nReinstall qimgv.")
            .arg(fileName(), QFileInfo(lib.fileName()).absolutePath() + "/");
    }
    // Found but refused, usually for a DLL of libmpv's that is missing; the system says which file it tried.
    if(!lib.fileName().isEmpty())
        return lib.errorString();
#endif
    QString text = QCoreApplication::translate("PlayerPlugin", "Could not load %1 from:").arg(fileName());
    for(QString const &dir : searchDirs())
        text.append("\n" + dir + "/");
    return text;
}

} // namespace PlayerPlugin
