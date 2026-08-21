# ============================================================================
#  The qimgv application target.
# ============================================================================
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Version — generated from project(VERSION ...) so there is exactly one
# place to bump it.
# ---------------------------------------------------------------------------
configure_file(appversion.cpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/appversion.cpp" @ONLY)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
if(APPLE)
    set(APPLE_ICON_NAME "qimgv.icns")
    set(APPLE_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/distrib/${APPLE_ICON_NAME}")
endif()

qt_add_executable(qimgv
    MANUAL_FINALIZATION
    "${CMAKE_CURRENT_BINARY_DIR}/appversion.cpp"
    core.cpp
    main.cpp
    settings.cpp
    themestore.cpp
    sharedresources.cpp
    shortcutbuilder.cpp
    proxystyle.cpp
    macosapplication.cpp
    resources.qrc
    qimgv.rc
    ${APPLE_ICON_PATH}
)

# Source lists live next to the code they describe.
add_subdirectory(components)
add_subdirectory(sourcecontainers)
add_subdirectory(gui)
add_subdirectory(utils)
if(QIMGV_OPENCV)
    add_subdirectory(3rdparty/QtOpenCV)
endif()

target_include_directories(qimgv PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(qimgv PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Svg
    Qt6::PrintSupport
    Qt6::OpenGLWidgets
    Qt6::Network
    qimgv::warnings
)

# ---------------------------------------------------------------------------
# Optional features
# ---------------------------------------------------------------------------
if(QIMGV_EXIV2)
    target_link_libraries(qimgv PRIVATE PkgConfig::Exiv2)
    target_compile_definitions(qimgv PRIVATE USE_EXIV2)
endif()

if(QIMGV_OPENCV)
    target_link_libraries(qimgv PRIVATE opencv_core opencv_imgproc)
    target_include_directories(qimgv PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_compile_definitions(qimgv PRIVATE USE_OPENCV)
endif()

if(QIMGV_KDE_BLUR)
    target_link_libraries(qimgv PRIVATE KF6::WindowSystem)
    target_compile_definitions(qimgv PRIVATE USE_KDE_BLUR)
endif()

if(QIMGV_VIDEO_SUPPORT)
    target_compile_definitions(qimgv PRIVATE USE_MPV)
    if(WIN32)
        target_compile_definitions(qimgv PRIVATE
            _QIMGV_PLUGIN_DIR="plugins"
            _QIMGV_PLAYER_PLUGIN="player_mpv.dll")
    elseif(APPLE)
        target_compile_definitions(qimgv PRIVATE
            _QIMGV_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/qimgv"
            _QIMGV_PLAYER_PLUGIN="player_mpv.dylib")
    else()
        target_compile_definitions(qimgv PRIVATE
            _QIMGV_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/qimgv"
            _QIMGV_PLAYER_PLUGIN="player_mpv.so")
    endif()
endif()

# ---------------------------------------------------------------------------
# Translations
#
# .qm files are written next to the executable so that a build-tree run picks
# them up via the applicationDirPath() fallback in core.cpp, and installed to
# the shared data dir for a real install.
#
# To add a language: create res/translations/<locale>.ts, list it below, then
#   cmake --build <dir> --target qimgv_lupdate
# and translate with Qt Linguist.
# ---------------------------------------------------------------------------
set(QIMGV_TS_FILES
    de_DE.ts
    es_ES.ts
    fr_FR.ts
    ja_JP.ts
    tr_TR.ts
    uk_UA.ts
    zh_CN.ts
)
list(TRANSFORM QIMGV_TS_FILES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/res/translations/")

if(Qt6LinguistTools_FOUND)
    set(QIMGV_QM_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations")
    set_source_files_properties(${QIMGV_TS_FILES}
        PROPERTIES OUTPUT_LOCATION "${QIMGV_QM_DIR}")

    qt_add_lupdate(qimgv TS_FILES ${QIMGV_TS_FILES})
    qt_add_lrelease(qimgv TS_FILES ${QIMGV_TS_FILES})
else()
    message(WARNING "Qt6 LinguistTools not found — translations will not be built.")
endif()

# ---------------------------------------------------------------------------
# Platform specifics
# ---------------------------------------------------------------------------
if(WIN32)
    set_target_properties(qimgv PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(qimgv PRIVATE "/ENTRY:mainCRTStartup")
    endif()
elseif(APPLE)
    set_target_properties(qimgv PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "qimgv"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.duckautomata.qimgv"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/distrib/MyBundleInfo.plist.in"
        MACOSX_BUNDLE_ICON_FILE "${APPLE_ICON_NAME}"
    )
    set_source_files_properties(${APPLE_ICON_PATH}
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_finalize_executable(qimgv)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS qimgv
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(NOT WIN32 AND NOT APPLE)
    install(FILES distrib/qimgv.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")

    install(FILES distrib/qimgv.appdata.xml
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo")

    foreach(_icon IN ITEMS 16x16 22x22 24x24 32x32 36x36 48x48 64x64 128x128 256x256)
        install(FILES "distrib/hicolor/${_icon}/apps/qimgv.png"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${_icon}/apps")
    endforeach()
    install(FILES distrib/hicolor/scalable/apps/qimgv.svg
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps")
endif()

if(Qt6LinguistTools_FOUND)
    if(WIN32)
        install(DIRECTORY "${QIMGV_QM_DIR}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}")
    elseif(NOT APPLE)
        target_compile_definitions(qimgv PRIVATE
            TRANSLATIONS_PATH="${CMAKE_INSTALL_FULL_DATAROOTDIR}/qimgv/translations")
        install(DIRECTORY "${QIMGV_QM_DIR}"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/qimgv")
    endif()
endif()
