#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QEvent>
#include <clocale>

#include "appversion.h"
#include "settings.h"
#include "components/actionmanager/actionmanager.h"
#include "utils/inputmap.h"
#include "utils/actions.h"
#include "utils/cmdoptionsrunner.h"
#include "sharedresources.h"
#include "proxystyle.h"
#include "core.h"
#include "components/directorymanager/directoryscanner.h"

#ifdef USE_EXIV2
    #include <exiv2/exiv2.hpp>
#endif

#ifdef __APPLE__
#include "macosapplication.h"
#endif

//------------------------------------------------------------------------------
void saveSettings() {
    delete settings;
#ifdef USE_EXIV2
    // Pairs with the initialize() in main(). Registered through atexit, so this
    // runs after the pools have been torn down and nothing can still be decoding.
    Exiv2::XmpParser::terminate();
#endif
}
//------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // force some env variables

#ifdef _WIN32
    // if this is set by other app, platform plugin may fail to load
    // https://github.com/easymodo/qimgv/issues/410 (upstream)
    qputenv("QT_PLUGIN_PATH", "");

    // Put the CRT's narrow-string functions in UTF-8 mode. exiv2 >= 0.28 has
    // no wide-path API left, so DocumentInfo::loadExifTags() has to hand it a
    // narrow path -- and the CRT would otherwise decode that as the ANSI
    // codepage and fail on any name outside it. Set before any thread starts;
    // setlocale mutates process-global state. LC_CTYPE only, so LC_NUMERIC
    // keeps the C locale and number parsing is unaffected.
    std::setlocale(LC_CTYPE, ".UTF8");
#endif

#ifdef USE_EXIV2
    // Exiv2::XmpParser guards its own initialisation with a plain bool and no
    // lock, and its header says outright that initialize() "is not thread-safe
    // and needs to be called in a thread-safe manner (e.g., on program
    // startup)". Metadata extraction runs on a worker, and DocumentInfo is
    // already built on the loader and thumbnailer pools, so two threads can
    // reach the first XMP decode at once. Doing it here, before any thread
    // exists, is the whole fix.
    //
    // Not theoretical: with four threads racing the first decode, a stress
    // harness saw 6 failures in 30 runs -- one segfault and five hangs, the
    // hangs being the worse outcome since the pool thread never comes back.
    // With this call first, 30/30 clean.
    Exiv2::XmpParser::initialize();
#endif

    // for hidpi testing
    // qputenv("QT_SCALE_FACTOR","1.5");
    // qputenv("QT_SCREEN_SCALE_FACTORS", "1;1.7");

    // do we still need this?
    qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0");

    // Qt6 hidpi rendering on windows still has artifacts
    // This disables it for scale factors < 1.75
    // In this case only fonts are scaled
#ifdef _WIN32
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
#endif

    // qDebug() << qgetenv("QT_SCALE_FACTOR");
    // qDebug() << qgetenv("QT_SCREEN_SCALE_FACTORS");
    // qDebug() << qgetenv("QT_ENABLE_HIGHDPI_SCALING");

#ifdef __APPLE__
    MacOSApplication a(argc, argv);
    // default to "fusion" if available ("macos" has layout bugs, weird comboboxes etc)
    if(QStyleFactory::keys().contains("Fusion"))
        a.setStyle(QStyleFactory::create("Fusion"));
#else
    QApplication a(argc, argv);
    // use some style workarounds
    a.setStyle(new ProxyStyle);
#endif

    QCoreApplication::setOrganizationName("qimgv");
    QCoreApplication::setOrganizationDomain("github.com/duckautomata/qimgv");
    QCoreApplication::setApplicationName("qimgv");
    QCoreApplication::setApplicationVersion(appVersion.toString());
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QGuiApplication::setDesktopFileName(QCoreApplication::applicationName() + ".desktop");

    // needed for mpv
#ifndef _MSC_VER
    setlocale(LC_NUMERIC, "C");
#endif

#ifdef __GLIBC__
    // default value of 128k causes memory fragmentation issues
    mallopt(M_MMAP_THRESHOLD, 64000);
#endif

    // use custom types in signals
    qRegisterMetaType<ScalerRequest>("ScalerRequest");
    qRegisterMetaType<Script>("Script");
    qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");
    qRegisterMetaType<std::shared_ptr<Thumbnail>>("std::shared_ptr<Thumbnail>");
    qRegisterMetaType<std::shared_ptr<DirectoryScanResult>>("std::shared_ptr<DirectoryScanResult>");

    // globals
    inputMap = InputMap::getInstance();
    appActions = Actions::getInstance();
    settings = Settings::getInstance();
    scriptManager = ScriptManager::getInstance();
    actionManager = ActionManager::getInstance();
    shrRes = SharedResources::getInstance();

    atexit(saveSettings);

    // parse args ------------------------------------------------------------------
    QCommandLineParser parser;
    QString appDescription = qApp->applicationName() + " - Fast and configurable image viewer.";
    appDescription.append("\nVersion: " + qApp->applicationVersion());
    appDescription.append("\nLicense: GNU GPLv3");
    parser.setApplicationDescription(appDescription);
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", QCoreApplication::translate("main", "File or directory path."));
    parser.addOptions({
        {"gen-thumbs", QCoreApplication::translate("main", "Generate all thumbnails for directory."),
         QCoreApplication::translate("main", "directory-path")},
        {"gen-thumbs-size",
         QCoreApplication::translate("main", "Thumbnail size. Current size is used if not specified."),
         QCoreApplication::translate("main", "thumbnail-size")},
        {"build-options", QCoreApplication::translate("main", "Show build options.")},
    });
    parser.process(a);

    if(parser.isSet("build-options")) {
        CmdOptionsRunner r;
        QTimer::singleShot(0, &r, &CmdOptionsRunner::showBuildOptions);
        return a.exec();
    } else if(parser.isSet("gen-thumbs")) {
        int size = settings->folderViewIconSize();
        if(parser.isSet("gen-thumbs-size"))
            size = parser.value("gen-thumbs-size").toInt();

        CmdOptionsRunner r;
        QTimer::singleShot(0, &r, std::bind(&CmdOptionsRunner::generateThumbs, &r, parser.value("gen-thumbs"), size));
        return a.exec();
    }

    // -----------------------------------------------------------------------------

    Core core;

#ifdef __APPLE__
    QObject::connect(&a, &MacOSApplication::fileOpened, &core, &Core::loadPath);
#endif

    if(parser.positionalArguments().count())
        core.loadPath(parser.positionalArguments().at(0));
    else if(settings->defaultViewMode() == MODE_FOLDERVIEW)
        core.loadPath(QDir::homePath());

    // wait for event queue to catch up before showing window
    // this avoids white background flicker on windows (or not?)
    qApp->processEvents();

    core.showGui();
    return a.exec();
}
