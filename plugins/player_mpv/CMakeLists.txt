# ============================================================================
#  player_mpv — video playback plugin backed by libmpv.
#
#  Loaded at runtime by qimgv (see gui/viewers/videoplayerinitproxy.cpp), so
#  it is a MODULE, not a shared library.
# ============================================================================

# --- Find libmpv ------------------------------------------------------------
# pkg-config works on every platform we support, including MSYS2/MinGW on
# Windows. QIMGV_MPV_DIR is an escape hatch for a hand-placed libmpv SDK
# (e.g. the prebuilt Windows package from mpv.io).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(Mpv IMPORTED_TARGET mpv>=2.0)
    # mpv drags in every Requires.private .pc, and some MSYS2 packages hardcode
    # MSYS-style paths a native CMake cannot resolve. See cmake/PkgConfigFixup.cmake.
    qimgv_fix_pkgconfig_paths(PkgConfig::Mpv)
endif()

if(NOT Mpv_FOUND)
    set(QIMGV_MPV_DIR "" CACHE PATH "Path to a libmpv SDK (containing include/ and lib/)")
    if(NOT QIMGV_MPV_DIR AND DEFINED MPV_DIR)
        set(QIMGV_MPV_DIR "${MPV_DIR}")  # legacy name
    endif()
    if(NOT QIMGV_MPV_DIR)
        message(FATAL_ERROR
            "libmpv not found.\n"
            "  Linux:   install libmpv-dev / mpv-devel\n"
            "  MSYS2:   pacman -S mingw-w64-ucrt-x86_64-mpv\n"
            "  macOS:   brew install mpv\n"
            "Or point -DQIMGV_MPV_DIR=<path> at a libmpv SDK, "
            "or disable video with -DQIMGV_VIDEO_SUPPORT=OFF.")
    endif()
    message(STATUS "Using libmpv SDK from ${QIMGV_MPV_DIR}")
endif()

# --- Target -----------------------------------------------------------------
qt_add_library(player_mpv MODULE
    src/videoplayer.cpp
    src/mpvwidget.cpp
    src/videoplayermpv.cpp
)

target_include_directories(player_mpv PRIVATE src)

target_link_libraries(player_mpv PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGLWidgets
    qimgv::warnings
)

if(Mpv_FOUND)
    target_link_libraries(player_mpv PRIVATE PkgConfig::Mpv)
else()
    target_include_directories(player_mpv PRIVATE "${QIMGV_MPV_DIR}/include")
    target_link_directories(player_mpv PRIVATE
        "${QIMGV_MPV_DIR}/lib"
        "${QIMGV_MPV_DIR}/lib/$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,x86_64,i686>")
    target_link_libraries(player_mpv PRIVATE mpv)
endif()

# Only CreatePlayerWidget() is part of the plugin ABI.
target_compile_definitions(player_mpv PRIVATE QIMGV_PLAYER_MPV_LIBRARY)

set_target_properties(player_mpv PROPERTIES
    PREFIX ""
    # qimgv looks for the plugin in a "plugins" dir next to the exe on Windows.
    LIBRARY_OUTPUT_DIRECTORY
        "$<IF:$<BOOL:${WIN32}>,${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins,${CMAKE_RUNTIME_OUTPUT_DIRECTORY}>"
)

# --- Install ----------------------------------------------------------------
include(GNUInstallDirs)
if(WIN32)
    install(TARGETS player_mpv LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins")
elseif(NOT APPLE)
    install(TARGETS player_mpv LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/qimgv")
endif()
