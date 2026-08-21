#include "cmdoptionsrunner.h"

#include <QImageReader>
#include <QLibraryInfo>
#include <QTextStream>

#include "appversion.h"

// CLI output goes to stdout, NOT qDebug.
//
// Several distributions (Fedora, RHEL, Rocky, ...) ship a Qt-wide
// /usr/share/qt6/qtlogging.ini containing `*.debug=false`, which silently
// discards every qDebug() message. These subcommands used to print through
// qDebug and therefore produced no output at all on those systems.
static QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

void CmdOptionsRunner::generateThumbs(QString dirPath, int size) {
    if(size <= 50 || size > 400) {
        out() << "Error: invalid thumbnail size.\n"
              << "Please specify a value between 50 and 400.\n"
              << "Example: qimgv --gen-thumbs=/home/user/Pictures --gen-thumbs-size=120\n";
        out().flush();
        QCoreApplication::exit(1);
        return;
    }

    Thumbnailer th;
    DirectoryManager dm;
    if(!dm.setDirectoryRecursive(dirPath)) {
        out() << "Error: invalid path: " << dirPath << "\n";
        out().flush();
        QCoreApplication::exit(1);
        return;
    }

    auto list = dm.fileList();
    out() << "\nDirectory:  " << dirPath
          << "\nFile count: " << list.size()
          << "\nSize limit: " << size << "x" << size << " px"
          << "\nGenerating thumbnails...\n";
    out().flush();

    for(auto path : list)
        th.getThumbnailAsync(path, size, false, false);

    th.waitForDone();
    out() << "Done.\n";
    out().flush();
    QCoreApplication::quit();
}

// Printed by `qimgv --build-options`. This is what we ask for in bug reports,
// so it should say enough to diagnose a "format X won't open" issue without a
// follow-up round trip.
void CmdOptionsRunner::showBuildOptions() {
    out() << "qimgv " << appVersion.toString() << "\n\n";

    out() << "Qt:\n"
          << "   compiled against  " << QT_VERSION_STR << "\n"
          << "   running against   " << qVersion() << "\n"
          << "   plugin path       " << QLibraryInfo::path(QLibraryInfo::PluginsPath) << "\n\n";

    QStringList features;
#ifdef USE_MPV
    features << "USE_MPV       (video playback via libmpv)";
#endif
#ifdef USE_EXIV2
    features << "USE_EXIV2     (EXIF metadata)";
#endif
#ifdef USE_OPENCV
    features << "USE_OPENCV    (high quality scaling)";
#endif
#ifdef USE_KDE_BLUR
    features << "USE_KDE_BLUR  (KWin background blur)";
#endif
    out() << "Build options:\n";
    if(features.isEmpty())
        out() << "   (none)\n";
    for(const auto &f : features)
        out() << "   " << f << "\n";

    // The single most useful line in a bug report: which image formats this
    // install can actually read. Missing plugins are by far the most common
    // cause of "qimgv won't open my file".
    QStringList formats;
    for(const auto &f : QImageReader::supportedImageFormats())
        formats << QString::fromLatin1(f);
    out() << "\nReadable image formats (" << formats.size() << "):\n   "
          << formats.join(QStringLiteral(" ")) << "\n";

    // Where each notable format comes from, so a missing one points at the
    // package that supplies it rather than at a bug.
    static const struct { const char *format; const char *source; } kNotable[] = {
        {"webp", "qt6-imageformats"},
        {"tiff", "qt6-imageformats"},
        {"avif", "kimageformats"},
        {"heic", "kimageformats"},
        {"jxl",  "kimageformats"},
        {"apng", "QtApng (github.com/Skycoder42/QtApng) -- not packaged by most distros"},
    };
    out() << "\n";
    for(const auto &entry : kNotable) {
        const bool have = formats.contains(QString::fromLatin1(entry.format));
        out() << "   " << (have ? "[x] " : "[ ] ")
              << QString::fromLatin1(entry.format).leftJustified(6);
        if(!have)
            out() << " -- provided by " << entry.source;
        out() << "\n";
    }

    out().flush();
    QCoreApplication::quit();
}
