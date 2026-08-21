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
    "${CMAKE_SOURCE_DIR}/qimgv/settings.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/themestore.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/stuff.cpp"
    "${CMAKE_SOURCE_DIR}/qimgv/utils/script.cpp"
)

target_include_directories(qimgv_format_tests PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/qimgv"
)

target_compile_definitions(qimgv_format_tests PRIVATE
    QIMGV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

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
