# ============================================================================
#  Unit tests. Enabled with -DQIMGV_BUILD_TESTS=ON (on by default in the
#  `dev` and `ci` presets). Run with: ctest --preset dev
# ============================================================================
find_package(Qt6 6.5 REQUIRED COMPONENTS Test Widgets)

# ---------------------------------------------------------------------------
#  Environment every test needs.
#
#  QT_QPA_PLATFORM  GUI tests need a display; use the offscreen plugin.
#  QT_FORCE_STDERR_LOGGING
#      On Windows, Qt sends test output to OutputDebugString instead of the
#      console whenever stderr is not a tty -- which is always the case under
#      ctest. Without this, a failing test reports nothing but an exit code
#      and --output-on-failure prints an empty block. Harmless elsewhere.
# ---------------------------------------------------------------------------
set(QIMGV_TEST_ENV "QT_QPA_PLATFORM=offscreen" "QT_FORCE_STDERR_LOGGING=1")

qt_add_executable(qimgv_tests
    test_mapoverlay.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/gui/overlays/mapoverlay.cpp"
)

target_include_directories(qimgv_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_link_libraries(qimgv_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    qimgv::warnings
)

add_test(NAME qimgv_tests COMMAND qimgv_tests)
set_tests_properties(qimgv_tests PROPERTIES ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Format detection tests. These link the real DocumentInfo so the sniffers
# are exercised as shipped, not as a copy.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_format_tests
    test_formatdetect.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/documentinfo.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/mediaprobe.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
)

target_include_directories(qimgv_format_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

# USE_MPV turns on Settings::videoPlayback() and audioPlayback(), without which
# no file is ever detected as video or audio. Nothing here links libmpv.
target_compile_definitions(qimgv_format_tests PRIVATE
    QIMGV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    USE_MPV)

target_link_libraries(qimgv_format_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    Qt6::Svg
    qimgv::warnings
)

if(QIMGV_EXIV2)
    target_link_libraries(qimgv_format_tests PRIVATE PkgConfig::Exiv2)
    target_compile_definitions(qimgv_format_tests PRIVATE USE_EXIV2)
endif()

add_test(NAME qimgv_format_tests COMMAND qimgv_format_tests)
set_tests_properties(qimgv_format_tests PROPERTIES ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# DirectoryManager. Covers the path -> index cache: a missed invalidation does
# not crash, it silently returns another file's index, so every list-mutating
# path is exercised and checked against filePathAt().
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_directory_tests
    test_directorymanager.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/directorymanager.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/directoryscanner.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/directorywatcher.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/watcherevent.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/watcherworker.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/dummywatcher.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/fsentry.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
)

if(WIN32)
    target_sources(qimgv_directory_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/windows/windowswatcher.cpp"
        "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/windows/windowsworker.cpp")
elseif(UNIX AND NOT APPLE)
    target_sources(qimgv_directory_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/linux/linuxwatcher.cpp"
        "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/linux/linuxworker.cpp"
        "${CMAKE_SOURCE_DIR}/qimgv/components/directorymanager/watchers/linux/linuxfsevent.cpp")
endif()

target_include_directories(qimgv_directory_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_link_libraries(qimgv_directory_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    qimgv::warnings
)

add_test(NAME qimgv_directory_tests COMMAND qimgv_directory_tests)
set_tests_properties(qimgv_directory_tests PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Update check. Only the tag -> version step is unit-testable; the rest of
# UpdateChecker is network I/O. It drags in settings because the reply handler
# records the last check time, hence the extra translation units.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_update_tests
    test_updatechecker.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/components/updatechecker.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
    "${CMAKE_BINARY_DIR}/qimgv/appversion.cpp"
)

target_include_directories(qimgv_update_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_link_libraries(qimgv_update_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    Qt6::Network
    qimgv::warnings
)

add_test(NAME qimgv_update_tests COMMAND qimgv_update_tests)
set_tests_properties(qimgv_update_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Randomizer regression tests (shuffle-mode hang on 0/1-image directories).
# TIMEOUT is deliberate: if the hang regresses we want a failure, not a
# wedged CI job.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_randomizer_tests
    test_randomizer.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/utils/randomizer.cpp"
)
target_include_directories(qimgv_randomizer_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/qimgv")
target_link_libraries(qimgv_randomizer_tests PRIVATE
    Qt6::Test qimgv::warnings)

add_test(NAME qimgv_randomizer_tests COMMAND qimgv_randomizer_tests)
set_tests_properties(qimgv_randomizer_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Video zoom: the pure VideoZoomModel, the mpv plugin's header-only helpers
# (mpvOptionsFor, mpvDisplaySize, StaleFrameCover) and the VideoZoom
# controller driven through a fake VideoPlayer with synthetic mouse and wheel
# events. No libmpv and no settings.cpp: videozoommodel.h only needs the enums
# from settings.h.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_videozoom_tests
    test_videozoom.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/videoplayer.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/videozoom.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/videozoommodel.cpp"
)

target_include_directories(qimgv_videozoom_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers"
    "${CMAKE_SOURCE_DIR}/plugins/player_mpv/src"
)

target_link_libraries(qimgv_videozoom_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    qimgv::warnings
)

add_test(NAME qimgv_videozoom_tests COMMAND qimgv_videozoom_tests)
set_tests_properties(qimgv_videozoom_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Audio tags and cover art. The parsers are pure functions over a bounded byte
# source, tested against real ffmpeg-made files in data/audio (regenerate with
# data/audio/make-fixtures.sh) and hand-built byte sequences for what ffmpeg
# cannot write. Split in three: two groups of parsers, then the reader that
# dispatches between them, sidecar covers, and robustness against corrupt
# input. No libmpv.
# ---------------------------------------------------------------------------
set(QIMGV_AUDIOMETA_DIR "${CMAKE_SOURCE_DIR}/qimgv/components/audiometa")

function(qimgv_add_audiometa_test name)
    qt_add_executable(${name} ${ARGN} "${QIMGV_AUDIOMETA_DIR}/audiometa_p.cpp")
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/qimgv"
        "${QIMGV_AUDIOMETA_DIR}")
    target_compile_definitions(${name} PRIVATE
        QIMGV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Gui qimgv::warnings)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "${QIMGV_TEST_ENV}")
endfunction()

# ID3v2/ID3v1, APEv2, RIFF/AIFF/DSF, Matroska.
qimgv_add_audiometa_test(qimgv_audiometa_a_tests
    test_audiometa_a.cpp
    "${QIMGV_AUDIOMETA_DIR}/id3.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ape.cpp"
    "${QIMGV_AUDIOMETA_DIR}/riff.cpp"
    "${QIMGV_AUDIOMETA_DIR}/matroska.cpp")

# FLAC, Vorbis comments, Ogg, MP4, ASF.
qimgv_add_audiometa_test(qimgv_audiometa_b_tests
    test_audiometa_b.cpp
    "${QIMGV_AUDIOMETA_DIR}/flac.cpp"
    "${QIMGV_AUDIOMETA_DIR}/vorbiscomment.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ogg.cpp"
    "${QIMGV_AUDIOMETA_DIR}/mp4.cpp"
    "${QIMGV_AUDIOMETA_DIR}/asf.cpp")

# The whole reader, as the app uses it.
qimgv_add_audiometa_test(qimgv_audiometa_tests
    test_audiometa.cpp
    "${QIMGV_AUDIOMETA_DIR}/audiometadata.cpp"
    "${QIMGV_AUDIOMETA_DIR}/id3.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ape.cpp"
    "${QIMGV_AUDIOMETA_DIR}/riff.cpp"
    "${QIMGV_AUDIOMETA_DIR}/matroska.cpp"
    "${QIMGV_AUDIOMETA_DIR}/flac.cpp"
    "${QIMGV_AUDIOMETA_DIR}/vorbiscomment.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ogg.cpp"
    "${QIMGV_AUDIOMETA_DIR}/mp4.cpp"
    "${QIMGV_AUDIOMETA_DIR}/asf.cpp")

# ---------------------------------------------------------------------------
# The Shuffle playback mode's order: rounds without repeats, history, and a
# pool that changes under it. Pure logic, seeded RNG.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_audioshuffle_tests
    test_audioshuffle.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/utils/audioshuffle.cpp"
)
target_include_directories(qimgv_audioshuffle_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/qimgv")
target_link_libraries(qimgv_audioshuffle_tests PRIVATE
    Qt6::Test qimgv::warnings)

add_test(NAME qimgv_audioshuffle_tests COMMAND qimgv_audioshuffle_tests)
set_tests_properties(qimgv_audioshuffle_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Audio in the thumbnailer and the file info panel: cover art sized and
# cropped like an image file, the placeholder for a file without any, the
# format label, and the I-key sections. Real detection and the real tag
# reader. USE_MPV only turns audio detection on; nothing here links libmpv,
# and audio must never start the mpv binary the video thumbnails use.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_audiothumbs_tests
    test_audiothumbs.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/components/thumbnailer/thumbnailerrunnable.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/cache/thumbnailcache.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/fileinfo/fileinfoextractor.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/thumbnail.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/documentinfo.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/mediaprobe.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/imagelib.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
    "${QIMGV_AUDIOMETA_DIR}/audiometa_p.cpp"
    "${QIMGV_AUDIOMETA_DIR}/audiometadata.cpp"
    "${QIMGV_AUDIOMETA_DIR}/id3.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ape.cpp"
    "${QIMGV_AUDIOMETA_DIR}/riff.cpp"
    "${QIMGV_AUDIOMETA_DIR}/matroska.cpp"
    "${QIMGV_AUDIOMETA_DIR}/flac.cpp"
    "${QIMGV_AUDIOMETA_DIR}/vorbiscomment.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ogg.cpp"
    "${QIMGV_AUDIOMETA_DIR}/mp4.cpp"
    "${QIMGV_AUDIOMETA_DIR}/asf.cpp"
)

target_include_directories(qimgv_audiothumbs_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

# No USE_OPENCV: ImageLib only needs OpenCV for the viewer's scaling filters,
# which thumbnails never use.
target_compile_definitions(qimgv_audiothumbs_tests PRIVATE
    QIMGV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    USE_MPV)

target_link_libraries(qimgv_audiothumbs_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    Qt6::Svg
    qimgv::warnings
)

# With exiv2 in, "no Exif/IPTC/XMP section for audio" is a real check rather
# than a given.
if(QIMGV_EXIV2)
    target_link_libraries(qimgv_audiothumbs_tests PRIVATE PkgConfig::Exiv2)
    target_compile_definitions(qimgv_audiothumbs_tests PRIVATE USE_EXIV2)
endif()

add_test(NAME qimgv_audiothumbs_tests COMMAND qimgv_audiothumbs_tests)
set_tests_properties(qimgv_audiothumbs_tests PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# The Play folder playback mode: which files make up a folder's audio, and
# where next/previous land. Pure logic over file paths.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_audionavigation_tests
    test_audionavigation.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/utils/audionavigation.cpp"
)
target_include_directories(qimgv_audionavigation_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/qimgv")
target_link_libraries(qimgv_audionavigation_tests PRIVATE
    Qt6::Test qimgv::warnings)

add_test(NAME qimgv_audionavigation_tests COMMAND qimgv_audionavigation_tests)
set_tests_properties(qimgv_audionavigation_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# The audio view: layout at every size, what it shows for the player's
# reports, and that it keeps focus and unused mouse input out of its way. A
# scripted AudioPlayer stands in for the plugin; one test plays a fixture
# through the real plugin, loaded by path, when the build has it. USE_MPV
# without _QIMGV_PLAYER_PLUGIN: the view's own lookup has no file name to look
# for, so the missing-plugin state is testable too.
# QIMGV_AUDIOVIEW_RENDER_DIR=<dir> keeps a PNG of every layout.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_audioview_tests
    test_audioview.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/resources.qrc"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/audioview.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/audioplayer.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/viewers/playerplugin.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/customwidgets/mediaslider.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/customwidgets/actionbutton.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/customwidgets/iconbutton.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/gui/customwidgets/iconwidget.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/actionmanager/actionmanager.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/scriptmanager/scriptmanager.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/shortcutbuilder.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/actions.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/inputmap.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/imagelib.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/image.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/documentinfo.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/mediaprobe.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
    "${QIMGV_AUDIOMETA_DIR}/audiometadata.cpp"
    "${QIMGV_AUDIOMETA_DIR}/audiometa_p.cpp"
    "${QIMGV_AUDIOMETA_DIR}/id3.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ape.cpp"
    "${QIMGV_AUDIOMETA_DIR}/riff.cpp"
    "${QIMGV_AUDIOMETA_DIR}/matroska.cpp"
    "${QIMGV_AUDIOMETA_DIR}/flac.cpp"
    "${QIMGV_AUDIOMETA_DIR}/vorbiscomment.cpp"
    "${QIMGV_AUDIOMETA_DIR}/ogg.cpp"
    "${QIMGV_AUDIOMETA_DIR}/mp4.cpp"
    "${QIMGV_AUDIOMETA_DIR}/asf.cpp"
)

target_include_directories(qimgv_audioview_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_compile_definitions(qimgv_audioview_tests PRIVATE
    QIMGV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    USE_MPV)

target_link_libraries(qimgv_audioview_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    Qt6::Svg
    qimgv::warnings
)

if(TARGET player_mpv)
    add_dependencies(qimgv_audioview_tests player_mpv)
    target_compile_definitions(qimgv_audioview_tests PRIVATE
        QIMGV_TEST_PLAYER_PLUGIN="$<TARGET_FILE:player_mpv>")
endif()

add_test(NAME qimgv_audioview_tests COMMAND qimgv_audioview_tests)
set_tests_properties(qimgv_audioview_tests PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "${QIMGV_TEST_ENV}")

# ---------------------------------------------------------------------------
# Media and volume keys: their names on every platform, and when qimgv takes
# one rather than passing it on to the system. The real ActionManager, hence
# settings and the script manager behind it.
# ---------------------------------------------------------------------------
qt_add_executable(qimgv_shortcutbuilder_tests
    test_shortcutbuilder.cpp
    "${CMAKE_SOURCE_DIR}/qimgv/shortcutbuilder.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/inputmap.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/actions.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/actionmanager/actionmanager.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/components/scriptmanager/scriptmanager.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/image.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/documentinfo.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/sourcecontainers/mediaprobe.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
)

target_include_directories(qimgv_shortcutbuilder_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_link_libraries(qimgv_shortcutbuilder_tests PRIVATE
    Qt6::Test
    Qt6::Widgets
    qimgv::warnings
)

add_test(NAME qimgv_shortcutbuilder_tests COMMAND qimgv_shortcutbuilder_tests)
set_tests_properties(qimgv_shortcutbuilder_tests PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "${QIMGV_TEST_ENV}")
