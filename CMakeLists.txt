# ============================================================================
#  qimgv — fast, configurable image viewer
#  https://github.com/duckautomata/qimgv
#
#  Quick start:
#      cmake --preset dev && cmake --build --preset dev
#  See docs/BUILDING.md for dependencies and platform notes.
# ============================================================================
cmake_minimum_required(VERSION 3.21)

project(qimgv
    VERSION 2.0.0
    DESCRIPTION "Fast and configurable image viewer with optional video support"
    HOMEPAGE_URL "https://github.com/duckautomata/qimgv"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectDefaults)
include(CompilerWarnings)
include(PkgConfigFixup)
include(FeatureSummary)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(QIMGV_VIDEO_SUPPORT "Enable video playback via libmpv"          ON)
option(QIMGV_EXIV2         "Read/write EXIF metadata via exiv2"        ON)
option(QIMGV_OPENCV        "High quality scaling via OpenCV"           ON)
option(QIMGV_KDE_BLUR      "Enable KDE/KWin background blur"           OFF)
option(QIMGV_BUILD_TESTS   "Build the unit test suite"                 OFF)
option(QIMGV_LTO           "Enable link-time optimization in Release"  ON)
option(QIMGV_WERROR        "Treat compiler warnings as errors"         OFF)
option(QIMGV_SANITIZE      "Build with ASan+UBSan (debug builds only)" OFF)

# Backwards compatibility with the pre-fork option names, so existing
# packaging scripts (AUR PKGBUILD, distro recipes) keep working.
qimgv_compat_option(VIDEO_SUPPORT  QIMGV_VIDEO_SUPPORT)
qimgv_compat_option(EXIV2          QIMGV_EXIV2)
qimgv_compat_option(OPENCV_SUPPORT QIMGV_OPENCV)
qimgv_compat_option(KDE_SUPPORT    QIMGV_KDE_BLUR)

if(DEFINED USE_QT5)
    message(WARNING
        "USE_QT5 is no longer supported — qimgv requires Qt 6.5 or newer. "
        "The option is ignored.")
endif()

# LTO / linker / sanitizer selection depends on the options above.
qimgv_apply_build_tuning()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport OpenGLWidgets Network)
find_package(Qt6 6.5 QUIET    COMPONENTS LinguistTools)

# Qt 6.3+ can strip unused Qt code and speed up moc; harmless when absent.
qt_standard_project_setup(REQUIRES 6.5)

message(STATUS "Qt version: ${Qt6_VERSION} (${Qt6_DIR})")

# ---------------------------------------------------------------------------
# Optional dependencies
# ---------------------------------------------------------------------------
if(QIMGV_EXIV2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Exiv2 REQUIRED IMPORTED_TARGET exiv2>=0.27)
    qimgv_fix_pkgconfig_paths(PkgConfig::Exiv2)
endif()

if(QIMGV_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc)
endif()

if(QIMGV_KDE_BLUR)
    find_package(KF6WindowSystem REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(qimgv)

if(QIMGV_VIDEO_SUPPORT)
    add_subdirectory(plugins/player_mpv)
endif()

if(QIMGV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(qimgv/tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
add_feature_info("Video playback (libmpv)" QIMGV_VIDEO_SUPPORT "H.264/H.265/AV1/VP9/ProRes via mpv")
add_feature_info("EXIF metadata (exiv2)"   QIMGV_EXIV2         "read/write image metadata")
add_feature_info("HQ scaling (OpenCV)"     QIMGV_OPENCV        "bicubic / bilinear+sharpen filters")
add_feature_info("KDE blur"                QIMGV_KDE_BLUR      "translucent background under KWin")
add_feature_info("Unit tests"              QIMGV_BUILD_TESTS   "ctest suite")
add_feature_info("LTO"                     QIMGV_LTO           "link-time optimization")

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES
                INCLUDE_QUIET_PACKAGES
                DESCRIPTION "\n---- qimgv ${PROJECT_VERSION} build configuration ----\n")

# ---------------------------------------------------------------------------
# Uninstall target (for `cmake --install` users; packages handle their own)
# ---------------------------------------------------------------------------
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
endif()
